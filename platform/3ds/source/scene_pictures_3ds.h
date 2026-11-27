#pragma once
/* Pictures that extend a scene the GBA drew at 240x160, for the renderers (docs/wide-scene-extension.md).
 * The title screen's forest, 400x240: the player's own title-forest.bin in the data folder, else the
 * one shipped in the romfs. Loaded when first asked for, freed by ScenePictures_Release. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SCENE_TITLE_W = 400, SCENE_TITLE_H = 240, SCENE_CAVE_W = 400, SCENE_CAVE_H = 240, SCENE_CAVE_FIRST_PALETTE = 11 };
/* The vertical Minish paths drawn by verticalMinishPathBackgroundManager.c (a 240-wide room, its ground, BG1 over it
 * and BG3's shadows): the generated strips beside the room, 80 columns each side, for its three layers.
 * SCENE_SHAFT_CRENEL, the Crenel paths' shaft (area 0x12 room 00, 240x800): the ground (BG2) has the room's top (32
 * rows) and one turn of the 320 rows it repeats, which the renderer repeats too; the rock ledges (BG1) and their
 * shadows (BG3) the 960 rows their scroll shows. SCENE_SHAFT_MINISH, the grass corridor of the Minish Paths (area
 * 0x11, room 0): its leaves (BG1) and their shadows (BG3), 1024 rows; its ground filled with the colour of its outer
 * band. The other Minish corridors (rooms 1-4) as that one, each with its own ground. */
enum { SCENE_SHAFT_STRIP_W = 80, SCENE_SHAFT_ROOM_W = 240 };
typedef enum { SCENE_SHAFT_GROUND, SCENE_SHAFT_LEDGES, SCENE_SHAFT_SHADOWS, SCENE_SHAFT_LAYERS } SceneShaftLayer;
typedef enum {
    SCENE_SHAFT_CRENEL, SCENE_SHAFT_MINISH, SCENE_SHAFT_MINISH_WATER, SCENE_SHAFT_MINISH_TOWN, SCENE_SHAFT_MINISH_RANCH,
    SCENE_SHAFT_MINISH_CABIN, SCENE_SHAFT_CRENEL_ROCKS, SCENE_SHAFT_SCENES
} SceneShaft;
/* The shaft of a vertical Minish path room, -1 for none (or BG1's map, 0x2000 bytes, not the one its strips are of; NULL
 * when BG1 is not the manager's, which only the scenes found by their room alone take). */
int ScenePictures_ShaftScene(int area, int room, const uint8_t* leavesMap);

bool ScenePictures_TitleAvailable(void);
/* GBA 15-bit colours, row-major, 0 where the game's own picture shows (the shipped files keep
 * only the generated parts); NULL when there is no picture. */
const uint16_t* ScenePictures_Title555(void);
/* The same through the game's colour conversion (colour correction as it is now), 0 kept. */
const uint32_t* ScenePictures_TitleAbgr(void);
/* For the software renderer: faded as the shown frame's palettes are (the GPU renderer fades
 * the whole frame itself), else ScenePictures_TitleAbgr. */
const uint32_t* ScenePictures_TitleAbgrFaded(void);
void ScenePictures_Release(void);

/* The pause menu's background, 400x240, made by the game from its own BG3 (no file: nothing of the game's art is
 * shipped) while the pause menu shows it: palette indices, (palette - *firstPalette) * 16 + index into four BG palettes
 * from *firstPalette, 0 where the game's own screen is or nothing could be made. Made again, in the same buffer, when
 * BG3's map changes (*remade, when not NULL, says so).
 * ScenePictures_Pause555 is the same in GBA colours (the shown frame's unfaded palettes), for the presenter. Freed by
 * ScenePictures_ReleasePause. */
enum { SCENE_PAUSE_W = 400, SCENE_PAUSE_H = 240 };
const uint8_t* ScenePictures_PauseIndices(unsigned* firstPalette, bool* remade);
const uint16_t* ScenePictures_Pause555(void);
void ScenePictures_ReleasePause(void);

/* The file select's cave, 400x240 (fileselect-cave.bin, tools/scene-extend/to_console.py fileselect):
 * one byte a pixel, (palette - SCENE_CAVE_FIRST_PALETTE) * 16 + index into BG palettes 11-14, which
 * the game cycles; 0 where the game's own picture is. NULL when there is none. Freed by
 * ScenePictures_ReleaseCave. */
