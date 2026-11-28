/*
 * 3DS-only wrapper around the shared Minish Cap second-screen compositor.
 *
 * Keeping the wrapper in platform/3ds makes the title/file-select behavior
 * independent of Android and desktop. The shared compositor is included as
 * this translation unit's implementation so this small adapter can select
 * its existing Settings page without adding title-menu state to every port.
 */
#include "port_second_screen_3ds.h"
#include "bottom_idle_3ds.h"
#include "bottom_frame_state_3ds.h"
#include "bottom_map_anim_3ds.h"

#include <stdbool.h>

/* This replaces the direct port/port_second_screen.c entry in the 3DS CMake
 * source list. All normal exported functions retain their original symbols. */
#include "../../../port/port_second_screen.c"

void Port_SecondScreen_3DS_DrawCombinedCollection(uint32_t* pixels, int stride, const SecondScreenSnapshot* snap) {
    Port_SecondScreenQuest_DrawCompact3DS(pixels, stride, snap);
}

int Port_SecondScreen_3DS_CombinedCollectionTap(int x, int y) {
    for (int id = 0; id < PORT_QUEST_COMPACT_COUNT; ++id) {
        const SecondScreenQuestRect r = Port_SecondScreenQuest_Compact3DSRect(id);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return id;
    }
    return -1;
}

bool Port_SecondScreen_3DS_CombinedCollectionRect(int id, int* x, int* y, int* width, int* height) {
    const SecondScreenQuestRect r = Port_SecondScreenQuest_Compact3DSRect(id);
    if (!r.w) return false;
    if (x) *x = r.x;
    if (y) *y = r.y;
    if (width) *width = r.w;
    if (height) *height = r.h;
    return true;
}

static volatile uint8_t sIdleSettingsOpen;
static uint8_t sIdleOwnsSettingsTab;
static BottomFrameState3DS sFrameState;
static struct { int x, y, longPress, frames; } sDeferredTap;

bool Port_SecondScreen_3DS_HasDeferredTap(void) { return sDeferredTap.frames != 0; }

static uint32_t BeginRefresh(void) {
    return BottomFrameState3DS_BeginPaint(&sFrameState);
}

static void FinishRefresh(uint32_t request) {
    /* A tap may arrive while the worker is painting.  Acknowledge only the
     * generation this frame actually observed; a newer generation remains
     * pending and schedules another paint instead of being lost. */
    BottomFrameState3DS_FinishPaint(&sFrameState, request);
}

static uint32_t RequestRefresh(void) {
    return BottomFrameState3DS_Request(&sFrameState);
}

static uint32_t RequestTouchRefresh(void) {
    sDeferredTap.frames = 0;
    return BottomFrameState3DS_RequestTouchRefresh(&sFrameState);
}

bool Port_SecondScreen_3DS_InGame(void) {
    SecondScreenSnapshot snap;
    Port_SecondScreenState_Read(&snap);
    return snap.inGame != 0;
}

void Port_SecondScreen_3DS_ToggleInventory(void) {
    SecondScreenSnapshot snap;
    Port_SecondScreenState_Read(&snap);
    if (!snap.inGame || snap.introCinema) return;
    UI_LOCK();
    if (sUi.tab != SS_TAB_SETTINGS) {
        sUi.tab = sUi.tab == SS_TAB_ITEMS ? SS_TAB_MAP : SS_TAB_ITEMS;
        sUi.questView = SS_QUEST_MAIN;
        sUi.liveItem = sUi.saveConfirm = sUi.saveResult = 0;
        sTapTargetCount = 0;
        RequestTouchRefresh();
    }
    UI_UNLOCK();
}

