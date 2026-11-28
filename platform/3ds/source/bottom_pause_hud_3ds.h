#ifndef BOTTOM_PAUSE_HUD_3DS_H
#define BOTTOM_PAUSE_HUD_3DS_H

#include <stdint.h>

#include "port_second_screen_state.h"

/* Touch rectangles for the equipped-item header. End coordinates are exclusive. */
enum {
    BOTTOM_PAUSE_HUD_A_X0 = 235,
    BOTTOM_PAUSE_HUD_A_X1 = 270,
    BOTTOM_PAUSE_HUD_B_X0 = 274,
    BOTTOM_PAUSE_HUD_B_X1 = 309,
    BOTTOM_PAUSE_HUD_EQUIPMENT_Y0 = 8,
    BOTTOM_PAUSE_HUD_EQUIPMENT_Y1 = 32,
};

/* Paint over the caller's 320×240 RGBA8888 surface. Stride is in pixels.
 * Use a published gameplay snapshot and an initialized second-screen theme. */
void BottomPauseHud3DS_DrawStatus(uint32_t* pixels, int stride, const SecondScreenSnapshot* snap);
void BottomPauseHud3DS_DrawEquipment(uint32_t* pixels, int stride, const SecondScreenSnapshot* snap);

#endif
