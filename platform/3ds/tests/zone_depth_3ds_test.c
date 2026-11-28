/* Host test of the zone depth fixes' arithmetic (platform/3ds/source/zone_depth_3ds.h): the frame's scroll read back from
 * a layer's offset registers, a fix placed in the frame (a room at a positive origin in Full View, scrolled past), the
 * scissor of a batch redrawn for a zone (cut by the batch, by the frame, moved for each eye), empty and negative sizes;
 * the draw order (the copy under the layer in front: its opaque pixels are never covered); the plane a sprite rests
 * on inside a zone (ZoneDepth3DS_PlaneAt, ZoneDepth3DS_SupportPlane);
 * and every line of platform/3ds/source/port_zone_depth.inc parsed through the same macro the game uses and checked.
 *
 *   cc -std=c11 -Wall -Wextra -Iplatform/3ds/source platform/3ds/tests/zone_depth_3ds_test.c -o /tmp/zone_depth_3ds_test
 *   /tmp/zone_depth_3ds_test
 */
#include "zone_depth_3ds.h"

#include <stdio.h>

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr);        \
            return 1;                                                      \
        }                                                                  \
    } while (0)

#define PORT_ZONE_DEPTH(area, room, bg, x, y, w, h, toBg) \
    { (uint8_t)(area), (uint8_t)(room), (uint8_t)(bg), (uint8_t)(toBg), (int16_t)(x), (int16_t)(y), (int16_t)(w), (int16_t)(h) },
static const ZoneDepth3DSFix kTable[] = {
#include "port_zone_depth.inc"
};
#undef PORT_ZONE_DEPTH

static bool same(ZoneDepth3DSRect r, int left, int top, int right, int bottom) {
    return r.left == left && r.top == top && r.right == right && r.bottom == bottom;
}

/* The game's registers for a scroll (UpdateScreenShake): low four bits of (scroll - origin), plus 8 on y. */
static unsigned hofs(int scroll, int origin) { return (unsigned)((scroll - origin) & 0xf); }
static unsigned vofs(int scroll, int origin) { return (unsigned)(((scroll - origin) & 0xf) + ZONE_DEPTH_3DS_VOFS_BIAS); }

static int test_frame_scroll(void) {
    /* Registers in step with the camera: the scroll itself. */
    for (int scroll = -40; scroll < 600; ++scroll) {
        CHECK(ZoneDepth3DS_FrameScroll(scroll, 480, hofs(scroll, 480), 0) == scroll);
        CHECK(ZoneDepth3DS_FrameScroll(scroll, 480, vofs(scroll, 480), ZONE_DEPTH_3DS_VOFS_BIAS) == scroll);
    }
    /* The registers a tick behind (or ahead of) the camera, up to 7 px: the frame's scroll, not the camera's. */
    for (int lag = -8; lag <= 7; ++lag)
        for (int scroll = 100; scroll < 140; ++scroll) {
            CHECK(ZoneDepth3DS_FrameScroll(scroll, 16, hofs(scroll + lag, 16), 0) == scroll + lag);
            CHECK(ZoneDepth3DS_FrameScroll(scroll, 16, vofs(scroll + lag, 16), ZONE_DEPTH_3DS_VOFS_BIAS) == scroll + lag);
        }
    /* Registers wider than four bits (a screen shake's negative offset as a u16) read only their low bits. */
    CHECK(ZoneDepth3DS_FrameScroll(200, 0, (unsigned)(uint16_t)(hofs(198, 0) + 0xfff0u), 0) == 198);
    return 0;
}

