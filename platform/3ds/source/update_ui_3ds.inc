/* Included by the existing compositor: reuse Minish Cap's runtime-decoded
 * stone plate, red header chip, pale buttons and native message fonts. */
#include "updater.h"
#include <ctype.h>

static bool sUpdateConfirm, sUpdateNotes;
static unsigned sUpdatePage, sUpdatePages = 1;

static void UpdateUI_Reset(void) {
    UI_LOCK();
    sUpdateConfirm = sUpdateNotes = false;
    sUpdatePage = 0;
    UI_UNLOCK();
}

bool Port_SecondScreen_3DS_UpdateOpen(void) {
    UI_LOCK();
    bool open = sUi.tab == SS_TAB_SETTINGS && sUi.settingsPage == SS_SETTINGS_UPDATE;
    UI_UNLOCK();
    return open;
}

/* Uppercase ASCII only. UTF-8 continuation bytes must pass through untouched,
 * otherwise every Chinese caption turns into mojibake. */
static void UpdateUpperAscii(char* s) {
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80)
            *s = (char)toupper(c);
    }
}

static void UpdateSmallText(const SSurf* s, const char* text, int x, int y, int style) {
    char label[128];
    snprintf(label, sizeof(label), "%s", text);
    UpdateUpperAscii(label);
    text = label;
    if (!Port_SecondScreenTheme_DrawBigText(s->px, s->w, s->h, s->stride, x, y, 1, style, text))
        DrawTextStr(s, text, x, y + 4, 1, Port_SecondScreenTheme_Color(SSC_MENU_INK), 0);
}

static int UpdateTextWidth(const char* text) {
    char label[128];
    snprintf(label, sizeof(label), "%s", text);
    UpdateUpperAscii(label);
    text = label;
    /* TextWidthPx() is static to port_second_screen.c and not visible from this
     * translation unit; BigTextWidth already reports the real advance. */
    return Port_SecondScreenTheme_BigTextWidth(text, 1);
}

static void UpdateCenteredText(const SSurf* s, const char* text, int cx, int y) {
    UpdateSmallText(s, text, cx - UpdateTextWidth(text) / 2, y, SS_TEXT_NAVY);
}

static void UpdateButton(const SSurf* s, TargetList* tl, float x0, float y0, float x1, float y1, const char* text,
                         int action, bool pressed, float u, int ts) {
    DrawMenuButton(s, x0, y0, x1, y1, "", pressed, 0, u, ts);
    UpdateSmallText(s, text, (int)((x0 + x1 - UpdateTextWidth(text)) / 2), (int)((y0 + y1) / 2 - 8), SS_TEXT_NAVY);
    if (action)
        AddTarget(tl, x0, y0, x1, y1, action, 0);
}

/* Cache formatted notes on the bottom-screen paint thread. The formatter
 * strips markup and non-ASCII text and wraps to 42 columns. Wrap again to
 * the actual pixel width so wide letters stay inside the panel. */
