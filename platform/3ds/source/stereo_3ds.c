#include "stereo_3ds.h"

#include "port_ppu_gpu_3ds_model.h"
#include "zone_depth_3ds.h"
#include "port_ppu_gpu_3ds.h"

#include <3ds.h>
#include <string.h>

extern bool Port_Config_Stereo3D(void);
extern int Port_Stereo_Scenes(void); /* PORT_STEREO_SCENE_* of port_widescreen.h */
extern int Port_Config_StereoWorldDepth(int which); /* 0: the layer over the playfield (BG1), 1: sprites (not the HUD's) */
enum { SCENE_TITLE_RAYS = 1, SCENE_LIGHT_RAYS = 2, SCENE_PATH_LEAVES = 4, SCENE_FILE_SELECT = 8, SCENE_LOGOS = 16, SCENE_MAP_HINT = 32, SCENE_GAME_OVER = 64, SCENE_WORLD = 128, SCENE_RAIN = 256 };

/* Pixels at the full slider, by layer (BG0..BG3, OBJ) and priority (docs/stereo-3d.md). In front of
 * the screen: BG0 at priority 0, the HUD and the text boxes, and the sprites at priority 0, the HUD's
 * buttons (the game draws Link and the world's sprites at 1-3). The rest on the screen plane. */
static const signed char kDepth[5][4] = {
    { 6, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 6, 0, 0, 0 },
};

/* Sun rays hang in front of the screen, behind the HUD's 6, only in the scenes that have them
 * (owner, 2026-10-17): the title's (BG0 at priority 2) and the Minish Woods' and the barrel
 * house's (BG3 at priority 0, LightRayManager). */
/* The leaves of the Minish Paths (BG1 at priority 1) hang the same way. */
/* The game over screen: the letters of GAME OVER stand out furthest, the panel's choices (BG0 at priority 0, kDepth's 6)
 * and the stone panel behind them. */
enum { GAME_OVER_TEXT_DEPTH = 10, GAME_OVER_CHOICES_DEPTH = 6, GAME_OVER_PANEL_DEPTH = 3 };
enum { MAP_HINT_BACK_DEPTH = 0, MAP_HINT_MAP_DEPTH = 2, MAP_HINT_MARKER_DEPTH = 5 };
/* The layers over the world (fog, sun rays, the paths' leaves, the rain) at the HUD's 6: the world's sprites reach 5
 * (a tall one, the Crenel's mushrooms), and at 4 they stood level with them (owner, 2026-11-19). The HUD is the layer
 * of highest priority, so it stays drawn over them. */
enum { RAYS_DEPTH = 6, LEAVES_DEPTH = 6, TITLE_FRONT_DEPTH = 8, FILE_SELECT_OBJ_DEPTH = 4, FILE_SELECT_PANEL_DEPTH = 2 };

static bool sActive;
static bool sHasDepth;
static int sScenes;
static int sScenesAny; /* the scenes even with the slider down: the tint views read them */
static float sSlider;
static bool sForce;
void Stereo3DS_SetForce(bool on) { sForce = on; }
bool Stereo3DS_Forced(void) { return sForce; }

void Stereo3DS_BeginFrame(void) {
    const bool setting = Port_Config_Stereo3D();
    sSlider = sForce ? 1.0f : osGet3DSliderState();
    sActive = setting && sSlider > 0.0f;
    sScenesAny = setting ? Port_Stereo_Scenes() : 0;
    sScenes = sActive ? sScenesAny : 0;
    sHasDepth = sScenes != 0;
    if (sActive)
        for (unsigned layer = 0; layer < 5 && !sHasDepth; ++layer)
            for (unsigned priority = 0; priority < 4; ++priority)
                if (kDepth[layer][priority] != 0) sHasDepth = true;
    /* The current room's zone depth fixes (port_zone_depth.inc), only in the world scene with 3D on, or with a PLANE
     * TINT view up (it paints them at their plane with the slider down too). */
    ZoneDepth3DS_BeginFrame((sHasDepth && Stereo3DS_WorldScene()) ||
                            (PortPpuGpu3DS_TintMode() != 0 && (sScenesAny & SCENE_WORLD) != 0));
}

bool Stereo3DS_Active(void) { return sActive; }

bool Stereo3DS_HasDepth(void) { return sHasDepth; }

bool Stereo3DS_WorldScene(void) { return sActive && (sScenesAny & SCENE_WORLD) != 0; }