static int test_screen_rect(void) {
    const ZoneDepth3DSFix chick = { 0x02, 0x00, 2, 1, 272, 864, 16, 16 };
    ZoneDepth3DSRect r;
    /* Hyrule Town's origin (1488, 1520), the camera at (1600, 2300): the chick's cell at (160, 84) of the frame. */
    CHECK(ZoneDepth3DS_ScreenRect(&chick, 1488 - 1600, 1520 - 2300, &r));
    CHECK(same(r, 160, 84, 176, 100));
    /* Full View with a room narrower than the view: its origin rests at a positive x of the frame. */
    const ZoneDepth3DSFix small = { 0x20, 0x00, 1, 2, 8, 16, 32, 8 };
    CHECK(ZoneDepth3DS_ScreenRect(&small, 72, 0, &r));
    CHECK(same(r, 80, 16, 112, 24));
    /* No area: nothing. */
    const ZoneDepth3DSFix flatW = { 0, 0, 1, 2, 8, 8, 0, 8 }, negH = { 0, 0, 1, 2, 8, 8, 8, -8 };
    CHECK(!ZoneDepth3DS_ScreenRect(&flatW, 0, 0, &r));
    CHECK(!ZoneDepth3DS_ScreenRect(&negH, 0, 0, &r));
    CHECK(!ZoneDepth3DS_FixValid(&flatW) && !ZoneDepth3DS_FixValid(&negH));
    const ZoneDepth3DSFix sameLayer = { 0, 0, 2, 2, 0, 0, 8, 8 }, bg0 = { 0, 0, 0, 1, 0, 0, 8, 8 }, bg4 = { 0, 0, 1, 4, 0, 0, 8, 8 };
    CHECK(!ZoneDepth3DS_FixValid(&sameLayer) && !ZoneDepth3DS_FixValid(&bg0) && !ZoneDepth3DS_FixValid(&bg4));
    CHECK(ZoneDepth3DS_FixValid(&chick));
    return 0;
}

static int test_batch_scissor(void) {
    const ZoneDepth3DSRect zone = { 160, 84, 176, 100 };
    ZoneDepth3DSRect s;
    /* A batch covering the whole 400x240 view: the zone, moved by each eye's shift (left +1, right 0; left +2, right -1). */
    CHECK(ZoneDepth3DS_BatchScissor(&zone, 0, 400, 0, 240, 1, 400, 240, &s) && same(s, 161, 84, 177, 100));
    CHECK(ZoneDepth3DS_BatchScissor(&zone, 0, 400, 0, 240, 0, 400, 240, &s) && same(s, 160, 84, 176, 100));
    CHECK(ZoneDepth3DS_BatchScissor(&zone, 0, 400, 0, 240, 2, 400, 240, &s) && same(s, 162, 84, 178, 100));
    CHECK(ZoneDepth3DS_BatchScissor(&zone, 0, 400, 0, 240, -1, 400, 240, &s) && same(s, 159, 84, 175, 100));
    /* A band of lines and a window region: only their part of the zone, then moved. */
    CHECK(ZoneDepth3DS_BatchScissor(&zone, 0, 400, 90, 120, 1, 400, 240, &s) && same(s, 161, 90, 177, 100));
    CHECK(ZoneDepth3DS_BatchScissor(&zone, 168, 400, 0, 240, 1, 400, 240, &s) && same(s, 169, 84, 177, 100));
    /* Bands that miss the zone: nothing. */
    CHECK(!ZoneDepth3DS_BatchScissor(&zone, 0, 400, 0, 84, 1, 400, 240, &s));
    CHECK(!ZoneDepth3DS_BatchScissor(&zone, 0, 400, 100, 160, 1, 400, 240, &s));
    CHECK(!ZoneDepth3DS_BatchScissor(&zone, 0, 160, 0, 240, 1, 400, 240, &s));
    /* Partly off the frame: cut by the batch's own columns (the layer's place), moved, then cut by the frame. At the
     * left edge a +1 move leaves column 0 to the layer's own unmoved picture, as the layer's moved batches do. */
    const ZoneDepth3DSRect left = { -10, -4, 6, 12 }, right = { 392, 232, 408, 248 };
    CHECK(ZoneDepth3DS_BatchScissor(&left, 0, 400, 0, 240, 1, 400, 240, &s) && same(s, 1, 0, 7, 12));
    CHECK(ZoneDepth3DS_BatchScissor(&left, 0, 400, 0, 240, -1, 400, 240, &s) && same(s, 0, 0, 5, 12));
    CHECK(ZoneDepth3DS_BatchScissor(&right, 0, 400, 0, 240, 1, 400, 240, &s) && same(s, 393, 232, 400, 240));
    /* A 240x160 frame: the same zone at the right edge of a smaller view. */
    CHECK(ZoneDepth3DS_BatchScissor(&right, 0, 240, 0, 160, 1, 240, 160, &s) == false);
    const ZoneDepth3DSRect edge = { 232, 150, 248, 166 };
    CHECK(ZoneDepth3DS_BatchScissor(&edge, 0, 240, 0, 160, 1, 240, 160, &s) && same(s, 233, 150, 240, 160));
    /* Wholly off the frame, or moved off it: nothing. */
    const ZoneDepth3DSRect off = { -20, 10, -4, 26 }, edgeOut = { 399, 10, 400, 26 };
    CHECK(!ZoneDepth3DS_BatchScissor(&off, 0, 400, 0, 240, 1, 400, 240, &s));
    CHECK(!ZoneDepth3DS_BatchScissor(&edgeOut, 0, 400, 0, 240, 1, 400, 240, &s));
    /* Empty or inverted zones (never placed, but the clip must not turn them into something). */
    const ZoneDepth3DSRect empty = { 50, 50, 50, 60 }, inverted = { 60, 60, 50, 50 };
    CHECK(!ZoneDepth3DS_BatchScissor(&empty, 0, 400, 0, 240, 1, 400, 240, &s));
    CHECK(!ZoneDepth3DS_BatchScissor(&inverted, 0, 400, 0, 240, 1, 400, 240, &s));
    return 0;
}

