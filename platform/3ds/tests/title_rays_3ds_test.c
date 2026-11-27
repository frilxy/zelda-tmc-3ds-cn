/* Host test of title_rays_3ds.c against a screen dump of the settled title.
 *
 *   cc -std=c11 -Iplatform/3ds/source platform/3ds/tests/title_rays_3ds_test.c \
 *      platform/3ds/source/title_rays_3ds.c -o /tmp/title_rays_test
 *   /tmp/title_rays_test DUMP-vram.bin [OUT.raw]
 *
 * Checks that the extension leaves BG0's original pixels where the 400x240 view shows them and
 * stays in the VRAM it may use; OUT.raw gets the extended BG0 as 400x240 indices (0 transparent),
 * to compare with tools/scene-extend/extend_rays.py's rays-indices.png. The dump is the game's art:
 * not in the repository, so without one the test only says so. */
#include "title_rays_3ds.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t Pixel(const uint8_t* vram, int screen, int wide, int x, int y) {
    const int tx = x / 8, ty = y / 8;
    const uint8_t* p = vram + (screen + (wide ? tx / 32 : 0)) * 0x800 + (ty % 32 * 32 + tx % 32) * 2;
    const unsigned e = p[0] | (p[1] << 8), t = e & 0x3ffu;
    const int sx = (e >> 10) & 1 ? 7 - x % 8 : x % 8, sy = (e >> 11) & 1 ? 7 - y % 8 : y % 8;
    return (vram[t * 32u + sy * 4u + sx / 2u] >> (4 * (sx & 1))) & 15;
}

int main(int argc, char** argv) {
    static uint8_t vram[0x18000], before[0x18000];
    FILE* f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (f == NULL || fread(vram, 1, sizeof(vram), f) != sizeof(vram)) {
        printf("title_rays_3ds_test: no dump given (DUMP-vram.bin): skipped\n");
        return 0;
    }
    fclose(f);
    memcpy(before, vram, sizeof(vram));

    assert(TitleRays3DS_Update(vram) == TITLE_RAYS_SCREEN);
    /* The original, unchanged, at its place in the view. */
    for (int y = 0; y < 120; y++)
        for (int x = 0; x < 256; x++)
            assert(Pixel(vram, TITLE_RAYS_SCREEN, 1, x + TITLE_RAYS_LEFT, y + TITLE_RAYS_TOP) ==
                   Pixel(before, TITLE_RAYS_SOURCE_SCREEN, 0, x, y));
    /* Only the free tiles at the top of character block 0 and screen blocks 26-27 written. */
    size_t firstTile = 0x8000;
    for (size_t i = 0; i < sizeof(vram); i++) {
        if (vram[i] == before[i]) continue;
        const int inMap = i >= TITLE_RAYS_SCREEN * 0x800u && i < (TITLE_RAYS_SCREEN + 2) * 0x800u;
        assert(inMap || (i >= 0x6000 && i < 0x8000));
        if (!inMap && i < firstTile) firstTile = i;
    }
    /* Already there: nothing rewritten; overwritten: put back. */
    memcpy(before, vram, sizeof(vram));
    assert(TitleRays3DS_Update(vram) == TITLE_RAYS_SCREEN && memcmp(before, vram, sizeof(vram)) == 0);
    memset(vram + TITLE_RAYS_SCREEN * 0x800, 0, 0x1000);
    memset(vram + firstTile, 0, 0x8000 - firstTile);
    assert(TitleRays3DS_Update(vram) == TITLE_RAYS_SCREEN && memcmp(before, vram, sizeof(vram)) == 0);

    if (argc > 2) {
        FILE* out = fopen(argv[2], "wb");
        for (int y = 0; y < 240; y++)
            for (int x = 0; x < 400; x++) fputc(Pixel(vram, TITLE_RAYS_SCREEN, 1, x, y), out);
        fclose(out);
    }
    printf("title_rays_3ds_test: ok\n");
    return 0;
}
