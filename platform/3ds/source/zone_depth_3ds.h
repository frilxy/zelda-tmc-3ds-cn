#ifndef ZONE_DEPTH_3DS_H
#define ZONE_DEPTH_3DS_H

/* Zone depth fixes (docs/stereo-3d.md, "Zone depth fixes"): a rectangle of one background layer's pixels drawn at another
 * layer's stereo plane, hand-curated per room in platform/3ds/source/port_zone_depth.inc:
 *
 *   PORT_ZONE_DEPTH(area, room, bg, x, y, w, h, toBg)
 *
 * The table and the per-room lookup are zone_depth_3ds.c (it reads the game's room and registers); the GPU renderer
 * (port_ppu_gpu_3ds.c) draws the rectangles. What follows is the arithmetic both use, header only and with no
 * dependencies so the host test (platform/3ds/tests/zone_depth_3ds_test.c) checks it as it runs. */

#include <stdbool.h>
#include <stdint.h>

enum {
    ZONE_DEPTH_3DS_MAX_PER_ROOM = 16, /* fixes past this many for one room are ignored */
    ZONE_DEPTH_3DS_VOFS_BIAS = 8      /* the game's BG VOFS is the scroll's low four bits plus 8 (UpdateScreenShake) */
};

/* One line of port_zone_depth.inc. */
typedef struct ZoneDepth3DSFix {
    uint8_t area, room, bg, toBg;
    int16_t x, y, w, h;
} ZoneDepth3DSFix;

/* A rectangle in frame pixels, half open: [left, right) x [top, bottom). */
typedef struct ZoneDepth3DSRect {
    int left, top, right, bottom;
} ZoneDepth3DSRect;

/* A fix of the current room placed on this frame: its rectangle before any eye's shift. */
typedef struct ZoneDepth3DSZone {
    ZoneDepth3DSRect rect;
    uint8_t bg, toBg;
} ZoneDepth3DSZone;

/* zone_depth_3ds.c, once a frame before it is built (Stereo3DS_BeginFrame): `world` is the world scene with a depth
 * (3D on); places the current room's fixes in the frame, looking the room's up only when the room changed. */
void ZoneDepth3DS_BeginFrame(bool world);
/* This frame's zones (none outside the world scene, or in a room with no fix). */
unsigned ZoneDepth3DS_Zones(const ZoneDepth3DSZone** zones);

/* A line of the table is usable: both layers BG1-BG3 and different, a rectangle with an area. */
static inline bool ZoneDepth3DS_FixValid(const ZoneDepth3DSFix* fix) {
    return fix->bg >= 1u && fix->bg <= 3u && fix->toBg >= 1u && fix->toBg <= 3u && fix->bg != fix->toBg && fix->w > 0 &&
           fix->h > 0;
}

/* The camera scroll (world pixels) a layer of the shown frame was drawn with. The game writes a room layer's offset
 * register as the low four bits of (scroll - origin), plus `bias` (0 for HOFS, ZONE_DEPTH_3DS_VOFS_BIAS for VOFS) and
 * any screen shake, and the register the frame was drawn from can be a tick behind scroll_x/y while the camera moves:
 * the scroll nearest to `scroll` (within -8..+7) whose low bits match the register's is the frame's. The same split
 * Port_Widescreen_ContentRects reads. */
static inline int ZoneDepth3DS_FrameScroll(int scroll, int origin, unsigned registerOffset, int bias) {
    const int seen = ((int)registerOffset - bias) & 0xf;
    return scroll + ((seen - ((scroll - origin) & 0xf) + 8) & 0xf) - 8;
}

/* Where a fix lies in the frame, unshifted: the room's origin is at (originX, originY) of the frame (origin minus the
 * frame's scroll; with Full View the frame is the whole 400x240 view and scroll_x/y its top-left corner, a room
 * narrower than the view resting at a positive origin). False for a rectangle with no area. */
static inline bool ZoneDepth3DS_ScreenRect(const ZoneDepth3DSFix* fix, int originX, int originY, ZoneDepth3DSRect* out) {
    if (fix->w <= 0 || fix->h <= 0) return false;
    out->left = originX + fix->x;
    out->top = originY + fix->y;
    out->right = out->left + fix->w;
    out->bottom = out->top + fix->h;
    return true;
}

