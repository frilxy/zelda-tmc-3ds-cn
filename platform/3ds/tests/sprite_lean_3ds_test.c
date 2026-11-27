/* Host test of the per-tile stereo depth of a leaning sprite (platform/3ds/source/sprite_lean_3ds.h): rows (the normal
 * rule and PORT_DEPTH_DOWN), columns (PORT_DEPTH_LEFT / PORT_DEPTH_RIGHT), flips, flat, 1xN and Nx1 sprites, the run count
 * in drawing order and the run cap; the eyes' shifts of a side lean (no gap between neighbouring columns, in either eye,
 * at any slider), and PORT_DEPTH_RAISED.
 *
 *   cc -std=c11 -Wall -Wextra -Iplatform/3ds/source platform/3ds/tests/sprite_lean_3ds_test.c -o /tmp/sprite_lean_3ds_test
 *   /tmp/sprite_lean_3ds_test
 */
#include "sprite_lean_3ds.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr);        \
            return 1;                                                      \
        }                                                                  \
    } while (0)

/* A plain (not affine) sprite of cols x rows tiles. */
static SpriteLean3DS plain(int cols, int rows, unsigned nearQ, unsigned farQ, bool columns) {
    SpriteLean3DS s;
    memset(&s, 0, sizeof(s));
    s.x = 40;
    s.y = 30;
    s.width = s.boundsWidth = cols * 8;
    s.height = s.boundsHeight = rows * 8;
    s.depthNear = nearQ;
    s.depthFar = farQ;
    s.columns = columns;
    return s;
}

/* stereo_3ds.c's EyeMove: the disparity (pixels) rounded and split between the eyes, the odd pixel to the left one. */
static int EyeMove(float disparity, bool leftEye) {
    const float magnitude = disparity < 0.0f ? -disparity : disparity;
    const int total = (int)(magnitude + 0.5f);
    const int left = (total + 1) / 2;
    const int move = leftEye ? left : -(total - left);
    return disparity < 0.0f ? -move : move;
}
/* What the renderer moves a batch of `depthQ` quarters by for an eye (port_ppu_gpu_3ds.c, SpritePx), at `slider`. */
static int BatchShift(unsigned depthQ, unsigned side, float slider, bool leftEye) {
    const float disparity = 0.25f * (float)depthQ * slider;
    return SpriteLean3DS_EyeShift(EyeMove(disparity, true), EyeMove(disparity, false), side, leftEye);
}

/* The columns of a row of side-leaning OAM entries drawn next to one another, left to right on the screen: each column's
 * left edge and the depth of its batch. */
typedef struct {
    int left[64];
    unsigned depth[64];
    int count;
} Columns;

/* Appends the columns of an entry `cols` x `rows` tiles at screen x `x` whose depth goes from `rightQ` at its right edge to
 * `leftQ` at its left one, as port_draw.c writes it and the GPU model splits it (SpriteLean3DS_Fill). */
static int AddEntry(Columns* out, int x, int cols, int rows, unsigned rightQ, unsigned leftQ, bool hflip) {
    SpriteLean3DS s;
    memset(&s, 0, sizeof(s));
    s.x = x;
    s.y = 30;
    s.width = s.boundsWidth = cols * 8;
    s.height = s.boundsHeight = rows * 8;
    s.hflip = hflip;
    s.depthNear = rightQ;
    s.depthFar = leftQ;
    s.columns = rightQ != leftQ;
    uint8_t d[SPRITE_LEAN_3DS_MAX_TILES];
    if (SpriteLean3DS_Fill(&s, d) <= 0) return 0;
    for (int screenCol = 0; screenCol < cols; ++screenCol) {
        const int col = hflip ? cols - 1 - screenCol : screenCol; /* the tile data's column drawn there */
        for (int row = 1; row < rows; ++row)
            if (d[row * cols + col] != d[col]) return 0; /* a column lean: one depth down each column */
        out->left[out->count] = x + screenCol * 8;
        out->depth[out->count] = d[col];
        ++out->count;
    }
    return 1;
}

