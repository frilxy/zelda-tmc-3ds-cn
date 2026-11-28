#include "title_rays_3ds.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The method is tools/scene-extend/extend_rays.py's (that script shows the result on the PC):
 * each ray (8-connected run of BG0 pixels) has to repeat one row down and one column left; each
 * of its diagonals cut by the picture's top row or last column is continued up the slope to the
 * view's edge by repeating a stretch of the ray's upper half on that diagonal. Original pixels and
 * whole rays are never touched. The gaps left at the sides get copies of whole rays (kNewRays). */

enum {
    SRC_W = 256,
    VIEW_W = 400,
    VIEW_H = 240,
    MAP_W = 64, /* tiles, 512x256 */
    MAP_H = 32,
    MAX_TILES = 128,
    MIN_RAY_PX = 20,
};

static uint8_t* sSourceKey; /* the source map and the character data it uses, as last built from */
static size_t sSourceKeySize;
static uint16_t sMap[MAP_W * MAP_H]; /* screen blocks 26 and 27, block-major as VRAM holds them */
static uint8_t sTiles[MAX_TILES * 32];
static unsigned sTileCount, sFirstTile;
static bool sBuilt;

static uint16_t MapEntry(const uint8_t* vram, int screen, int tx, int ty) {
    const uint8_t* p = vram + screen * 0x800 + (ty * 32 + tx) * 2;
    return (uint16_t)(p[0] | (p[1] << 8));
}

static unsigned MaxTileUsed(const uint8_t* vram, int screen) {
    unsigned max = 0;
    for (int i = 0; i < 32 * 32; i++) {
        const unsigned t = MapEntry(vram, screen, i % 32, i / 32) & 0x3ffu;
        if (t > max) max = t;
    }
    return max;
}

/* The source as bytes to compare: BG0's map, then the character data up to its last tile. */
static size_t SourceKey(const uint8_t* vram, uint8_t* out) {
    const size_t mapBytes = 0x800, tileBytes = (MaxTileUsed(vram, TITLE_RAYS_SOURCE_SCREEN) + 1u) * 32u;
    if (out) {
        memcpy(out, vram + TITLE_RAYS_SOURCE_SCREEN * 0x800, mapBytes);
        memcpy(out + mapBytes, vram, tileBytes);
    }
    return mapBytes + tileBytes;
}

/* The new rays: a copy of ray `ray` (numbered by its first pixel in reading order, as
 * extend_rays.py prints them) moved by dx, dy in the view. Chosen with `extend_rays.py --suggest`
 * (`--partial` for one cut by the view's edge) and checked with `--add`
 * (docs/wide-scene-extension.md). */
typedef struct {
    int ray, dx, dy;
} NewRay;
static const NewRay kNewRays[] = {
    {1, -84, 8},    /* the left gap: a wide ray in from the top, its end at mid height */
    {7, 86, 8},     /* the lower right corner */
    {7, -351, -40}, /* a wide one in the top left corner, mostly past the view's edges */
    {2, -164, -25}, /* a thin one between, nearer the corner: the gaps are not even */
};

typedef struct {
    const uint8_t* idx;
    const int16_t* lab;
    uint8_t* view;
    int height;
    const int* stretch; /* per ray: the stretch repeated up its cut diagonals, 0 not a ray */
} Rays;

/* Diagonal u of ray k: when the picture cuts it (its top pixel on the top row or the last
 * column), the run of the ray's pixels down from there; else 0. *top gets its top pixel's v. */
static int CutRun(const Rays* r, int k, int u, int* top) {
    int v0 = -1;
    for (int v = 0; v < r->height && v0 < 0; v++)
        if (u - v >= 0 && u - v < SRC_W && r->lab[v * SRC_W + u - v] == k) v0 = v;
    if (top) *top = v0;
    if (v0 < 0 || (v0 != 0 && u - v0 != SRC_W - 1)) return 0;
    int run = 0;
    while (v0 + run < r->height && u - v0 - run >= 0 && r->lab[(v0 + run) * SRC_W + u - v0 - run] == k) run++;
    return run;
}

/* Ray k drawn into the view moved by dx, dy. copy false: only its continuation past the cut
 * (dx = dy = 0), outside the original's area. copy true: the whole ray with its continuation,
 * drawn only if none of it falls on the original's area or another ray. */