/* The scissor of one batch of the fix's layer redrawn for the zone: the zone's rectangle cut by the batch's own columns
 * [batchLeft, batchRight) and lines [firstLine, lineEnd) (both unshifted, the layer's own place), then moved `shift`
 * pixels sideways (the eye's shift of the plane the zone takes: art and scissor move together, as every layer's do) and
 * cut by the frame (viewWidth x viewHeight). False when nothing of it is left. */
static inline bool ZoneDepth3DS_BatchScissor(const ZoneDepth3DSRect* zone, int batchLeft, int batchRight, int firstLine,
                                             int lineEnd, int shift, int viewWidth, int viewHeight,
                                             ZoneDepth3DSRect* out) {
    int left = zone->left > batchLeft ? zone->left : batchLeft;
    int right = zone->right < batchRight ? zone->right : batchRight;
    int top = zone->top > firstLine ? zone->top : firstLine;
    int bottom = zone->bottom < lineEnd ? zone->bottom : lineEnd;
    left += shift;
    right += shift;
    if (left < 0) left = 0;
    if (right > viewWidth) right = viewWidth;
    if (top < 0) top = 0;
    if (bottom > viewHeight) bottom = viewHeight;
    if (left >= right || top >= bottom) return false;
    *out = (ZoneDepth3DSRect){ left, top, right, bottom };
    return true;
}

/* The layer whose plane a pixel of layer `bg` at frame pixel (x, y) is drawn at: toBg inside a zone of that layer, else
 * the layer's own. No zones: no loop. */
static inline unsigned ZoneDepth3DS_PlaneAt(const ZoneDepth3DSZone* zones, unsigned count, unsigned bg, int x, int y) {
    for (unsigned i = 0; i < count; ++i) {
        const ZoneDepth3DSZone* zone = &zones[i];
        if (zone->bg == bg && x >= zone->rect.left && x < zone->rect.right && y >= zone->rect.top && y < zone->rect.bottom)
            return zone->toBg;
    }
    return bg;
}

/* Whether layer `bg` has an opaque pixel where the support is sampled (the GPU model samples the frame's tiles). */
typedef bool (*ZoneDepth3DSOpaqueFn)(void* context, unsigned bg);

/* Stereo 3D "support" (docs/stereo-3d.md, "Support"): the plane a sprite at priority `objPriority` rests on at frame pixel
 * (x, y). The frontmost opaque BG1-BG3 layer behind it (priority at or after its own; at one priority the lower layer
 * number is in front) is what it stands on; a layer in front of it opaque there means it is behind that layer and takes
 * nothing (false). The plane is that layer's, or, inside a zone depth fix of that layer, the zone's toBg: a sprite on the
 * pixels a zone moves stands where they are seen. `bgPriority` is each layer's priority (BGnCNT & 3). Out: the layer and
 * priority whose depth applies. Layers behind the best one so far are not sampled, as before zones existed. */
static inline bool ZoneDepth3DS_SupportPlane(unsigned objPriority, const unsigned bgPriority[4], ZoneDepth3DSOpaqueFn opaque,
                                             void* context, const ZoneDepth3DSZone* zones, unsigned zoneCount, int x, int y,
                                             unsigned* layer, unsigned* priority) {
    int best = -1;
    unsigned bestPriority = 4;
    for (unsigned bg = 1; bg < 4u; ++bg) {
        const unsigned p = bgPriority[bg] & 3u;
        if (p < objPriority) {
            if (opaque(context, bg)) return false;
            continue;
        }
        if (p >= bestPriority) continue;
        if (opaque(context, bg)) {
            best = (int)bg;
            bestPriority = p;
        }
    }
    if (best < 0) return false;
    const unsigned plane = zoneCount != 0 ? ZoneDepth3DS_PlaneAt(zones, zoneCount, (unsigned)best, x, y) : (unsigned)best;
    *layer = plane;
    *priority = plane == (unsigned)best ? bestPriority : bgPriority[plane] & 3u;
    return true;
}

#endif