/* The widest gap between neighbouring columns in one eye (0 when they all meet or overlap), and the widest overlap. */
static int Gap(const Columns* c, unsigned side, float slider, bool leftEye, int* overlap) {
    int widest = 0;
    *overlap = 0;
    for (int i = 0; i + 1 < c->count; ++i) {
        const int rightEdge = c->left[i] + 8 + BatchShift(c->depth[i], side, slider, leftEye);
        const int nextLeft = c->left[i + 1] + BatchShift(c->depth[i + 1], side, slider, leftEye);
        if (c->left[i] + 8 != c->left[i + 1]) continue; /* not touching on the screen */
        if (nextLeft - rightEdge > widest) widest = nextLeft - rightEdge;
        if (rightEdge - nextLeft > *overlap) *overlap = rightEdge - nextLeft;
    }
    return widest;
}

/* The same with the normal split (both eyes move, SIDE_NONE): what opened the curtain's seam. */
static int GapNormalSplit(const Columns* c, float slider, bool leftEye) {
    int unused;
    return Gap(c, SPRITE_LEAN_3DS_SIDE_NONE, slider, leftEye, &unused);
}

/* Every slider from -1 to 1 (the sign: out of or into the screen), both eyes: no gap with the side's split, the disparity
 * of every column kept, a column with none left in place, and the overlap no wider than the step of disparity. */
static int CheckNoGap(const Columns* c, unsigned side, int* normalGaps) {
    for (int step = -100; step <= 100; ++step) {
        const float slider = (float)step / 100.0f;
        for (int eye = 0; eye < 2; ++eye) {
            int overlap;
            if (Gap(c, side, slider, eye == 0, &overlap) != 0) {
                fprintf(stderr, "gap: side %u slider %.2f %s eye\n", side, slider, eye == 0 ? "left" : "right");
                return 0;
            }
            if (overlap > 6) return 0; /* never more than the whole disparity of 20 quarters (5 px) and a rounding */
            if (GapNormalSplit(c, slider, eye == 0) != 0) ++*normalGaps;
        }
        for (int i = 0; i < c->count; ++i) {
            const float disparity = 0.25f * (float)c->depth[i] * slider;
            const int l = BatchShift(c->depth[i], side, slider, true), r = BatchShift(c->depth[i], side, slider, false);
            if (l - r != EyeMove(disparity, true) - EyeMove(disparity, false)) return 0; /* the depth seen is kept */
            if (l != 0 && r != 0) return 0;                                                 /* one eye stays */
        }
    }
    return 1;
}