static bool DrawRay(const Rays* r, int k, int dx, int dy, bool copy) {
    for (int pass = copy ? 0 : 1; pass < 2; pass++) { /* a copy: check, then draw */
        for (int u = 0; u < SRC_W + r->height; u++) {
            int v0, v1 = -1;
            const int run = CutRun(r, k, u, &v0);
            if (v0 < 0) continue;
            for (int v = v0; v < r->height && u - v >= 0; v++)
                if (r->lab[v * SRC_W + u - v] == k) v1 = v;
            const int seg = run < r->stretch[k] ? run : r->stretch[k];
            for (int v = copy ? v1 : v0 - 1;; v--) {
                uint8_t i;
                if (v >= v0) {
                    i = r->lab[v * SRC_W + u - v] == k ? r->idx[v * SRC_W + u - v] : 0;
                } else {
                    if (!run) break; /* the ray's own end */
                    const int t = ((v - v0) % seg + seg) % seg;
                    i = r->idx[(v0 + t) * SRC_W + u - v0 - t];
                }
                const int vy = v + TITLE_RAYS_TOP + dy, vx = u - v + TITLE_RAYS_LEFT + dx;
                if (vy < 0 || vx >= VIEW_W) break;
                if (vx < 0 || vy >= VIEW_H || !i) continue;
                const bool inside = vy >= TITLE_RAYS_TOP && vy < TITLE_RAYS_TOP + r->height &&
                                    vx >= TITLE_RAYS_LEFT && vx < TITLE_RAYS_LEFT + SRC_W;
                if (pass == 0 && (inside || r->view[vy * VIEW_W + vx])) return false;
                if (pass == 1 && !inside) r->view[vy * VIEW_W + vx] = i;
            }
        }
    }
    return true;
}

static bool Build(const uint8_t* vram) {
    bool ok = false;
    int bank = -1, height = 0, comps = 0;
    uint8_t* idx = calloc(SRC_W * 256, 1);
    int16_t* lab = calloc(SRC_W * 256, sizeof(int16_t));
    int32_t* stack = malloc(SRC_W * 256 * sizeof(int32_t));
    uint8_t* view = calloc(VIEW_W * VIEW_H, 1);
    int* runs = malloc((SRC_W + 256) * sizeof(int));
    int* rayStretch = NULL;
    int16_t* ours = calloc(MAP_W * MAP_H, sizeof(int16_t)); /* map cells of new tiles: tile + 1 */
    if (!idx || !lab || !stack || !view || !runs || !ours) goto done;

    /* 1. BG0 as 4-bit indices, one palette bank. */
    for (int ty = 0; ty < 32; ty++) {
        for (int tx = 0; tx < 32; tx++) {
            const uint16_t e = MapEntry(vram, TITLE_RAYS_SOURCE_SCREEN, tx, ty);
            const unsigned t = e & 0x3ffu, hf = (e >> 10) & 1u, vf = (e >> 11) & 1u;
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    const int sx = hf ? 7 - x : x, sy = vf ? 7 - y : y;
                    const uint8_t b = vram[t * 32u + sy * 4u + sx / 2u];
                    const uint8_t i = (b >> (4 * (sx & 1))) & 15;
                    if (!i) continue;
                    if (bank >= 0 && bank != e >> 12) goto done; /* not the rays */
                    bank = e >> 12;
                    idx[(ty * 8 + y) * SRC_W + tx * 8 + x] = i;
                    if (ty * 8 + y + 1 > height) height = ty * 8 + y + 1;
                }
            }
        }
    }
    height = (height + 7) & ~7;
    if (bank < 0 || TITLE_RAYS_TOP + height > VIEW_H) goto done;

    /* 2. The rays: 8-connected components. */
    for (int p = 0; p < SRC_W * height; p++) {
        if (!idx[p] || lab[p]) continue;
        if (comps == INT16_MAX) goto done;
        int sp = 0;
        lab[p] = (int16_t)++comps;
        stack[sp++] = p;
        while (sp) {
            const int q = stack[--sp], qy = q / SRC_W, qx = q % SRC_W;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const int ny = qy + dy, nx = qx + dx;
                    if (ny < 0 || ny >= height || nx < 0 || nx >= SRC_W) continue;
                    const int n = ny * SRC_W + nx;
                    if (idx[n] && !lab[n]) {
                        lab[n] = (int16_t)comps;
                        stack[sp++] = n;
                    }
                }
            }
        }
    }

    rayStretch = calloc((size_t)comps + 1, sizeof(int));
    if (!rayStretch) goto done;
    const Rays rays = {idx, lab, view, height, rayStretch};
    for (int k = 1; k <= comps; k++) {
        /* Its slope: the step (columns per row down) its pixels repeat at best must be -1. */
        unsigned px = 0, tot[5] = {0}, eq[5] = {0};
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < SRC_W; x++) {
                if (lab[y * SRC_W + x] != k) continue;
                px++;
                if (y + 1 >= height) continue;
                for (int s = -2; s <= 2; s++) {
                    const int x2 = x + s;
                    if (x2 < 0 || x2 >= SRC_W || lab[(y + 1) * SRC_W + x2] != k) continue;
                    tot[s + 2]++;
                    eq[s + 2] += idx[y * SRC_W + x] == idx[(y + 1) * SRC_W + x2];
                }
            }
        }
        if (px < MIN_RAY_PX) continue;
        int best = 0;
        for (int s = 1; s < 5; s++)
            if ((uint64_t)eq[s] * (tot[best] ? tot[best] : 1) > (uint64_t)eq[best] * (tot[s] ? tot[s] : 1)) best = s;
        if (best != 1) {
            fprintf(stderr, "title rays: ray %d has another slope (step %d), left as it is\n", k, best - 2);
            continue;
        }

        /* 3. Its diagonals (x + y = u, v = y along it) cut by the top row or the last column: the
         * stretch repeated up them is half the median of their runs. */
        int cut = 0;
        for (int u = 0; u < SRC_W + height; u++) {
            const int run = CutRun(&rays, k, u, NULL);
            if (run) runs[cut++] = run;
        }
        if (!cut) continue;
        for (int i = 1; i < cut; i++)
            for (int j = i; j > 0 && runs[j - 1] > runs[j]; j--) {
                const int t = runs[j];
                runs[j] = runs[j - 1];
                runs[j - 1] = t;
            }
        rayStretch[k] = runs[cut / 2] / 2 > 1 ? runs[cut / 2] / 2 : 1;
        DrawRay(&rays, k, 0, 0, false);
    }

    /* New rays in the gaps the GBA's screen never showed: copies of whole rays, continued up to
     * the view's edge, where they touch neither the original's area nor another ray. */
    for (size_t i = 0; i < sizeof(kNewRays) / sizeof(kNewRays[0]); i++) {
        const NewRay* n = &kNewRays[i];
        if (n->ray > comps || !rayStretch[n->ray] || !DrawRay(&rays, n->ray, n->dx, n->dy, true))
            fprintf(stderr, "title rays: new ray %d:%d,%d does not fit, left out\n", n->ray, n->dx, n->dy);
    }

    /* 4. Tiles and map: the original's entries in the middle, the extension's own tiles around. */
    memset(sMap, 0, sizeof(sMap));
    sTileCount = 0;
    for (int ty = 0; ty < VIEW_H / 8; ty++) {
        for (int tx = 0; tx < VIEW_W / 8; tx++) {
            const int ox = tx - TITLE_RAYS_LEFT / 8, oy = ty - TITLE_RAYS_TOP / 8;
            uint16_t entry = 0; /* the game's empty cell */
            if (ox >= 0 && ox < 32 && oy >= 0 && oy < height / 8) {
                entry = MapEntry(vram, TITLE_RAYS_SOURCE_SCREEN, ox, oy);
            } else {
                uint8_t tile[32] = {0};
                bool any = false;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        const uint8_t i = view[(ty * 8 + y) * VIEW_W + tx * 8 + x];
                        tile[y * 4 + x / 2] |= (uint8_t)(i << (4 * (x & 1)));
                        any |= i != 0;
                    }
                if (any) {
                    unsigned n = 0;
                    while (n < sTileCount && memcmp(sTiles + n * 32, tile, 32)) n++;
                    if (n == sTileCount) {
                        if (sTileCount == MAX_TILES) goto done;
                        memcpy(sTiles + sTileCount++ * 32, tile, 32);
                    }
                    ours[(tx / 32) * 1024 + ty * 32 + tx % 32] = (int16_t)(n + 1);
                    continue;
                }
            }
            sMap[(tx / 32) * 1024 + ty * 32 + tx % 32] = entry;
        }
    }
    /* The new tiles go at the top of character block 0, past every tile BG0 and BG1 use. */
    {
        unsigned used = MaxTileUsed(vram, TITLE_RAYS_SOURCE_SCREEN);
        const unsigned forest = MaxTileUsed(vram, TITLE_RAYS_FOREST_SCREEN);
        if (forest > used) used = forest;
        if (sTileCount > 1023u - used) goto done;
        sFirstTile = 1024u - sTileCount;
    }
    for (int i = 0; i < MAP_W * MAP_H; i++)
        if (ours[i]) sMap[i] = (uint16_t)((sFirstTile + (unsigned)ours[i] - 1u) | (unsigned)bank << 12);
    fprintf(stderr, "title rays: %d rays, %u new tiles from %u\n", comps, sTileCount, sFirstTile);
    ok = true;