/* The moved copy fills the hole the moved toBg leaves exactly: for each eye the zone's scissor is the hole's place in
 * that layer moved by the same shift, so no column of the hole is left uncovered and none of its neighbours is covered. */
static int test_hole_filled(void) {
    const ZoneDepth3DSRect zone = { 160, 84, 176, 100 };
    for (int shift = -6; shift <= 6; ++shift) {
        ZoneDepth3DSRect s;
        CHECK(ZoneDepth3DS_BatchScissor(&zone, 0, 400, 0, 240, shift, 400, 240, &s));
        CHECK(s.left == zone.left + shift && s.right == zone.right + shift);
        CHECK(s.top == zone.top && s.bottom == zone.bottom);
    }
    return 0;
}

/* The draw order, modelled on one line of pixels (port_ppu_gpu_3ds.c, DrawOutput's colour loop in the world scene): BG2
 * unmoved; the zone's copy of BG2, right after BG2's own batches (its anchor), cut and moved by ZoneDepth3DS_BatchScissor;
 * then BG1, its unmoved copy first (IsGhostBatch) and its moved picture over it. BG1 has a hole and, inside the zone,
 * opaque pixels of its own (the chick's). `zoneLast` draws the copy after BG1 instead, the first version's order. */
enum { LINE = 64, EMPTY = 0 };
static int bg2At(int x) { return 1000 + x; }
static int bg1At(int x) { return (x >= 20 && x < 32 && x != 26 && x != 27) ? 0 : 2000 + x; } /* hole 20..32, opaque 26..27 */

static void drawLine(int out[LINE], const ZoneDepth3DSRect* zone, int shift, bool zoneLast) {
    for (int x = 0; x < LINE; ++x) out[x] = EMPTY;
    for (int x = 0; x < LINE; ++x) out[x] = bg2At(x);
    ZoneDepth3DSRect cut;
    const bool zoneOn = ZoneDepth3DS_BatchScissor(zone, 0, LINE, 0, 1, shift, LINE, 1, &cut);
    if (zoneOn && !zoneLast)
        for (int x = cut.left; x < cut.right; ++x) out[x] = bg2At(x - shift);
    for (int x = 0; x < LINE; ++x)
        if (bg1At(x)) out[x] = bg1At(x);
    for (int x = 0; x < LINE; ++x)
        if (x - shift >= 0 && x - shift < LINE && bg1At(x - shift)) out[x] = bg1At(x - shift);
    if (zoneOn && zoneLast)
        for (int x = cut.left; x < cut.right; ++x) out[x] = bg2At(x - shift);
}