int main(void) {
    uint8_t d[SPRITE_LEAN_3DS_MAX_TILES];

    /* Every tile once, in either order. */
    for (int columns = 0; columns < 2; ++columns) {
        int seen[6 * 4] = {0};
        for (int step = 0; step < 6 * 4; ++step) {
            int row, col;
            SpriteLean3DS_TileAt(6, 4, columns != 0, step, &row, &col);
            CHECK(row >= 0 && row < 6 && col >= 0 && col < 4);
            ++seen[row * 4 + col];
        }
        for (int i = 0; i < 6 * 4; ++i) CHECK(seen[i] == 1);
    }
    {
        int row, col;
        SpriteLean3DS_TileAt(6, 4, true, 7, &row, &col); /* column by column: the 8th tile is column 1, row 1 */
        CHECK(row == 1 && col == 1);
        SpriteLean3DS_TileAt(6, 4, false, 7, &row, &col); /* row by row: row 1, column 3 */
        CHECK(row == 1 && col == 3);
    }

    /* Up (the normal rule): 2 x 4 tiles, 0 at the bottom edge, 8 at the top; each row the depth at its middle. */
    {
        const SpriteLean3DS s = plain(2, 4, 0, 8, false);
        CHECK(SpriteLean3DS_Fill(&s, d) == 4);
        const uint8_t want[4] = {7, 5, 3, 1}; /* top row first */
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 2; ++col) CHECK(d[row * 2 + col] == want[row]);
    }
    /* Down (PORT_DEPTH_DOWN / INVERT): the bottom edge carries the higher depth. */
    {
        const SpriteLean3DS s = plain(2, 4, 8, 0, false);
        CHECK(SpriteLean3DS_Fill(&s, d) == 4);
        const uint8_t want[4] = {1, 3, 5, 7};
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 2; ++col) CHECK(d[row * 2 + col] == want[row]);
    }
    /* Up, flipped vertically: the tile data's top row is drawn at the bottom. */
    {
        SpriteLean3DS s = plain(2, 4, 0, 8, false);
        s.vflip = true;
        CHECK(SpriteLean3DS_Fill(&s, d) == 4);
        const uint8_t want[4] = {1, 3, 5, 7};
        for (int row = 0; row < 4; ++row) CHECK(d[row * 2] == want[row] && d[row * 2 + 1] == want[row]);
    }

    /* Left (PORT_DEPTH_LEFT): 4 x 2 tiles, the right edge at 0 (the ground), the left one at 8. One run per column:
     * drawn row by row it would make 8. */
    {
        const SpriteLean3DS s = plain(4, 2, 0, 8, true);
        CHECK(SpriteLean3DS_Fill(&s, d) == 4);
        const uint8_t want[4] = {7, 5, 3, 1}; /* leftmost column first */
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 4; ++col) CHECK(d[row * 4 + col] == want[col]);
    }
    /* Right (PORT_DEPTH_RIGHT): the left edge at 0, the right one at 8. */
    {
        const SpriteLean3DS s = plain(4, 2, 8, 0, true);
        CHECK(SpriteLean3DS_Fill(&s, d) == 4);
        const uint8_t want[4] = {1, 3, 5, 7};
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 4; ++col) CHECK(d[row * 4 + col] == want[col]);
    }
    /* Left, flipped horizontally: the screen keeps the depth rising to the left, so the tile data's column 0 (drawn at
     * the right) is the lowest. */
    {
        SpriteLean3DS s = plain(4, 2, 0, 8, true);
        s.hflip = true;
        CHECK(SpriteLean3DS_Fill(&s, d) == 4);
        const uint8_t want[4] = {1, 3, 5, 7};
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 4; ++col) CHECK(d[row * 4 + col] == want[col]);
    }
    /* A vertical flip does not change a column lean; a horizontal one does not change a row lean. */
    {
        SpriteLean3DS s = plain(4, 2, 0, 8, true);
        s.vflip = true;
        CHECK(SpriteLean3DS_Fill(&s, d) == 4);
        CHECK(d[0] == 7 && d[3] == 1 && d[4] == 7 && d[7] == 1);
        s = plain(2, 4, 0, 8, false);
        s.hflip = true;
        CHECK(SpriteLean3DS_Fill(&s, d) == 4);
        CHECK(d[0] == 7 && d[1] == 7 && d[6] == 1 && d[7] == 1);
    }

    /* Flat: one depth, one run, whichever axis. */
    for (int columns = 0; columns < 2; ++columns) {
        const SpriteLean3DS s = plain(4, 4, 5, 5, columns != 0);
        CHECK(SpriteLean3DS_Fill(&s, d) == 1);
        for (int i = 0; i < 16; ++i) CHECK(d[i] == 5);
    }

    /* 1 x N and N x 1. A strip one tile wide leaning across its columns takes the middle depth: one run. */
    {
        SpriteLean3DS s = plain(1, 8, 0, 8, true); /* 8 px wide, 64 tall, PORT_DEPTH_LEFT */
        CHECK(SpriteLean3DS_Fill(&s, d) == 1);
        for (int i = 0; i < 8; ++i) CHECK(d[i] == 4);
        s = plain(8, 1, 0, 8, true); /* 64 px wide, 8 tall, PORT_DEPTH_LEFT: 0..7 from the right */
        CHECK(SpriteLean3DS_Fill(&s, d) == 8);
        for (int col = 0; col < 8; ++col) CHECK(d[col] == (uint8_t)(8 * ((7 - col) * 8 + 4) / 64));
        s = plain(8, 1, 0, 8, false); /* the same strip with the normal rule: one row, the middle depth */
        CHECK(SpriteLean3DS_Fill(&s, d) == 1);
        for (int col = 0; col < 8; ++col) CHECK(d[col] == 4);
        s = plain(1, 8, 0, 8, false); /* the tall strip, normal rule */
        CHECK(SpriteLean3DS_Fill(&s, d) == 8);
        for (int row = 0; row < 8; ++row) CHECK(d[row] == (uint8_t)(8 * ((7 - row) * 8 + 4) / 64));
    }

    /* The widest lean (8 x 8 tiles, 0 to 20): 8 runs column by column, under the cap; drawn row by row it would be 64. */
    {
        const SpriteLean3DS s = plain(8, 8, 0, 20, true);
        CHECK(SpriteLean3DS_Fill(&s, d) == 8);
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col) CHECK(d[row * 8 + col] == d[col]);
        CHECK(d[0] > d[7]);
    }

    /* Support is added to every tile; the sum stops at 20 quarters. */
    {
        SpriteLean3DS s = plain(4, 1, 0, 12, true);
        s.support = 12;
        CHECK(SpriteLean3DS_Fill(&s, d) == 4);
        CHECK(d[0] == 20 && d[1] == 19 && d[2] == 16 && d[3] == 13); /* 10, 7, 4, 1 from the lean */
    }

    /* Affine, identity matrix: the column lean follows the screen, highest on the left, one run per column. */
    {
        SpriteLean3DS s = plain(4, 2, 0, 8, true);
        s.affine = true;
        s.pa = s.pd = 256;
        CHECK(SpriteLean3DS_Fill(&s, d) == 4);
        CHECK(d[0] > d[1] && d[1] > d[2] && d[2] > d[3]);
        for (int col = 0; col < 4; ++col) CHECK(d[4 + col] == d[col]);
        /* Mirrored by the matrix (pa = -1): the source's column 0 lands on the right, so it is the lowest. */
        s.pa = -256;
        CHECK(SpriteLean3DS_Fill(&s, d) == 4);
        CHECK(d[0] < d[1] && d[1] < d[2] && d[2] < d[3]);
        /* Rotated 90 degrees: the source's rows are the screen's columns, so a column lean runs along the rows of the
         * tile data (the same depth across each row). */
        s = plain(2, 4, 0, 8, true);
        s.affine = true;
        s.width = s.height = s.boundsWidth = s.boundsHeight = 32; /* 4 x 4 tiles, square so it rotates in place */
        s.pa = 0;
        s.pb = -256;
        s.pc = 256;
        s.pd = 0;
        const int runs = SpriteLean3DS_Fill(&s, d);
        CHECK(runs >= 4 && runs <= SPRITE_LEAN_3DS_MAX_RUNS);
        for (int row = 0; row < 4; ++row)
            for (int col = 1; col < 4; ++col) CHECK(d[row * 4 + col] == d[row * 4]);
        CHECK(d[0] != d[12]);
        /* A singular matrix: nothing (the caller draws it with one depth). */
        s.pa = s.pb = s.pc = s.pd = 0;
        CHECK(SpriteLean3DS_Fill(&s, d) == 0);
    }

    /* Rotated 45 degrees and leaning across 8 x 8 tiles: more changes of depth than the cap, so one depth, the highest. */
    {
        SpriteLean3DS s = plain(8, 8, 0, 20, true);
        s.affine = true;
        s.pa = 181;
        s.pb = -181;
        s.pc = 181;
        s.pd = 181;
        CHECK(SpriteLean3DS_Fill(&s, d) == 1);
        for (int i = 1; i < 64; ++i) CHECK(d[i] == d[0]);
        CHECK(d[0] > 0);
    }

    /* Too many tiles: no lean. */
    {
        const SpriteLean3DS s = plain(16, 8, 0, 8, true);
        CHECK(SpriteLean3DS_Fill(&s, d) == 0);
    }

    /* The top depth's bits as port_draw.c writes them: side, direction, and the depth under them. */
    {
        CHECK(SpriteLean3DS_Side(12) == SPRITE_LEAN_3DS_SIDE_NONE);
        CHECK(SpriteLean3DS_Side(SPRITE_LEAN_3DS_RISES_RIGHT | 12) == SPRITE_LEAN_3DS_SIDE_NONE); /* only with bit 7 */
        CHECK(SpriteLean3DS_Side(SPRITE_LEAN_3DS_COLUMNS | 12) == SPRITE_LEAN_3DS_SIDE_RISES_LEFT);
        CHECK(SpriteLean3DS_Side(SPRITE_LEAN_3DS_COLUMNS | SPRITE_LEAN_3DS_RISES_RIGHT | 12) == SPRITE_LEAN_3DS_SIDE_RISES_RIGHT);
        CHECK(((SPRITE_LEAN_3DS_COLUMNS | SPRITE_LEAN_3DS_RISES_RIGHT | 20) & SPRITE_LEAN_3DS_DEPTH_MASK) == 20);
        CHECK(SPRITE_LEAN_3DS_MAX_DEPTH <= SPRITE_LEAN_3DS_DEPTH_MASK);
    }

    /* The eyes' shifts. Not a side lean: the normal split, untouched (rows, standing sprites, everything else). */
    for (int l = -3; l <= 3; ++l)
        for (int r = -3; r <= 3; ++r) {
            CHECK(SpriteLean3DS_EyeShift(l, r, SPRITE_LEAN_3DS_SIDE_NONE, true) == l);
            CHECK(SpriteLean3DS_EyeShift(l, r, SPRITE_LEAN_3DS_SIDE_NONE, false) == r);
        }
    /* A side lean: the whole disparity in the eye that moves the higher columns towards the ground edge. */
    CHECK(SpriteLean3DS_EyeShift(2, -1, SPRITE_LEAN_3DS_SIDE_RISES_LEFT, true) == 3);  /* out of the screen: left eye, right */
    CHECK(SpriteLean3DS_EyeShift(2, -1, SPRITE_LEAN_3DS_SIDE_RISES_LEFT, false) == 0);
    CHECK(SpriteLean3DS_EyeShift(2, -1, SPRITE_LEAN_3DS_SIDE_RISES_RIGHT, true) == 0);
    CHECK(SpriteLean3DS_EyeShift(2, -1, SPRITE_LEAN_3DS_SIDE_RISES_RIGHT, false) == -3); /* right eye, to the left */
    CHECK(SpriteLean3DS_EyeShift(-2, 1, SPRITE_LEAN_3DS_SIDE_RISES_LEFT, false) == 3);   /* into the screen: right eye, right */
    CHECK(SpriteLean3DS_EyeShift(-2, 1, SPRITE_LEAN_3DS_SIDE_RISES_LEFT, true) == 0);
    CHECK(SpriteLean3DS_EyeShift(-2, 1, SPRITE_LEAN_3DS_SIDE_RISES_RIGHT, true) == -3);
    CHECK(SpriteLean3DS_EyeShift(-2, 1, SPRITE_LEAN_3DS_SIDE_RISES_RIGHT, false) == 0);
    CHECK(SpriteLean3DS_EyeShift(0, 0, SPRITE_LEAN_3DS_SIDE_RISES_LEFT, true) == 0);

    /* No gap between neighbouring columns, in either eye, at any slider: one entry of 2 x 1, 2 x 2 and 8 x 1 tiles, flipped
     * or not, both sides, and the depths the curtain gets (0 at the ground edge, up to the cap). The normal split opens
     * one in the same cases (the test would see the seam). */
    {
        int normalGaps = 0;
        const int shapes[3][2] = { { 2, 1 }, { 2, 2 }, { 8, 1 } };
        const unsigned spans[4][2] = { { 0, 6 }, { 0, 12 }, { 3, 20 }, { 0, 20 } }; /* ground edge, far edge */
        for (int shape = 0; shape < 3; ++shape)
            for (int span = 0; span < 4; ++span)
                for (int flip = 0; flip < 2; ++flip)
                    for (int sideI = 0; sideI < 2; ++sideI) {
                        const unsigned side = sideI == 0 ? SPRITE_LEAN_3DS_SIDE_RISES_LEFT : SPRITE_LEAN_3DS_SIDE_RISES_RIGHT;
                        const unsigned groundQ = spans[span][0], farQ = spans[span][1];
                        Columns c = { .count = 0 };
                        CHECK(AddEntry(&c, 40, shapes[shape][0], shapes[shape][1],
                                       side == SPRITE_LEAN_3DS_SIDE_RISES_LEFT ? groundQ : farQ,
                                       side == SPRITE_LEAN_3DS_SIDE_RISES_LEFT ? farQ : groundQ, flip != 0));
                        CHECK(c.count == shapes[shape][0]);
                        CHECK(CheckNoGap(&c, side, &normalGaps));
                    }
        CHECK(normalGaps > 0);
    }
    /* The curtain: two entries 16 px wide side by side (the seam was between them), PORT_DEPTH_LEFT, 0 at the right edge
     * and 6 quarters a 16 px half at the full rule; also with the left half capped flat (both its edges at 20, which
     * port_draw.c still marks as a side lean) and with the right one flat at 0. */
    {
        const unsigned halves[3][3] = { { 12, 6, 0 }, { 20, 20, 12 }, { 8, 0, 0 } }; /* left edge, middle, right edge */
        int normalGaps = 0;
        for (int h = 0; h < 3; ++h) {
            for (int sideI = 0; sideI < 2; ++sideI) {
                const bool left = sideI == 0;
                const unsigned side = left ? SPRITE_LEAN_3DS_SIDE_RISES_LEFT : SPRITE_LEAN_3DS_SIDE_RISES_RIGHT;
                Columns c = { .count = 0 };
                /* PORT_DEPTH_RIGHT is the mirror: the depths read from the other edge. */
                const unsigned a = left ? halves[h][0] : halves[h][2], m = halves[h][1], b = left ? halves[h][2] : halves[h][0];
                CHECK(AddEntry(&c, 24, 2, 4, m, a, false));
                CHECK(AddEntry(&c, 40, 2, 4, b, m, false));
                CHECK(c.count == 4);
                CHECK(CheckNoGap(&c, side, &normalGaps));
            }
        }
        CHECK(normalGaps > 0);
        /* The case on the console: left eye no gap with the normal split, the right eye a 1 px one; none now. */
        Columns c = { .count = 0 };
        CHECK(AddEntry(&c, 24, 2, 4, 6, 12, false));
        CHECK(AddEntry(&c, 40, 2, 4, 0, 6, false));
        CHECK(GapNormalSplit(&c, 1.0f, true) == 0 && GapNormalSplit(&c, 1.0f, false) >= 1);
        int overlap;
        CHECK(Gap(&c, SPRITE_LEAN_3DS_SIDE_RISES_LEFT, 1.0f, true, &overlap) == 0 && overlap >= 1);
        CHECK(Gap(&c, SPRITE_LEAN_3DS_SIDE_RISES_LEFT, 1.0f, false, &overlap) == 0 && overlap == 0);
    }
    /* Rows are not touched: a row lean's batches are not a side lean, so they keep the normal split (rows only move
     * sideways, so the steps between them never open a hole). */
    {
        const SpriteLean3DS s = plain(2, 4, 0, 12, false);
        CHECK(SpriteLean3DS_Fill(&s, d) == 4);
        for (int row = 0; row < 4; ++row)
            for (int eye = 0; eye < 2; ++eye) {
                const float disparity = 0.25f * (float)d[row * 2] * 1.0f;
                CHECK(BatchShift(d[row * 2], SPRITE_LEAN_3DS_SIDE_NONE, 1.0f, eye == 0) == EyeMove(disparity, eye == 0));
            }
    }

    /* PORT_DEPTH_RAISED: the entity's depth plus the rise of its top edge, capped at 20 quarters. */
    {
        CHECK(SpriteLean3DS_RaisedDepth(0, 0) == 0);
        CHECK(SpriteLean3DS_RaisedDepth(0, 12) == 12);
        CHECK(SpriteLean3DS_RaisedDepth(4, 12) == 16);
        CHECK(SpriteLean3DS_RaisedDepth(12, 12) == 20);
        CHECK(SpriteLean3DS_RaisedDepth(20, 12) == 20);
        CHECK(SpriteLean3DS_RaisedDepth(3, -5) == 3);
        /* The same as the top edge of the normal lean (port_draw.c: entity depth + Port_Config_StereoBodyDepthQ(height),
         * capped): with the default rule (height * body / 25, 12 at most) a 32 px archway at body 8 rises 10 quarters. */
        const int body = 8, height = 32;
        const int riseQ = height * body / 25 > 12 ? 12 : height * body / 25;
        CHECK(SpriteLean3DS_RaisedDepth(0, riseQ) == 10);
        /* Its pieces carry that depth at both edges: one batch, every tile at it, flipped or affine. */
        for (int variant = 0; variant < 4; ++variant) {
            SpriteLean3DS s = plain(4, 4, 10, 10, false);
            s.hflip = variant == 1;
            s.vflip = variant == 2;
            if (variant == 3) {
                s.affine = true;
                s.pa = 181; /* rotated 45 degrees */
                s.pb = -181;
                s.pc = 181;
                s.pd = 181;
            }
            CHECK(SpriteLean3DS_Fill(&s, d) == 1);
            for (int i = 0; i < 16; ++i) CHECK(d[i] == 10);
        }
        /* Over a layer that stands out the support still adds (capped). */
        SpriteLean3DS s = plain(2, 2, 16, 16, false);
        s.support = 8;
        CHECK(SpriteLean3DS_Fill(&s, d) == 1);
        for (int i = 0; i < 4; ++i) CHECK(d[i] == 20);
    }

    puts("sprite_lean_3ds_test: PASS");
    return 0;
}
