#ifndef TMC_PORT_SECOND_SCREEN_3DS_H
#define TMC_PORT_SECOND_SCREEN_3DS_H

#include <stdint.h>
#include <stdbool.h>

#include "bottom_frame_state_3ds.h"
#include "port_second_screen_state.h"

#ifdef __cplusplus
extern "C" {
#endif

bool Port_SecondScreen_3DS_UpdateOpen(void);
bool Port_SecondScreen_3DS_PaintUpdateTop(uint32_t* pixels, int stride);

/* Main-thread routing for the existing compositor while the native menu is paused. */
void Port_SecondScreen_3DS_OpenPauseSettings(bool updates);
bool Port_SecondScreen_3DS_PauseSettingsOpen(void);
void Port_SecondScreen_3DS_ClosePauseSettings(void);
void Port_SecondScreen_3DS_DrawPauseTitle(uint32_t* pixels, int stride, const char* title);
void Port_SecondScreen_3DS_DrawCombinedCollection(uint32_t* pixels, int stride, const SecondScreenSnapshot* snap);
/* IDs: bag/heart/skills/shells (0–3), diamond left/top/right/bottom (4–7),
 * passive gear left to right (8–10), carried quest-item tray (11). */
int Port_SecondScreen_3DS_CombinedCollectionTap(int x, int y);
bool Port_SecondScreen_3DS_CombinedCollectionRect(int id, int* x, int* y, int* width, int* height);

uint32_t Port_SecondScreen_3DS_PaintInto(uint32_t* pixels, int width, int height, int strideInPixels,
                                        const SecondScreenSnapshot* snap, uint32_t tick);
void Port_SecondScreen_3DS_ResetFrameState(void);
uint32_t Port_SecondScreen_3DS_RequestRefresh(void);
void Port_SecondScreen_3DS_MarkSubmitted(uint32_t generation, int inGame);
void Port_SecondScreen_3DS_PromoteSubmitted(void);
void Port_SecondScreen_3DS_GetFrameStats(BottomFrameState3DSStats* out);
void Port_SecondScreen_3DS_OnTap(int x, int y, int longPress);
int Port_SecondScreen_3DS_NeedsRefresh(void);
/* `tick` is the free-running animation tick, `paintedTick` the tick the last
 * scheduled paint used; both are required for the MAP-tab skip signature. */
int Port_SecondScreen_3DS_NeedsPeriodicRefresh(const SecondScreenSnapshot* snap, uint32_t tick,
                                               uint32_t paintedTick, int32_t width,
                                               int32_t height);
int Port_SecondScreen_3DS_SnapshotChangeNeedsRefresh(const SecondScreenSnapshot* previous,
                                                     const SecondScreenSnapshot* current,
                                                     int previousValid);

#ifdef __cplusplus
}
#endif

#endif
