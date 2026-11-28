/* The game's screen fade, described so the GPU renderer can apply it (docs/gpu-fade.md).
 *
 * src/fade.c's FadeVBlank turns each of the 32 palettes (16 BG, 16 OBJ; 16
 * colours each) of gPaletteBuffer into PAL_RAM: every channel moves toward
 * `color` (0..255, grey) by `intensity`/256, then goes through the user's
 * brightness table (the identity at the default brightness). When every palette
 * was faded by the same (intensity, color), the whole picture is the unfaded
 * one blended toward that grey, which costs the GPU one full-screen quad while
 * the tiles, decoded once from the unfaded palettes, are never decoded again.
 *
 * This file lives in the port, not in src/: a change to src/ moves the game's
 * layout and invalidates every save state (docs/save-states.md). It repeats
 * the fade arithmetic of src/fade.c's Port_MakeFadeBuff256 and then checks the
 * result against PAL_RAM, so a copy that drifts from the game's fails the check
 * and the frame simply keeps using PAL_RAM. */
#include "fade_gpu_3ds.h"

#include "global.h"
#include "main.h"
#include "region.h"
#include "structures.h"
#include "port_gba_mem.h"
#include "port_rom.h"

#include <string.h>

/* Offsets of the 9-table block in the ROM (data/data_08000F54.s): 3 brightness
 * levels, each {R, G, B} of 32 u16 entries, the EU build 0x48 later. */
static const u32 sFadeTableOffsets[3][3] = {
    { 0x0F84, 0x0FC4, 0x1004 },
    { 0x1044, 0x1084, 0x10C4 },
    { 0x1104, 0x1144, 0x1184 },
};

static void FadePalette(const u16* src, u16* dst, u16 intensity, u8 color) {
    const u32 bias = (u32)intensity * (u32)color;
    const u32 factor = 0x400 - (u32)intensity * 4;
    u8 brightness = gEwram[6];
    if (brightness >= 3)
        brightness = 0;
    u32 regionShift = 0;
#if defined(MULTI_REGION) || defined(EU)
    if (REGION_IS_EU)
        regionShift = 0x48;
#endif
    const u16* tableR = (const u16*)(gRomData + sFadeTableOffsets[brightness][0] + regionShift);
    const u16* tableG = (const u16*)(gRomData + sFadeTableOffsets[brightness][1] + regionShift);
    const u16* tableB = (const u16*)(gRomData + sFadeTableOffsets[brightness][2] + regionShift);
    for (unsigned i = 0; i < 16; ++i) {
        const u32 shifted = (u32)src[i] << 1;
        u32 r = shifted & 0x3E;
        u32 g = (shifted >> 5) & 0x3E;
        u32 b = (shifted >> 10) & 0x3E;
        r = ((factor * r + bias) >> 10) & 0x3E;
        g = ((factor * g + bias) >> 10) & 0x3E;
        b = ((factor * b + bias) >> 10) & 0x3E;
        dst[i] = tableR[r >> 1] | tableG[g >> 1] | tableB[b >> 1];
    }
}

/* Why the last calls said no, for the link (tmc-link.py peek gFadeGpu3DSStats 64):
 * calls, no fade, brightness or ROM, mismatch, ok, palette that mismatched,
 * intensity, colour, then the first differing entry: index, expected, PAL_RAM,
 * source, gUsedPalettes. */
uint32_t gFadeGpu3DSStats[16];

/* Does PAL_RAM hold exactly gPaletteBuffer faded by (intensity, color)? */
static bool PalRamIsFade(const u16* source, const u16* palRamBg, const u16* palRamObj, u16 intensity, u8 color,
                         bool record) {
    u16 faded[16];
    for (unsigned palette = 0; palette < 32; ++palette) {
        FadePalette(&source[palette * 16], faded, intensity, color);
        const u16* expected = palette < 16 ? palRamBg + palette * 16 : palRamObj + (palette - 16) * 16;
        if (memcmp(faded, expected, sizeof(faded)) != 0) {
            if (record) {
                ++gFadeGpu3DSStats[3];
                gFadeGpu3DSStats[5] = palette;
                for (unsigned entry = 0; entry < 16; ++entry) {
                    if (faded[entry] != expected[entry]) {
                        gFadeGpu3DSStats[8] = entry;
                        gFadeGpu3DSStats[9] = faded[entry];
                        gFadeGpu3DSStats[10] = expected[entry];
                        gFadeGpu3DSStats[11] = source[palette * 16 + entry];
                        gFadeGpu3DSStats[12] = gUsedPalettes & 0xffffu;
                        break;
                    }
                }
            }
            return false;
        }
    }
    return true;
}