static int test_draw_order(void) {
    /* A generous zone, bigger than BG1's hole (16..36 against 20..32). */
    const ZoneDepth3DSRect zone = { 16, 0, 36, 1 };
    for (int shift = -3; shift <= 3; ++shift) {
        int line[LINE];
        drawLine(line, &zone, shift, false);
        for (int x = 0; x < LINE; ++x) {
            CHECK(line[x] != EMPTY);
            /* BG1's moved opaque pixels are never covered: the chick's own pixels stay on top. */
            if (x - shift >= 0 && x - shift < LINE && bg1At(x - shift)) CHECK(line[x] == bg1At(x - shift));
        }
        /* Inside BG1's moved hole, away from the ghost's one-pixel edge, BG2 is the moved copy (the roof's plane). */
        for (int x = 20 + shift; x < 32 + shift; ++x) {
            const bool ghost = x >= 0 && x < LINE && bg1At(x);
            const bool bg1 = bg1At(x - shift) != 0;
            if (!ghost && !bg1) CHECK(line[x] == bg2At(x - shift));
        }
        /* Outside the zone, nothing changed: BG1 and BG2 as without the fix. */
        int plain[LINE];
        const ZoneDepth3DSRect none = { 0, 0, 0, 0 };
        drawLine(plain, &none, shift, false);
        for (int x = 0; x < LINE; ++x)
            if (x < zone.left + (shift < 0 ? shift : 0) || x >= zone.right + (shift > 0 ? shift : 0)) CHECK(line[x] == plain[x]);
        /* The first version's order (the copy after BG1) covered BG1's opaque pixels inside the zone. */
        if (shift != 0) {
            int late[LINE];
            drawLine(late, &zone, shift, true);
            CHECK(late[26 + shift] != bg1At(26));
        }
    }
    return 0;
}

/* Support with zones: the opaque layers at the sampled pixel are a mask (bit n: BGn). */
static bool maskOpaque(void* context, unsigned bg) { return (*(const unsigned*)context >> bg) & 1u; }