bool ScenePictures_CaveAvailable(void);
const uint8_t* ScenePictures_CaveIndices(void);
void ScenePictures_ReleaseCave(void);

/* A layer's strips (crenel-paths-{ground,ledges,shadows}.bin, tools/scene-extend/to_console.py crenel-*): GBA
 * 15-bit colours, 2 * SCENE_SHAFT_STRIP_W wide (left strip, then right), 0 where empty; NULL when there is none.
 * `height` gets its rows, `loopStart` and `loopLength` the rows the renderer repeats past them (0: none). For the
 * scene ScenePictures_SetShaftView named last. Freed by ScenePictures_ReleaseShaft. */
const uint16_t* ScenePictures_Shaft555(SceneShaftLayer layer, unsigned* height, unsigned* loopStart,
                                       unsigned* loopLength);
void ScenePictures_ReleaseShaft(void);
/* Which shaft is shown and where it lies on the view, set by the game each frame (port_linked_stubs.c): `groundBg` the
 * BG that draws its ground, -1 when no shaft is shown in Full View; x the left strip's left edge on the view, y[layer]
 * its top. */
void ScenePictures_SetShaftView(int scene, int groundBg, int x, int groundY, int ledgesY, int shadowsY);
int ScenePictures_ShaftView(int* x, int y[SCENE_SHAFT_LAYERS]);

/* Crenel's horizontal Minish path (area 0x12 room 02, 800x192, horizontalMinishPathBackgroundManager.c) in Full View:
 * the room is shorter than the view, so its layers get bands above and below it instead of strips beside it
 * (crenel-hpath-{ground,rocks,shadows}.bin, tools/scene-extend/to_console.py crenel-hpath-*). Each layer's picture is
 * `width` columns (a power of two, repeated sideways by the renderer) and rows[0] + rows[1] tall: the first band
 * (rows[0] rows), then the second. The ground (64 wide: one turn of its wall's 64-column repeat) has 48 rows above the
 * room and 48 below; the rocks (BG1) and their shadows (BG3), 1024 wide (the whole layer): the layer's rows -32..15
 * (the small stalactites' continuation above their cut: one copy starts at row 16, which the view shows at row 4)
 * and rows 208-255 (the stalagmites' continuation below theirs). GBA 15-bit colours, 0
 * where empty; NULL without the file, or without WIDE + AI (generated art). Freed by ScenePictures_ReleaseRows. */
typedef enum { SCENE_ROW_GROUND, SCENE_ROW_ROCKS, SCENE_ROW_SHADOWS, SCENE_ROW_LAYERS } SceneRowLayer;
/* The rooms that have them: 0x12/02 and the next one, 0x12/03 (the same maps and tiles in other palettes, so the same
 * pictures with each colour moved to the same palette entry: crenel-hpath-03-*.bin). */
typedef enum { SCENE_ROW_CRENEL_02, SCENE_ROW_CRENEL_03, SCENE_ROW_SCENES } SceneRowScene;
const uint16_t* ScenePictures_Row555(SceneRowLayer layer, unsigned* width, unsigned rows[2]);
void ScenePictures_ReleaseRows(void);
/* Which room is shown and where the layers' bands lie on the view, set by the game each frame (port_linked_stubs.c):
 * `scene` a SceneRowScene, `groundBg` the BG that draws the ground, -1 when no such room is shown in Full View; x[layer] the view column of the picture's column 0
 * (the layer's own scroll), y[layer][band] the band's top row on the view. */
void ScenePictures_SetRowView(int scene, int groundBg, const int x[SCENE_ROW_LAYERS], const int y[SCENE_ROW_LAYERS][2]);
int ScenePictures_RowView(int x[SCENE_ROW_LAYERS], int y[SCENE_ROW_LAYERS][2]);

/* Reads a scene file into `out` (exactly `bytes`): the player's own `sd` path, else the shipped `romfs` one. The file
 * is the picture as it is (`bytes` long, as older to_console.py wrote it), a zlib stream (tools/scene-extend/
 * to_console.py's default) or the GBA BIOS's LZ77 (0x10 and the size; to_console.py --lz77, the fallback should zlib
 * ever be missing). False when neither file is there or it is none of those. */
bool ScenePictures_ReadFile(const char* sd, const char* romfs, void* out, size_t bytes);