typedef struct FadeSnapshot {
    u16 buffer[512];
    u16 intensity;
    u8 color;
} FadeSnapshot;

static FadeSnapshot sSnapshot[2];
static unsigned sCurrent;

void FadeGpu3DS_OnFrameBoundary(void) {
    sCurrent ^= 1u;
    FadeSnapshot* snapshot = &sSnapshot[sCurrent];
    memcpy(snapshot->buffer, gPaletteBuffer, sizeof(snapshot->buffer));
    snapshot->intensity = gUnk_020354C0[0].unk2;
    snapshot->color = gUnk_020354C0[0].unk1;
}

bool FadeGpu3DS_FadeShown(const uint16_t* src, uint16_t* dst, size_t count) {
    const FadeSnapshot* snapshot = &sSnapshot[sCurrent ^ 1u];
    if (snapshot->intensity == 0 || snapshot->intensity > 256 || gRomData == NULL) return false;
    uint16_t in[16], out[16];
    for (size_t i = 0; i < count; i += 16) {
        const size_t n = count - i < 16 ? count - i : 16;
        memcpy(in, src + i, n * sizeof(uint16_t));
        FadePalette(in, out, snapshot->intensity, snapshot->color);
        for (size_t k = 0; k < n; ++k) dst[i + k] = src[i + k] != 0 ? out[k] : 0; /* 0 stays "none" */
    }
    return true;
}

const uint16_t* FadeGpu3DS_ShownPalettes(void) {
    return sSnapshot[sCurrent ^ 1u].buffer;
}

bool FadeGpu3DS_Shown(uint8_t* weight, uint8_t* level) {
    const FadeSnapshot* snapshot = &sSnapshot[sCurrent ^ 1u];
    if (snapshot->intensity == 0 || snapshot->intensity > 256) return false;
    *weight = (uint8_t)((snapshot->intensity * 255u + 128u) / 256u);
    *level = snapshot->color;
    return true;
}

bool FadeGpu3DS_ShownBlack(uint8_t* r8, uint8_t* g8, uint8_t* b8) {
    const FadeSnapshot* snapshot = &sSnapshot[sCurrent ^ 1u];
    if (snapshot->intensity == 0 || snapshot->intensity > 256 || gRomData == NULL) return false;
    uint16_t in[16] = { 0 }, out[16];
    FadePalette(in, out, snapshot->intensity, snapshot->color);
    *r8 = (uint8_t)((out[0] & 31u) * 255u / 31u);
    *g8 = (uint8_t)(((out[0] >> 5) & 31u) * 255u / 31u);
    *b8 = (uint8_t)(((out[0] >> 10) & 31u) * 255u / 31u);
    return true;
}

bool FadeGpu3DS_State(const uint16_t* palRamBg, const uint16_t* palRamObj, const uint16_t** bgPalette,
                      const uint16_t** objPalette, uint8_t* weight, uint8_t* level) {
    /* The game steps the fade and animates palettes in its logic, and only the
     * next vblank (src/interrupts.c, WaitForNextFrame) turns the buffer into
     * PAL_RAM. A presentation therefore shows the previous logic frame's
     * buffer faded with the previous frame's parameters: the snapshot of the
     * last boundary. The current one is tried as well, for a call the boundary
     * did not precede. */
    ++gFadeGpu3DSStats[0];
    const FadeSnapshot* candidates[2] = { &sSnapshot[sCurrent ^ 1u], &sSnapshot[sCurrent] };
    gFadeGpu3DSStats[6] = candidates[0]->intensity;
    gFadeGpu3DSStats[7] = candidates[0]->color;
    if (candidates[0]->intensity == 0 && candidates[1]->intensity == 0) {
        ++gFadeGpu3DSStats[1];
        return false;
    }
    /* Only the default brightness table is the identity the GPU blend assumes. */
    if (gEwram[6] != 1 || gRomData == NULL || palRamBg == NULL || palRamObj == NULL) {
        ++gFadeGpu3DSStats[2];
        return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        const FadeSnapshot* snapshot = candidates[i];
        if (snapshot->intensity == 0 || snapshot->intensity > 256)
            continue;
        if (!PalRamIsFade(snapshot->buffer, palRamBg, palRamObj, snapshot->intensity, snapshot->color, i == 0))
            continue;
        ++gFadeGpu3DSStats[4];
        *bgPalette = &snapshot->buffer[0];
        *objPalette = &snapshot->buffer[256];
        /* The game blends by intensity/256 (256: the whole way); the blend takes /255. */
        *weight = (uint8_t)((snapshot->intensity * 255u + 128u) / 256u);
        *level = snapshot->color;
        return true;
    }
    return false;
}