static void PaintUpdateNotes(const SSurf* s, TargetList* tl, const UpdateStatus* st, float x0, float y0, float x1,
                             float y1, float u, int ts) {
    enum { MAX_LINES = 512, LINE_HEIGHT = 17, NAV_HEIGHT = 24 };
    static char lines[MAX_LINES][43];
    static unsigned count, revision = UINT32_MAX;
    static int cachedWidth;
    const int textWidth = (int)(x1 - x0) - 12;
    if (count == 0 || revision != st->revision || cachedWidth != textWidth) {
        static char body[12289], formatted[MAX_LINES][43];
        Updater_GetNotes(body, sizeof(body));
        const unsigned rows = Update_FormatNotes(body, formatted, MAX_LINES);
        count = 0;
        for (unsigned i = 0; i < rows && count < MAX_LINES; ++i) {
            const char* text = formatted[i];
            do {
                char part[43];
                const size_t length = strlen(text);
                size_t take = length;
                memcpy(part, text, take + 1);
                while (take > 1 && UpdateTextWidth(part) > textWidth) part[--take] = 0;
                if (take < length) {
                    const char* space = strrchr(part, ' ');
                    if (space && space != part) take = (size_t)(space - part);
                }
                part[take] = 0;
                memcpy(lines[count++], part, take + 1);
                text += take;
                while (*text == ' ') ++text;
            } while (*text && count < MAX_LINES);
        }
        revision = st->revision;
        cachedWidth = textWidth;
    }

    const int cx = (int)((x0 + x1) / 2);
    const int wellTop = (int)y0 + 19;
    const int navTop = (int)y1 - NAV_HEIGHT;
    const int textTop = wellTop + 4;
    const int available = navTop - 4 - textTop;
    if (available < LINE_HEIGHT) return;
    const unsigned perPage = (unsigned)(available / LINE_HEIGHT);
    const unsigned pages = count ? (count + perPage - 1) / perPage : 1;
    UI_LOCK();
    sUpdatePages = pages;
    if (sUpdatePage >= pages) sUpdatePage = pages - 1;
    const unsigned page = sUpdatePage;
    UI_UNLOCK();

    char title[64];
    snprintf(title, sizeof(title), "更新日志 %s", st->version);
    UpdateCenteredText(s, title, cx, (int)y0);
    Port_SecondScreenTheme_DrawWell(s->px, s->w, s->h, s->stride, (int)x0, wellTop, (int)(x1 - x0),
                                   navTop - 3 - wellTop, 1);
    const unsigned start = page * perPage;
    for (unsigned i = start; i < count && i < start + perPage; ++i)
        UpdateSmallText(s, lines[i], (int)x0 + 6, textTop + (int)(i - start) * LINE_HEIGHT, SS_TEXT_INK);
    if (page > 0)
        UpdateButton(s, tl, x0, navTop, x0 + 68, y1, "上一页", SS_ACT_UPDATE_PREV, false, u, ts);
    if (page + 1 < pages)
        UpdateButton(s, tl, x1 - 68, navTop, x1, y1, "下一页", SS_ACT_UPDATE_NEXT, false, u, ts);
    char footer[32];
    snprintf(footer, sizeof(footer), "第 %u / %u 页", page + 1, pages);
    UpdateCenteredText(s, footer, cx, navTop + (NAV_HEIGHT - 16) / 2);
}

static void PaintUpdatePanel(const SSurf* s, TargetList* tl, float x0, float y0, float x1, float y1, float u, int ts) {
    UpdateStatus st;
    Updater_GetStatus(&st);
    bool confirm, notes;
    UI_LOCK();
    confirm = sUpdateConfirm;
    notes = sUpdateNotes;
    UI_UNLOCK();
    const bool busy = Updater_Busy();
    if (notes && !confirm && !busy && st.version[0]) {
        PaintUpdateNotes(s, tl, &st, x0, y0, x1, y1, u, ts);
        return;
    }
    float row = 26, gap = 4;
    int cx = (int)((x0 + x1) / 2);
    char text[96];
    snprintf(text, sizeof(text), "通道：%s", st.prerelease ? "预发布" : "稳定版");
    UpdateButton(s, tl, x0, y0, x1, y0 + row, text, SS_ACT_UPDATE_CHANNEL, false, u, ts);
    UpdateButton(s, tl, x0, y0 + row + gap, x1, y0 + 2 * row + gap, st.version[0] ? st.version : "未选择版本",
                 SS_ACT_UPDATE_RELEASE, false, u, ts);
    const char* message = confirm ? "下载并安装" : st.message;
    if (!message[0])
        message = "检查更新";
    // Long network errors are split at words inside the actual panel width.
    char first[80], second[80] = "";
    snprintf(first, sizeof(first), "%s", message);
    if (UpdateTextWidth(first) > x1 - x0) {
        char* split = strrchr(first, ' ');
        while (split && UpdateTextWidth(first) > x1 - x0) {
            *split = 0;
            snprintf(second, sizeof(second), "%s", message + strlen(first) + 1);
            if (UpdateTextWidth(first) <= x1 - x0)
                break;
            split = strrchr(first, ' ');
        }
    }
    UpdateCenteredText(s, first, cx, (int)(y0 + 60));
    if (second[0])
        UpdateCenteredText(s, second, cx, (int)(y0 + 75));
    else if (confirm || st.version[0])
        UpdateCenteredText(s, confirm ? "未保存的进度会丢失" : "点选版本查看日志", cx, (int)(y0 + 78));
    if (st.state == UPDATE_DOWNLOADING || st.state == UPDATE_VERIFYING || st.state == UPDATE_INSTALLING) {
        FillRect(s, (int)x0, (int)(y0 + 103), (int)x1, (int)(y0 + 111),
                 Port_SecondScreenTheme_Color(SSC_MENU_STONE_DARK));
        FillRect(s, (int)x0, (int)(y0 + 103), (int)(x0 + (x1 - x0) * st.progress / 100), (int)(y0 + 111),
                 Port_SecondScreenTheme_Color(SSC_RUPEE_GREEN));
    } else if (!confirm) {
        snprintf(text, sizeof(text), "已安装：v%s", TMC_PORT_VERSION);
        UpdateCenteredText(s, text, cx, (int)(y0 + 103));
    }
    const char* action = confirm                         ? "安装更新"
                         : st.state == UPDATE_INSTALLING ? "请稍候"
                         : busy                          ? "取消"
                         : st.state == UPDATE_AVAILABLE  ? "下载更新"
                         : st.state == UPDATE_DONE       ? "更新已安装"
                                                         : "检查更新";
    UpdateButton(s, tl, x0, y1 - 26, x1, y1, action, SS_ACT_UPDATE_ACTION, confirm, u, ts);
}

