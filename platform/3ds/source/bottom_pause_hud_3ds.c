#include "bottom_pause_hud_3ds.h"

#include "item_ids.h"
#include "port_second_screen_render.h"
#include "port_second_screen_theme.h"

enum {
    SCREEN_WIDTH = 320,
    SCREEN_HEIGHT = 240,
    STATUS_CENTER_X = 265,
    HEART_SIZE = 8,
    HEARTS_PER_ROW = 10,
    ITEM_SIZE = 16,
};

/* Theme sprites already contain the renderer's RGBA8888 pixel values. */
static void DrawSprite(uint32_t* pixels, int stride, int width, int height, int x, int y, int spriteId) {
    const SecondScreenThemeSprite* sprite = Port_SecondScreenTheme_Get(spriteId);
    if (sprite == NULL) return;

    for (int sy = 0; sy < sprite->h; ++sy) {
        if (y + sy < 0 || y + sy >= height) continue;
        for (int sx = 0; sx < sprite->w; ++sx) {
            if (x + sx < 0 || x + sx >= width) continue;
            const uint32_t color = sprite->px[sy * sprite->w + sx];
            if (color >> 24) pixels[(y + sy) * stride + x + sx] = color;
        }
    }
}

static void DrawStatusLabel(uint32_t* pixels, int stride, const char* label, int centerY) {
    const int width = Port_SecondScreenTheme_BigTextWidth(label, 1);
    const int y = Port_SecondScreenTheme_InkTopForCenter(centerY, 1, SS_FACE_BODY);
    Port_SecondScreenTheme_DrawBigText(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, stride,
                                      STATUS_CENTER_X - width / 2, y, 1, SS_TEXT_INK, label);
}

static void DrawHearts(uint32_t* pixels, int stride, const SecondScreenSnapshot* snap) {
    int hearts = snap->maxHealth / 8;
    if (hearts > 20) hearts = 20;
    const int quarters = snap->health == 1 ? 1 : snap->health / 2;
    for (int i = 0; i < hearts; ++i) {
        const int first = i / HEARTS_PER_ROW * HEARTS_PER_ROW;
        const int remaining = hearts - first;
        const int rowSize = remaining < HEARTS_PER_ROW ? remaining : HEARTS_PER_ROW;
        const int x = STATUS_CENTER_X - rowSize * HEART_SIZE / 2 + (i - first) * HEART_SIZE;
        const int y = 77 + i / HEARTS_PER_ROW * (HEART_SIZE + 2);
        const int fill = quarters - i * 4;
        const int sprite = fill >= 4 ? SST_HEART_FULL : fill <= 0 ? SST_HEART_EMPTY : SST_HEART_EMPTY + fill;
        DrawSprite(pixels, stride, SCREEN_WIDTH, SCREEN_HEIGHT, x, y, sprite);
    }
}

static void DrawCounter(uint32_t* pixels, int stride, int y, int icon, unsigned value, int minDigits, int yellow) {
    int digits = 1;
    for (unsigned n = value; n >= 10u; n /= 10u) ++digits;
    if (digits < minDigits) digits = minDigits;
    const int width = ITEM_SIZE + 4 + digits * 8;
    const int left = STATUS_CENTER_X - width / 2;
    DrawSprite(pixels, stride, SCREEN_WIDTH, SCREEN_HEIGHT, left, y, icon);
    Port_SecondScreenTheme_DrawHudNumber(pixels, SCREEN_WIDTH, SCREEN_HEIGHT, stride,
                                        left + width, y, 1, value, minDigits, yellow);
}

void BottomPauseHud3DS_DrawStatus(uint32_t* pixels, int stride, const SecondScreenSnapshot* snap) {
    if (pixels == NULL || stride < SCREEN_WIDTH || snap == NULL || !snap->inGame) return;

    DrawStatusLabel(pixels, stride, "生命", 60);
    DrawHearts(pixels, stride, snap);
    DrawCounter(pixels, stride, 110, SST_RUPEE_WALLET0 + (snap->walletType & 3), snap->rupees, 3,
                snap->walletMax != 0 && snap->rupees >= snap->walletMax);

    const uint8_t keyFlags = SECOND_SCREEN_AR_IS_DUNGEON | SECOND_SCREEN_AR_HAS_KEYS;
    if ((snap->areaFlags & keyFlags) == keyFlags && snap->dungeonIdx < 0x10) {
        const uint32_t color = Port_SecondScreenTheme_Color(SSC_MENU_STONE_DARK);
        for (int x = 234; x < 289; ++x) pixels[142 * stride + x] = color;
        DrawStatusLabel(pixels, stride, "地牢钥匙", 154);
        DrawCounter(pixels, stride, 166, SST_KEY, snap->dungeonKeys, 2, 0);
    }
}

static void DrawEquipmentSlot(uint32_t* pixels, int stride, int left, int right, int badge,
                              const SecondScreenSnapshot* snap, uint8_t item) {
    const int width = right - left;
    const int height = BOTTOM_PAUSE_HUD_EQUIPMENT_Y1 - BOTTOM_PAUSE_HUD_EQUIPMENT_Y0;
    uint32_t* slot = pixels + BOTTOM_PAUSE_HUD_EQUIPMENT_Y0 * stride + left;
    if (badge >= 0) {
        DrawSprite(slot, stride, width, height, 1, 4, badge);
    } else {
        const char* label = badge == -1 ? "X" : "Y";
        Port_SecondScreenTheme_DrawMenuButton(slot, width, height, stride, 0, 4, 16, 15, "", 0);
        const int textW = Port_SecondScreenTheme_BigTextWidth(label, 1);
        Port_SecondScreenTheme_DrawBigText(slot, width, height, stride, (16 - textW) / 2,
            Port_SecondScreenTheme_InkTopForCenter(11, 1, SS_FACE_BODY), 1, SS_TEXT_NAVY, label);
    }

    if (item >= ITEM_BOTTLE1 && item <= ITEM_BOTTLE4) {
        const uint8_t content = snap->bottleContents[item - ITEM_BOTTLE1];
        if (content != ITEM_NONE) item = content;
    }
    if (item == ITEM_NONE) return;

    const int iconX = width - ITEM_SIZE - 1;
    Port_SecondScreenRender_DrawItemIcon(slot, width, height, stride, iconX, 1, 1, item);
    int ammo = Port_SecondScreenItemAmmo(snap, item);
    if (ammo >= 0) {
        if (ammo > 99) ammo = 99;
        DrawSprite(slot, stride, width, height, iconX, 16, SST_SMALL_TENS_0 + ammo / 10);
        DrawSprite(slot, stride, width, height, iconX + 8, 16, SST_SMALL_ONES_0 + ammo % 10);
    }
}

void BottomPauseHud3DS_DrawEquipment(uint32_t* pixels, int stride, const SecondScreenSnapshot* snap) {
    if (pixels == NULL || stride < SCREEN_WIDTH || snap == NULL || !snap->inGame) return;

    const uint8_t items[] = {snap->equippedA, snap->equippedB, snap->equippedX, snap->equippedY};
    static const int badges[] = {SST_BUTTON_A, SST_BUTTON_B, -1, -2};
    for (int i = 0; i < 4; ++i) {
        const int left = BOTTOM_PAUSE_HUD_EQUIPMENT_X0 + i * BOTTOM_PAUSE_HUD_EQUIPMENT_PITCH;
        DrawEquipmentSlot(pixels, stride, left, left + BOTTOM_PAUSE_HUD_EQUIPMENT_WIDTH, badges[i], snap, items[i]);
    }
}