bool Port_SecondScreen_3DS_BindSelectedItem(unsigned button) {
    if (button >= 4) return false;
    SecondScreenSnapshot snap;
    Port_SecondScreenState_Read(&snap);
    if (!snap.inGame || snap.introCinema) return false;
    UI_LOCK();
    const unsigned slot = (unsigned)sUi.liveItem - 1;
    const bool selecting = sUi.tab == SS_TAB_ITEMS && sUi.questView == SS_QUEST_MAIN && !sUi.saveConfirm &&
                           slot < SECOND_SCREEN_ITEM_SLOTS && snap.menuItems[slot] != 0;
    if (selecting) {
        sUi.liveItem = 0;
        RequestRefresh();
    }
    UI_UNLOCK();
    if (selecting) Port_SecondScreenState_RequestEquip(snap.menuItems[slot], button);
    return selecting;
}

bool Port_SecondScreen_3DS_BackFromRegion(void) {
    if (!Port_SecondScreen_3DS_InGame() || !Port_SecondScreen_3DS_ExitRegion()) return false;
    RequestTouchRefresh();
    return true;
}

bool Port_SecondScreen_3DS_BeginRegionDrag(int x, int y) {
    return BottomFrameState3DS_VisibleModeMatches(&sFrameState, 1) &&
           Port_SecondScreen_3DS_DragRegion(x, y, 0, 0);
}

void Port_SecondScreen_3DS_OpenPauseSettings(bool updates) {
    const int mode = updates ? SS_PAUSE_SETTINGS_UPDATE : SS_PAUSE_SETTINGS_ROOT;
    UI_LOCK();
    if (sUi.pauseSettingsMode == mode) {
        UI_UNLOCK();
        return;
    }
    RequestTouchRefresh();
    sTapTargetCount = 0;
    sUi.resumeTab = sUi.tab;
    sUi.pauseSettingsMode = mode;
    sUi.tab = SS_TAB_SETTINGS;
    sUi.settingsPage = updates ? SS_SETTINGS_UPDATE : SS_SETTINGS_ROOT;
    sUi.armedRing = 0;
    sUi.loadConfirmActive = 0;
    sUi.randoConfirmActive = 0;
    UI_UNLOCK();
    if (updates)
        BeginUpdatePage();
}

bool Port_SecondScreen_3DS_PauseSettingsOpen(void) {
    UI_LOCK();
    const bool open = sUi.pauseSettingsMode != SS_PAUSE_SETTINGS_NONE;
    UI_UNLOCK();
    return open;
}

void Port_SecondScreen_3DS_ClosePauseSettings(void) {
    UI_LOCK();
    const bool idleOpen = __atomic_exchange_n(&sIdleSettingsOpen, 0, __ATOMIC_ACQ_REL) != 0;
    if (sUi.pauseSettingsMode != SS_PAUSE_SETTINGS_NONE || idleOpen) {
        RequestTouchRefresh();
        sTapTargetCount = 0;
        sUi.pauseSettingsMode = SS_PAUSE_SETTINGS_NONE;
        sUi.tab = idleOpen ? SS_TAB_MAP : sUi.resumeTab;
        sUi.settingsPage = SS_SETTINGS_ROOT;
        sUi.loadConfirmActive = 0;
        sUi.randoConfirmActive = 0;
    }
    UI_UNLOCK();
}

static void ResetIdleOnlyState(void) {
    UI_LOCK();
    sTapTargetCount = 0;
    sUi.mapLive = 0;
    sMapCameraMoving = 0;
    sUi.armedRing = 0;
    sUi.liveItem = sUi.saveConfirm = sUi.saveResult = 0;
    sUi.floorPreview = SS_NO_FLOOR;
    sUi.playerFloorDisp = SS_NO_FLOOR;
    sUi.regionState = SS_REGION_OFF;
    sUi.questView = SS_QUEST_MAIN;
    if (sUi.pauseSettingsMode == SS_PAUSE_SETTINGS_NONE) {
        sUi.settingsPage = SS_SETTINGS_ROOT;
        sUi.loadConfirmActive = 0;
        sUi.randoConfirmActive = 0;
        if (sIdleOwnsSettingsTab) sUi.tab = SS_TAB_MAP;
    }
    sIdleOwnsSettingsTab = 0;
    UI_UNLOCK();
    sLastFix.valid = 0;
    sCam.valid = 0;
}

