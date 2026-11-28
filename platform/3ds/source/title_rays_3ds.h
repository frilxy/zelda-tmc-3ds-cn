#pragma once
/* The title's light rays (BG0) extended over the 400x240 Full View by copying the rays themselves
 * (docs/wide-scene-extension.md, "Layers animated by palette"). Nothing of the game's art ships:
 * the extension is computed from the BG0 the game loaded into VRAM, the first time the title is
 * drawn wide, and written back into VRAM as tiles and a 512x256 map, so both renderers draw it as
 * the game's own BG0 (palette rotation, blend and fades unchanged). */
#include <stdint.h>

enum {
    TITLE_RAYS_SOURCE_SCREEN = 29, /* the game's BG0 map (src/title.c) */
    TITLE_RAYS_FOREST_SCREEN = 30, /* BG1's: its tiles share character block 0 */
    TITLE_RAYS_SCREEN = 26,        /* the extended map: screen blocks 26 and 27, unused by the title */
    TITLE_RAYS_LEFT = 80,          /* the original's place in the 400x240 view */
    TITLE_RAYS_TOP = 40,
};

/* Keeps the extension in vram (the GBA's 96 KB): rebuilt when the source changed (a new title, a
 * reloaded graphics group), rewritten when something overwrote it. Returns the screen block of the
 * extended 512x256 map, or 0 when it cannot be made (the source is not the rays: no extension). */
int TitleRays3DS_Update(uint8_t* vram);