static int test_support(void) {
    /* The log hut: BG1 (the roof, priority 1) has a hole where BG2 (the ground, priority 2) shows; a zone moves BG2's
     * pixels there to BG1's plane. BG3 (priority 3) is the sky. */
    const ZoneDepth3DSZone zones[2] = { { { 160, 80, 200, 128 }, 2, 1 }, { { 0, 0, 8, 8 }, 3, 1 } };
    const unsigned priorities[4] = { 0, 1, 2, 3 };
    unsigned layer = 9, priority = 9, mask;
    CHECK(ZoneDepth3DS_PlaneAt(zones, 2, 2, 170, 90) == 1);
    CHECK(ZoneDepth3DS_PlaneAt(zones, 2, 2, 200, 90) == 2);  /* right edge is outside */
    CHECK(ZoneDepth3DS_PlaneAt(zones, 2, 1, 170, 90) == 1);  /* another layer's zone */
    CHECK(ZoneDepth3DS_PlaneAt(zones, 2, 3, 4, 4) == 1);
    CHECK(ZoneDepth3DS_PlaneAt(zones, 0, 2, 170, 90) == 2);  /* no zones */
    /* A sprite at priority 1 (the chick on the roof) over the hole: BG1 transparent, BG2 opaque inside the zone: it rests
     * on BG1's plane (layer 1, BG1's priority 1). */
    mask = 1u << 2 | 1u << 3;
    CHECK(ZoneDepth3DS_SupportPlane(1, priorities, maskOpaque, &mask, zones, 2, 170, 90, &layer, &priority));
    CHECK(layer == 1 && priority == 1);
    /* The same pixel without the zone: the ground's plane, as before zones. */
    CHECK(ZoneDepth3DS_SupportPlane(1, priorities, maskOpaque, &mask, NULL, 0, 170, 90, &layer, &priority));
    CHECK(layer == 2 && priority == 2);
    /* Outside the zone, on BG2: the ground. */
    CHECK(ZoneDepth3DS_SupportPlane(1, priorities, maskOpaque, &mask, zones, 2, 220, 90, &layer, &priority));
    CHECK(layer == 2 && priority == 2);
    /* On the roof's own opaque pixels inside the zone: BG1 itself (it is in front of BG2, the zone is BG2's). */
    mask = 1u << 1 | 1u << 2 | 1u << 3;
    CHECK(ZoneDepth3DS_SupportPlane(1, priorities, maskOpaque, &mask, zones, 2, 170, 90, &layer, &priority));
    CHECK(layer == 1 && priority == 1);
    /* A sprite at priority 2 (Link) under the roof's opaque pixels: behind BG1, no support; in the hole, BG2 is in the
     * zone, so the zone's plane (the hole is the roof's). */
    CHECK(!ZoneDepth3DS_SupportPlane(2, priorities, maskOpaque, &mask, zones, 2, 170, 90, &layer, &priority));
    mask = 1u << 2 | 1u << 3;
    CHECK(ZoneDepth3DS_SupportPlane(2, priorities, maskOpaque, &mask, zones, 2, 170, 90, &layer, &priority));
    CHECK(layer == 1 && priority == 1);
    /* Nothing opaque: no support. */
    mask = 0;
    CHECK(!ZoneDepth3DS_SupportPlane(1, priorities, maskOpaque, &mask, zones, 2, 170, 90, &layer, &priority));
    /* Equal priorities: the lower layer number is in front (BG2 and BG3 both at 2, BG3's zone does not apply to BG2). */
    const unsigned same[4] = { 0, 1, 2, 2 };
    mask = 1u << 2 | 1u << 3;
    CHECK(ZoneDepth3DS_SupportPlane(1, same, maskOpaque, &mask, zones, 2, 4, 4, &layer, &priority));
    CHECK(layer == 2 && priority == 2);
    mask = 1u << 3;
    CHECK(ZoneDepth3DS_SupportPlane(1, same, maskOpaque, &mask, zones, 2, 4, 4, &layer, &priority));
    CHECK(layer == 1 && priority == 1);
    return 0;
}

static int test_table(void) {
    const unsigned count = (unsigned)(sizeof(kTable) / sizeof(kTable[0]));
    CHECK(count >= 1);
    bool chick = false;
    for (unsigned i = 0; i < count; ++i) {
        const ZoneDepth3DSFix* fix = &kTable[i];
        if (!ZoneDepth3DS_FixValid(fix)) {
            fprintf(stderr, "port_zone_depth.inc entry %u is not valid (bg %u toBg %u size %dx%d)\n", i, fix->bg,
                    fix->toBg, fix->w, fix->h);
            return 1;
        }
        CHECK(fix->x >= 0 && fix->y >= 0);
        /* The log hut's hole in Hyrule Town's BG1 roof (x 272..296, y 856..896) lies inside its fix. */
        if (fix->area == 0x02 && fix->room == 0x00 && fix->bg == 2 && fix->toBg == 1 && fix->x <= 272 && fix->y <= 856 &&
            fix->x + fix->w >= 296 && fix->y + fix->h >= 896)
            chick = true;
    }
    CHECK(chick);
    return 0;
}

int main(void) {
    if (test_frame_scroll() || test_screen_rect() || test_batch_scissor() || test_hole_filled() || test_draw_order() || test_support() || test_table()) return 1;
    printf("zone_depth_3ds_test: ok\n");
    return 0;
}