static bool HandleUpdateTap(int action, int arg) {
    if (!Port_SecondScreen_3DS_UpdateOpen())
        return false;
    UpdateStatus st;
    Updater_GetStatus(&st);
    if (st.state == UPDATE_INSTALLING || st.state == UPDATE_DONE)
        return true;
    bool busy = Updater_Busy();
    if (action == SS_ACT_SETTINGS_BACK) {
        UI_LOCK();
        const bool internalPage = sUpdateConfirm || sUpdateNotes;
        sUpdateConfirm = sUpdateNotes = false;
        sUpdatePage = 0;
        UI_UNLOCK();
        if (internalPage && !busy)
            return true;
        Updater_Cancel();
        if (busy)
            return true; // Wait for the worker before closing the update page.
        UpdateUI_Reset();
        return false;
    }
    if (action == SS_ACT_TAB) {
        if (busy)
            return true;
        UpdateUI_Reset();
        return false;
    }
    if (action == SS_ACT_UPDATE_ACTION) {
        if (busy) {
            Updater_Cancel();
            return true;
        }
        UI_LOCK();
        bool confirm = sUpdateConfirm;
        sUpdateConfirm = false;
        UI_UNLOCK();
        if (confirm && st.state == UPDATE_AVAILABLE)
            Updater_Download();
        else if (st.state == UPDATE_AVAILABLE) {
            UI_LOCK();
            sUpdateConfirm = true;
            UI_UNLOCK();
        } else
            Updater_Check();
        return true;
    }
    if (busy)
        return true;
    switch (action) {
        case SS_ACT_UPDATE_CHANNEL:
            UpdateUI_Reset();
            Updater_SetChannel(!st.prerelease);
            break;
        case SS_ACT_UPDATE_RELEASE:
            if (st.version[0]) {
                UI_LOCK();
                sUpdateNotes = true;
                sUpdatePage = 0;
                sUpdatePages = 1;
                UI_UNLOCK();
            }
            break;
        case SS_ACT_UPDATE_PREV:
            UI_LOCK();
            if (sUpdatePage)
                --sUpdatePage;
            UI_UNLOCK();
            break;
        case SS_ACT_UPDATE_NEXT:
            UI_LOCK();
            if (sUpdatePage + 1 < sUpdatePages)
                ++sUpdatePage;
            UI_UNLOCK();
            break;
        default:
            return true;
    }
    return true;
}