static void PaintIntroCinema(uint32_t* pixels, int width, int height, int stride) {
    __atomic_store_n(&sIdleSettingsOpen, 0, __ATOMIC_RELEASE);
    ResetIdleOnlyState();
    SSurf surface = { pixels, width, height, stride };
    FillRect(&surface, 0, 0, width, height, RGB(0, 0, 0));
}

uint32_t Port_SecondScreen_3DS_PaintInto(uint32_t* pixels, int width, int height, int strideInPixels,
                                        const SecondScreenSnapshot* snap, uint32_t tick) {
    if (!pixels || !snap || width <= 0 || height <= 0 || strideInPixels < width) return 0;

    const uint32_t refreshRequest = BeginRefresh();
    const bool settingsOpen = __atomic_load_n(&sIdleSettingsOpen, __ATOMIC_ACQUIRE) != 0;

    if (snap->introCinema) {
        PaintIntroCinema(pixels, width, height, strideInPixels);
        FinishRefresh(refreshRequest);
        return refreshRequest;
    }

    if (snap->inGame) {
        if (settingsOpen) __atomic_store_n(&sIdleSettingsOpen, 0, __ATOMIC_RELEASE);
        if (sIdleOwnsSettingsTab) ResetIdleOnlyState();
        Port_SecondScreen_PaintInto(pixels, width, height, strideInPixels, snap, tick);
        FinishRefresh(refreshRequest);
        return refreshRequest;
    }

    if (!settingsOpen) {
        UI_LOCK();
        sUi.tab = SS_TAB_MAP;
        sUi.pauseSettingsMode = SS_PAUSE_SETTINGS_NONE;
        UI_UNLOCK();
        /* Preserve every reset the shared non-game cinema path performed:
         * stale map fixes, armed items and submenu state must not cross a
         * title/file-select boundary even though the picture is replaced. */
        ResetIdleOnlyState();
        BottomIdle3DS_Paint(pixels, width, height, strideInPixels, tick, Platform3DS_IsNew3DS());
        FinishRefresh(refreshRequest);
        return refreshRequest;
    }

    /* The existing Settings painter only needs the inGame gate to become
     * visible; its values come from persistent port configuration, not from
     * gameplay coordinates. A zeroed title snapshot is therefore safe. */
    if (!sIdleOwnsSettingsTab) ResetIdleOnlyState();
    SecondScreenSnapshot settingsSnapshot = *snap;
    settingsSnapshot.inGame = 1;
    UI_LOCK();
    if (sUi.pauseSettingsMode == SS_PAUSE_SETTINGS_NONE) {
        sUi.tab = SS_TAB_SETTINGS;
        sIdleOwnsSettingsTab = 1;
    }
    UI_UNLOCK();
    Port_SecondScreen_PaintInto(pixels, width, height, strideInPixels, &settingsSnapshot, tick);

    FinishRefresh(refreshRequest);
    return refreshRequest;
}

void Port_SecondScreen_3DS_DrawPauseTitle(uint32_t* pixels, int stride, const char* title) {
    if (!title || !*title) return;
    SSurf surface = { pixels, 320, 240, stride };
    DrawLabelPlate(&surface, -3, 8, 112, 31, SS_PLATE_TITLE);
    MenuTextCenteredFace(&surface, title, 55, 19, 1, SS_TEXT_PLATE_INK, SS_FACE_LARGE);
}