static int DepthFor(int scenes, unsigned layer, unsigned priority) {
    if (layer > PPU_GPU3DS_OBJ || priority > 3) return 0;
    int depth = kDepth[layer][priority];
    /* Playing: the layer drawn over Link (tree tops, eaves, roofs) stands furthest out, Link and the world's
     * sprites (priority 1-3; the HUD's are 0) a little, the ground and the sky stay on the screen plane. The
     * scenes below override it. */
    if (scenes & SCENE_WORLD) {
        if (layer == PPU_GPU3DS_BG1) depth = Port_Config_StereoWorldDepth(0);
        else if (layer == PPU_GPU3DS_OBJ && priority >= 1) depth = Port_Config_StereoWorldDepth(1);
    }
    if (((scenes & SCENE_TITLE_RAYS) && layer == PPU_GPU3DS_BG0 && priority == 2) ||
        ((scenes & SCENE_LIGHT_RAYS) && layer == PPU_GPU3DS_BG3 && priority == 0))
        depth = RAYS_DEPTH;
    if ((scenes & SCENE_PATH_LEAVES) && layer == PPU_GPU3DS_BG1 && priority == 1) depth = LEAVES_DEPTH;
    /* Mount Crenel's rain (BG1 at priority 1) hangs where the fog and the sun rays do, behind the HUD. */
    if ((scenes & SCENE_RAIN) && layer == PPU_GPU3DS_BG1 && priority == 1) depth = RAYS_DEPTH;
    /* On the title the logo (sprites) and the sword (BG2 at priority 1) are drawn in front of the rays,
     * so they hang in front of them too (owner, 2026-10-17). */
    if ((scenes & SCENE_TITLE_RAYS) && (layer == PPU_GPU3DS_OBJ || (layer == PPU_GPU3DS_BG2 && priority == 1)))
        depth = TITLE_FRONT_DEPTH;
    /* The Nintendo and Capcom logos are BG2 at priority 2 (owner, 2026-10-17). */
    if ((scenes & SCENE_LOGOS) && layer == PPU_GPU3DS_BG2 && priority == 2) depth = FILE_SELECT_OBJ_DEPTH;
    /* The file select: its sprites (banner, slots, texts, the preview) hang in front of the cave, the preview's
     * panel (BG2) between them (owner, 2026-10-17). */
    if (scenes & SCENE_FILE_SELECT) {
        if (layer == PPU_GPU3DS_OBJ) depth = FILE_SELECT_OBJ_DEPTH;
        else if (layer == PPU_GPU3DS_BG2 && priority == 2) depth = FILE_SELECT_PANEL_DEPTH;
        else if (layer == PPU_GPU3DS_BG1 && priority == 1) depth = FILE_SELECT_OBJ_DEPTH; /* hearts, languages, the name's letters */
    }
    /* The map hints (owner, 2026-10-17): the grey that covers what is not unlocked and its frame are BG2, on the
     * screen plane with the backdrop (the green around the frame, which cannot move); the unlocked map (BG1, shown
     * through the OBJ window the sprites make) in front of the grey, and the markers (sprites) in front of both.
     * The HUD's depth stays for BG0 (the numbers box). */
    if (scenes & SCENE_MAP_HINT) {
        if (layer == PPU_GPU3DS_BG2) depth = MAP_HINT_BACK_DEPTH;
        else if (layer == PPU_GPU3DS_BG1) depth = MAP_HINT_MAP_DEPTH;
        else if (layer == PPU_GPU3DS_OBJ) depth = MAP_HINT_MARKER_DEPTH;
    }
    if (scenes & SCENE_GAME_OVER) {
        if (layer == PPU_GPU3DS_OBJ) depth = GAME_OVER_TEXT_DEPTH;
        else if (layer == PPU_GPU3DS_BG0) depth = GAME_OVER_CHOICES_DEPTH;
        else if (layer == PPU_GPU3DS_BG1 || layer == PPU_GPU3DS_BG2) depth = GAME_OVER_PANEL_DEPTH;
    }
    return depth;
}

bool Stereo3DS_LayerSeeThrough(unsigned layer, unsigned priority) {
    return (sScenes & SCENE_RAIN) && layer == PPU_GPU3DS_BG1 && priority == 1;
}

int Stereo3DS_LayerDepth(unsigned layer, unsigned priority) {
    return DepthFor(sScenesAny, layer, priority);
}

/* The move of an eye, in whole view pixels, for a total disparity `disparity` (depth x slider, pixels): the
 * disparity is rounded to whole pixels and split between the eyes, the left eye taking the larger half of an
 * odd one (1 px: left 1, right 0). Whole pixels keep the art unscaled; splitting the odd pixel makes the
 * smallest step 1 px of disparity instead of 2, which is what keeps a small depth (Link, a roof) from floating.
 * The left eye's move is nonzero whenever the disparity is: the renderer decides from it which layers move. */
static float EyeMove(float disparity, Stereo3DSEye eye) {
    const float magnitude = disparity < 0.0f ? -disparity : disparity;
    const int total = (int)(magnitude + 0.5f);
    const int left = (total + 1) / 2;
    const int move = eye == STEREO3DS_LEFT ? left : -(total - left);
    return disparity < 0.0f ? (float)-move : (float)move;
}

float Stereo3DS_LayerShift(unsigned layer, unsigned priority, Stereo3DSEye eye) {
    if (!sHasDepth || layer > PPU_GPU3DS_OBJ || priority > 3) return 0.0f;
    const int depth = DepthFor(sScenes, layer, priority);
    return EyeMove((float)depth * sSlider, eye);
}

float Stereo3DS_SpriteShift(unsigned depthQuarters, Stereo3DSEye eye) {
    if (!sHasDepth) return 0.0f;
    return EyeMove(0.25f * (float)depthQuarters * sSlider, eye);
}

/* The sprites of a map hint in OBJ-window mode mark the parts still covered (BG1, the map, is hidden inside them and BG2,
 * the grey, shows). The renderer cuts BG2 in batches along the window's edges as the game drew them, so the mask stays where
 * BG2 is: moved with the map it left a 2 px line of backdrop at its edges (found with both eyes captured, FORCE 3D). */
bool Stereo3DS_ObjWindowFollows(unsigned* layer, unsigned* priority) {
    if (!sHasDepth || !(sScenes & SCENE_MAP_HINT)) return false;
    *layer = PPU_GPU3DS_BG2;
    *priority = 3;
    return true;
}
