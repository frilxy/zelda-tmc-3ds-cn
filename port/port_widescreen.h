#ifndef PORT_WIDESCREEN_H
#define PORT_WIDESCREEN_H

#include "port_neighbour_rooms.h"

/*
 * port_widescreen.h — single source of truth for the rendered viewport
 * width, shared by the ViruaPPU renderer and the engine-side on-screen /
 * culling tests in the port.
 *
 * Why this exists (see docs/widescreen-*.md):
 *   The GBA decomp hardcodes a 240-px playfield into its visibility math
 *   (CheckOnScreen's 0x16E, CheckRectOnScreen's 0xF0, per-enemy
 *   `scroll_x + 0x108` bounds, ...). When the PPU renders a wider frame
 *   but the engine still culls/parks against 240, entities the engine
 *   treats as "off-screen" (parked, or AI-gated past col 240) leak into
 *   the revealed area — the sprite-edge flicker that has blocked Phase 2.
 *
 *   The two reference GBA ports that actually ship widescreen both fix
 *   this the same way — by keying every cull/camera bound off one
 *   display-width constant instead of a literal 240:
 *     - SAT-R/sa2:      include/gba/defines.h (DISPLAY_WIDTH) +
 *                       include/game/shared/stage/camera.h (IS_OUT_OF_RANGE,
 *                       IS_OUT_OF_DISPLAY_RANGE, CAM_BOUND_X).
 *     - pret/pokeemerald: src/event_object_movement.c cull at
 *                       `x >= DISPLAY_WIDTH + 16`; src/sprite.c parks
 *                       off-screen sprites at `DISPLAY_WIDTH + 64`.
 *
 * PORT_VIEW_WIDTH is that constant for this port. It tracks
 * MODE1_GBA_WIDTH, which xmake injects from `--widescreen_width=N`
 * (default 240) so the renderer and the engine cull tests can never
 * disagree. At 240 every derived bound collapses to its GBA-original
 * value, so the default build is behaviourally identical to native.
 */

/* Mirror of the ViruaPPU compile-time width. Kept #ifndef-guarded so
 * this header is self-contained when included by a TU that does not pull
 * in <cpu/mode1.h>; xmake always defines MODE1_GBA_WIDTH for tmc_pc, so
 * in a real build the injected value wins and this fallback is inert. */
#ifndef MODE1_GBA_WIDTH
#define MODE1_GBA_WIDTH 240
#endif

#include "port_3ds_full_view_policy.h"

/* Compile-time render capacity. Runtime gameplay remains 160 lines except
 * for the latched New 3DS outdoor 1:1 view. */
#define PORT_VIEW_WIDTH (MODE1_GBA_WIDTH)
#ifdef MODE1_GBA_HEIGHT
#define PORT_VIEW_HEIGHT (MODE1_GBA_HEIGHT)
#else
#define PORT_VIEW_HEIGHT 160
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime state for a wide-width build. These collapse to native no-ops when
 * MODE1_GBA_WIDTH == 240 or the WIP widescreen option is disabled.
 *
 * FallbackNative == 1 means this scene renders the GBA-native 240px canvas
 * and the presenter fits it at its correct 3:2 aspect (pillarbox bars) —
 * fixed canvases are NEVER stretched to the widescreen target.
 *
 * EffectiveViewWidth is additionally capped by the current room's width:
 * rooms narrower than the window-aspect target but wider than 240 (256 and
 * 272px rooms — most dungeons/interiors with any extra world) run true
 * widescreen at their full room width, pillarboxing only the remainder. */
int Port_Widescreen_FallbackNative(void);
int Port_Widescreen_IsActive(void);
int Port_Widescreen_EffectiveViewWidth(void);
int Port_Widescreen_EffectiveViewHeight(void);
/* World simulation keeps the last safe experimental geometry while a
 * dialogue/native mask temporarily uses the E2 presentation. */
int Port_Widescreen_GameplayViewWidth(void);
int Port_Widescreen_GameplayViewHeight(void);
/* Geometry the next BG/OAM producer tick must prepare. During a one-frame
 * activation stage it is experimental even though presentation remains E2. */
int Port_Widescreen_ProducerViewWidth(void);
int Port_Widescreen_ProducerViewHeight(void);
Port3DSFullViewMode Port_Widescreen_3DSViewMode(void);
Port3DSFullViewFallbackReason Port_Widescreen_3DSFallbackReason(void);
/* True while the top screen should keep showing its last frame instead of a
 * 240/266 one: a door (a fade, not a border scroll) has just loaded an
 * outdoor room that will be Full View within a few frames. The held frame is
 * the fully faded one. */