void Port_SecondScreen_3DS_DrawPauseButton(uint32_t* pixels, int stride, int x, int y, int width, int height,
                                           SecondScreenPauseIcon3DS icon) {
    enum { ICON_SIZE = 13 };
    static const char* const art[][ICON_SIZE] = {
        [SS_PAUSE_ICON_SAVE] = {
            "#########....", "##########...", "##.....####..", "##....######.", "##.....######",
            "#############", "#############", "###.......###", "###.......###", "###.......###",
            "###.......###", "#############", "#############",
        },
        [SS_PAUSE_ICON_SETTINGS] = {
            ".....###.....", "..#..###..#..", ".###.###.###.", "..#########..", "...###.###...",
            "#####...#####", "####.....####", "#####...#####", "...###.###...", "..#########..",
            ".###.###.###.", "..#..###..#..", ".....###.....",
        },
        [SS_PAUSE_ICON_BACK] = {
            ".............", ".....#.......", "....##.......", "...###.......", "..###........",
            ".############", "#############", ".############", "..###........", "...###.......",
            "....##.......", ".....#.......", ".............",
        },
    };
    SSurf surface = { pixels, 320, 240, stride };
    DrawLabelPlate(&surface, x, y, x + width, y + height, SS_PLATE_TITLE);
    const int iconX = x + (width - ICON_SIZE) / 2;
    const int iconY = y + (height - ICON_SIZE) / 2;
    DrawPixelArt(&surface, art[icon], ICON_SIZE, iconX, iconY + 1, 1, COL_PLATE_TITLE_OUTER);
    DrawPixelArt(&surface, art[icon], ICON_SIZE, iconX, iconY, 1, RGB(255, 250, 255));
}

void Port_SecondScreen_3DS_ResetFrameState(void) {
    BottomFrameState3DS_Reset(&sFrameState);
    sDeferredTap.frames = 0;
}

uint32_t Port_SecondScreen_3DS_RequestRefresh(void) {
    return RequestRefresh();
}

void Port_SecondScreen_3DS_MarkSubmitted(uint32_t generation, int inGame) {
    BottomFrameState3DS_MarkSubmitted(&sFrameState, generation, inGame);
}

void Port_SecondScreen_3DS_PromoteSubmitted(void) {
    BottomFrameState3DS_PromoteSubmitted(&sFrameState);
    if (sDeferredTap.frames == 0) return;
    const bool inGame = Port_SecondScreen_3DS_InGame();
    if (!BottomFrameState3DS_VisibleModeMatches(&sFrameState, inGame)) {
        sDeferredTap.frames = 0;
    } else if (BottomFrameState3DS_CanDispatchTouch(&sFrameState, inGame)) {
        sDeferredTap.frames = 0;
        Port_SecondScreen_3DS_OnTap(sDeferredTap.x, sDeferredTap.y, sDeferredTap.longPress);
    } else {
        --sDeferredTap.frames;
    }
}

void Port_SecondScreen_3DS_GetFrameStats(BottomFrameState3DSStats* out) {
    BottomFrameState3DS_GetStats(&sFrameState, out);
}

void Port_SecondScreen_3DS_OnTap(int x, int y, int longPress) {
    SecondScreenSnapshot currentSnapshot;
    Port_SecondScreenState_Read(&currentSnapshot);
    if (currentSnapshot.introCinema) return;
    /* Painting publishes the compositor's new hit boxes before its texture
     * can be submitted or shown.  Dispatch only when those hit boxes belong
     * to the confirmed visible generation and task; otherwise fail closed
     * while leaving the per-tick HID polling cadence untouched. */
    if (!BottomFrameState3DS_CanDispatchTouch(&sFrameState, currentSnapshot.inGame != 0)) {
        /* A same-task hidden paint is already on its way to the display; an
         * extra request would only lengthen this fail-closed interval.  Task
         * transitions (and the pre-first-frame state) still need a paint. */
        if (!BottomFrameState3DS_VisibleModeMatches(&sFrameState, currentSnapshot.inGame != 0)) {
            RequestTouchRefresh();
        } else if (!__atomic_load_n(&sFrameState.touchBlocked, __ATOMIC_ACQUIRE)) {
            /* A periodic repaint may finish just before a touch releases.
             * Let it become visible before routing the tap to its hitboxes. */
            sDeferredTap.x = x;
            sDeferredTap.y = y;
            sDeferredTap.longPress = longPress;
            sDeferredTap.frames = 8;
        }
        return;
    }
    if (currentSnapshot.inGame) {
        Port_SecondScreen_OnTap(x, y, longPress);
        RequestTouchRefresh();
        return;
    }

    if (!__atomic_load_n(&sIdleSettingsOpen, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&sIdleSettingsOpen, 1, __ATOMIC_RELEASE);
        RequestTouchRefresh();
        return;
    }

    Port_SecondScreen_OnTap(x, y, longPress);
    RequestTouchRefresh();
}

