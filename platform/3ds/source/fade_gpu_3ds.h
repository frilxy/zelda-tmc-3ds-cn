#pragma once
/* A screen fade the GPU renderer can apply itself; see fade_gpu_3ds.c. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* palRamBg / palRamObj are the palettes the renderer would otherwise read.
 * True when they are exactly gPaletteBuffer faded by one (intensity, color)
 * for all 32 palettes. Then *bgPalette / *objPalette are the unfaded palettes
 * to decode from and the picture is blended toward grey *level by *weight/255. */
/* The fade the next presentation shows (the last boundary's snapshot), whatever PAL_RAM
 * holds: for pictures the presenter adds over the game's frame. */
bool FadeGpu3DS_Shown(uint8_t* weight, uint8_t* level);
/* GBA colours faded as the shown frame's palettes are (the software renderer's PAL_RAM), for
 * pictures the renderers draw beside the game's tiles; 0 stays 0. False: no fade, dst untouched. */
bool FadeGpu3DS_FadeShown(const uint16_t* src, uint16_t* dst, size_t count);

/* The game's unfaded palettes (gPaletteBuffer: 256 BG, then 256 OBJ) the shown frame was drawn
 * from: the snapshot of the last frame boundary. */
const uint16_t* FadeGpu3DS_ShownPalettes(void);

/* Black as the shown frame's fade makes it (8-bit RGB), for the bars beside a narrow view: the same
 * snapshot as the picture, not the game's next fade step. False: no fade. */
bool FadeGpu3DS_ShownBlack(uint8_t* r8, uint8_t* g8, uint8_t* b8);

bool FadeGpu3DS_State(const uint16_t* palRamBg, const uint16_t* palRamObj, const uint16_t** bgPalette,
                      const uint16_t** objPalette, uint8_t* weight, uint8_t* level);

/* Once per logic frame, before it is presented (Platform3DS_BeginFrameBoundary):
 * remembers the palette buffer and fade parameters the game's vblank will turn
 * into PAL_RAM, which is what the next presentation shows. */
void FadeGpu3DS_OnFrameBoundary(void);