int Port_Widescreen_3DSHoldTop(void);
int Port_Widescreen_HudRightAnchor(void);
/* UI simulation can prepare button positions one frame before an experimental
 * geometry is published, keeping BG/OAM coherent on its first visible frame. */
int Port_Widescreen_HudTargetViewWidth(void);
int Port_Widescreen_HudTargetViewHeight(void);
/* True while the map BGs are what the PPU renders (>=1 shadow registered);
 * overlay screens (storybook, pause) drop this to 0 -> present native 240. */
int Port_Widescreen_ShadowsLive(void);
/* src/message.c: the text box as drawn now, in BG0 tiles with its border. */
int Port_Message_DrawnWindow(int* x, int* y, int* width, int* height);

/* The iris fade's WIN0 table (src/fade.c): SetVBlankDMA forgets the table it is
 * handed, the fade notes its circle after building one. Win0Lines fills
 * {left, right} per line (out: height * 2) for a view past 240x160 when the
 * HBlank DMA feeding WIN0 this frame is a noted circle, and says which kind:
 * MASK (the iris and the portal: everything inside, the backdrop outside) or
 * WINDOW (a dark room's light: the game's WININ/WINOUT apply). */
enum { PORT_WS_WIN0_NONE = 0, PORT_WS_WIN0_MASK = 1, PORT_WS_WIN0_WINDOW = 2 };
void Port_Widescreen_ForgetIris(const void* table);
void Port_Widescreen_NoteIris(void);
void Port_Widescreen_NoteWindowCircle(int x, int y, int radius);
/* A dark room's light around Link (src/manager/lightManager.c): centre in view
 * pixels, radius in pixels, drawn unscaled. */
void Port_Widescreen_NoteLightCircle(int x, int y, int radius);
/* The Minish portal's falling cutscene drawn at 400x240 (port_linked_stubs.c). Wide: the game
 * produces it this tick; ViewLive: the published frame is it; Strips: the picture's top-left
 * on screen and which picture it is (0 tree stump, 1 rock, 2 and 3 the other two; the index
 * src/enterPortalSubtask.c loads), for the presenter's side strips. */
int Port_Widescreen_PortalCutsceneWide(void);
int Port_Widescreen_CutsceneViewLive(void);
int Port_Widescreen_CutsceneStrips(int* pictureX, int* pictureY, int* portal);
/* The title screen's forest is on the shown frame (the presenter extends it). */
int Port_Widescreen_TitleForestShown(void);
/* The shown frame is a plain screen around its middle (the Nintendo/Capcom logos, a blank frame of
 * the title or file select): 1 the presenter stretches its edges over the 400x240 screen in Full
 * View, 0 not, -1 as on the frame before (port_linked_stubs.c). */
int Port_Widescreen_StretchEdges(void);
/* The file select screen's cave (BG3) is on the shown frame, 240x160 in Full View: the presenter
 * draws the extension around it from fileselect-cave.bin (port_linked_stubs.c). */
int Port_Widescreen_FileSelectCaveShown(void);
/* The pause menu's background (BG3, unscrolled) is on the shown frame, 240x160 in Full View: the presenter
 * draws the extension around it from pause-background.bin (port_linked_stubs.c). */
int Port_Widescreen_PauseBackgroundShown(void);
/* The pause menu drawn at 400x240 by the GPU renderer (port_linked_stubs.c): its BGs in the middle, the background
 * extended around them as BG3, every sprite moved into the middle as it is, so what the game moves past the GBA's
 * screen (the L and R buttons) shows whole. Wide: the game produces it this tick; ViewLive: the published frame is it. */
int Port_Widescreen_PauseWide(void);
int Port_Widescreen_PauseViewLive(void);
/* The file select drawn at 400x240 by the GPU renderer (port_linked_stubs.c): its windows and text
 * (BG0-BG2) and its cave (BG3) in the middle, the cave extended around them as BG3, the sprites
 * moved into the middle by port_draw.c. Wide: the game produces it this tick; ViewLive: the
 * published frame is it. */
int Port_Widescreen_FileSelectWide(void);
int Port_Widescreen_FileSelectViewLive(void);
/* port_draw.c: what is drawn between these moves into that view as one thing (each entity and each
 * DrawDirect is one by itself), e.g. a button and its label. Begin returns what End takes. */