int Port_SecondScreen_3DS_NeedsRefresh(void) {
    return BottomFrameState3DS_NeedsPaint(&sFrameState);
}

int Port_SecondScreen_3DS_NeedsPeriodicRefresh(const SecondScreenSnapshot* snap, uint32_t tick,
                                               uint32_t paintedTick, int32_t width,
                                               int32_t height) {
    if (!snap || snap->introCinema) return 0;

    const bool idleSettings = __atomic_load_n(&sIdleSettingsOpen, __ATOMIC_ACQUIRE) != 0;
    if (!snap->inGame && !idleSettings) {
        /* Match ALttP PR #32: Old 3DS keeps a steady gold card. */
        return Platform3DS_IsNew3DS();
    }

    int tab;
    int settingsPage;
    uint32_t dumpFlashUntil;
    uint32_t lastTick;
    int regionState;
    int floorPreview;
    int cameraMoving;
    UI_LOCK();
    tab = sUi.tab;
    settingsPage = sUi.settingsPage;
    dumpFlashUntil = sUi.dumpFlashUntil;
    lastTick = sUi.lastTick;
    regionState = sUi.regionState;
    floorPreview = sUi.floorPreview;
    cameraMoving = sMapCameraMoving;
    UI_UNLOCK();

    if (tab == SS_TAB_MAP) {
        if (!Port_Config_BottomMapSkip()) {
            return 1; /* the old unconditional repaint, kept for the A/B */
        }
        /* The camera glide, region bracket and floor preview advance inside
         * paint. Keep repainting until they settle, even when the engine
         * snapshot and marker animation signature have not changed. */
        if (cameraMoving || regionState == SS_REGION_BRACKET || floorPreview != SS_NO_FLOOR) {
            return 1;
        }
        if (BottomMapAnim_NeedsPaint(tick, paintedTick,
                                     (snap->areaFlags & SECOND_SCREEN_AR_IS_DUNGEON) != 0, width,
                                     height)) {
            return 1;
        }
        return 0;
    }
    if (tab == SS_TAB_ITEMS) {
        return 0; /* The live inventory cursor is static. */
    }
    if (tab != SS_TAB_SETTINGS) {
        /* Quest status and its two detail lists have no selection cursor. */
        return 0;
    }
    if (settingsPage == SS_SETTINGS_UPDATE) return 1;
    if (settingsPage == SS_SETTINGS_OVERLAY) {
        /* Live diagnostics deliberately retain their slower refresh rate. */
        return 1;
    }
    if (settingsPage == SS_SETTINGS_DEVELOPER &&
        (int32_t)(dumpFlashUntil - lastTick) > 0) {
        /* Keep painting until the post-dump DONE indicator expires. */
        return 1;
    }
    return 0;
}

int Port_SecondScreen_3DS_SnapshotChangeNeedsRefresh(const SecondScreenSnapshot* previous,
                                                     const SecondScreenSnapshot* current,
                                                     int previousValid) {
    if (!previous || !current || !previousValid) return 1;
    if (previous->inGame != current->inGame || previous->introCinema != current->introCinema) return 1;
    if (memcmp(previous, current, sizeof(*current)) == 0) return 0;

    /* Gameplay snapshots continue changing while a settings sheet covers the
     * entire panel, but none of those engine fields are drawn there.  Config
     * mutations originate in the tap handler and request an immediate paint;
     * the overlay and dump indicator are handled by NeedsPeriodicRefresh. */
    if (!current->inGame) return 0;
    int tab;
    UI_LOCK();
    tab = sUi.tab;
    UI_UNLOCK();
    return tab != SS_TAB_SETTINGS;
}