done:
    free(idx);
    free(lab);
    free(stack);
    free(view);
    free(runs);
    free(ours);
    free(rayStretch);
    return ok;
}

int TitleRays3DS_Update(uint8_t* vram) {
    const size_t keySize = SourceKey(vram, NULL);
    if (sSourceKey == NULL || keySize != sSourceKeySize ||
        memcmp(sSourceKey, vram + TITLE_RAYS_SOURCE_SCREEN * 0x800, 0x800) != 0 ||
        memcmp(sSourceKey + 0x800, vram, keySize - 0x800) != 0) {
        free(sSourceKey);
        sSourceKey = malloc(keySize);
        sSourceKeySize = sSourceKey ? keySize : 0;
        if (sSourceKey) SourceKey(vram, sSourceKey);
        sBuilt = sSourceKey != NULL && Build(vram);
    }
    if (!sBuilt) return 0;
    uint8_t* map = vram + TITLE_RAYS_SCREEN * 0x800;
    uint8_t* tiles = vram + sFirstTile * 32u;
    if (memcmp(map, sMap, sizeof(sMap)) != 0) memcpy(map, sMap, sizeof(sMap));
    if (memcmp(tiles, sTiles, sTileCount * 32u) != 0) memcpy(tiles, sTiles, sTileCount * 32u);
    return TITLE_RAYS_SCREEN;
}