int Port_Draw_WideGroupBegin(void);
void Port_Draw_WideGroupEnd(int group);
/* The pause menu's title banner closed on the left at 400x240 (port_draw.c); before CopyOAM. */
void Port_Draw_PauseBannerCap(void);
/* The pause sprites drawn while on may sit right of the GBA's screen (the item page's A, B, X, Y). */
void Port_Draw_SetPauseHudBeside(int on);
/* The title drawn at 400x240 (port_linked_stubs.c): Wide, the game produces it this tick;
 * ViewLive, the published frame is it. */
int Port_Widescreen_TitleWide(void);
int Port_Widescreen_TitleViewLive(void);
/* Scenes the stereoscopic 3D gives a depth to a layer in (platform/3ds/source/stereo_3ds.c). */
enum { PORT_STEREO_SCENE_TITLE_RAYS = 1, PORT_STEREO_SCENE_LIGHT_RAYS = 2,
       PORT_STEREO_SCENE_PATH_LEAVES = 4, PORT_STEREO_SCENE_FILE_SELECT = 8,
       PORT_STEREO_SCENE_LOGOS = 16, PORT_STEREO_SCENE_MAP_HINT = 32,
       PORT_STEREO_SCENE_GAME_OVER = 64, PORT_STEREO_SCENE_WORLD = 128,
       PORT_STEREO_SCENE_RAIN = 256 };
int Port_Stereo_Scenes(void);
/* The parts of the 400x240 view the rooms paint (the rest is black), for the presenter's blurred border: the
 * room first, then the neighbouring rooms drawn beside it; how many (at most `max`), or 0 when the room fills
 * the view or the view is not 400x240. */
int Port_Widescreen_ContentRects(PortBlurRect* rects, int max);
/* The title's light rays extended over the 400x240 view in VRAM (platform/3ds/source/
 * title_rays_3ds.c): the screen block of BG0's 512x256 map, or 0 (BG0 as the game has it). */
int Port_Widescreen_TitleRays(void);
int Port_Widescreen_Win0Lines(int width, int height, unsigned short* out);

/* EnterRoomTextboxManager writes its location banner directly to BG0 rather
 * than opening gMessage.  Publish that separate lifecycle so wide rendering
 * can center the complete two-row banner instead of applying the HUD's
 * right-edge remap through its middle. */
void Port_Widescreen_SetEnterRoomBannerActive(int active);

/* True widescreen: the view width tracks the WINDOW's aspect each frame —
 * viewW = clamp(floor(window_width * 160 / window_height), 240,
 * MODE1_GBA_WIDTH) — so a 16:9 monitor fills exactly (~284 px) with a
 * constant world scale, instead
 * of letterboxing a fixed 2.4:1 frame. MODE1_GBA_WIDTH is the framebuffer
 * capacity / hard cap, no longer the presented width.
 *  - Port_Widescreen_SetWindowPixels: present path publishes the live
 *    window client size (pixels) once per frame.
 *  - Port_Widescreen_TargetViewWidth: the aspect-fit width (config/task
 *    independent). TMC_WS_VIEW_WIDTH=<px> overrides for headless captures.
 * EffectiveViewWidth == min(TargetViewWidth, room width) while active, else
 * 240 — always use EffectiveViewWidth for camera/layout math, never the raw
 * target (see the room-cap note above). */
void Port_Widescreen_SetWindowPixels(int w, int h);
int Port_Widescreen_TargetViewWidth(void);

/* Camera rest position for a target at absolute world x (origin included):
 * target centered in the live view width, clamped so the view stays inside
 * the room. THE single formula for "where the camera stops" — used by the
 * follow camera (Scroll1), the snap cameras (InitializeCamera,
 * sub_08080974, sub_080809D4) and the script wait
 * (WaitForCameraTouchRoomBorder, which busy-waits for scroll_x EQUALITY:
 * any drift between producers and that check deadlocks a cutscene).
 * Reduces to the GBA `x - 120` clamped to [origin, origin+width-240] at an
 * effective view width of 240. */
int Port_Widescreen_CameraRestX(int target_x);
int Port_Widescreen_CameraRestY(int target_y);
/* Called before Scroll1 so a native-canvas overlay can atomically re-clamp
 * an experimental camera before BG/OAM for that fallback frame are built. */
/* Re-clamp the camera when the desired geometry has fallen back since the
 * first scroll pass. Returns nonzero only when it changed camera geometry. */
int Port_Widescreen_PrepareGameplayCamera(void);
/* Snapshot outgoing map tiles before the room loader replaces them. */
void Port_Widescreen_BeginScroll(unsigned room, unsigned direction);
int Port_Widescreen_AdvanceScroll(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_WIDESCREEN_H */
