/* Zone depth fixes (docs/stereo-3d.md, "Zone depth fixes"; the format is in port_zone_depth.inc): the table, the fixes
 * of the current room (looked up once per room change) and where they lie in the frame being drawn. */
#include "zone_depth_3ds.h"

#include "global.h"
#include "room.h"
#include "port_gba_mem.h"

#if defined(__has_include)
#if __has_include("port_zone_depth.inc")
#define PORT_ZONE_DEPTH(area, room, bg, x, y, w, h, toBg) \
    { (uint8_t)(area), (uint8_t)(room), (uint8_t)(bg), (uint8_t)(toBg), (int16_t)(x), (int16_t)(y), (int16_t)(w), (int16_t)(h) },
static const ZoneDepth3DSFix sFixes[] = {
#include "port_zone_depth.inc"
};
#undef PORT_ZONE_DEPTH
#define ZONE_FIX_COUNT ((unsigned)(sizeof(sFixes) / sizeof(sFixes[0])))
#endif
#endif
#ifndef ZONE_FIX_COUNT
static const ZoneDepth3DSFix sFixes[1];
#define ZONE_FIX_COUNT 0u
#endif

static const unsigned kFixCount = ZONE_FIX_COUNT;

/* The current room's fixes (indices into sFixes), for the room they were looked up for. */
static unsigned sRoomArea = 0xffffu, sRoomId = 0xffffu;
static uint16_t sRoomFixes[ZONE_DEPTH_3DS_MAX_PER_ROOM];
static unsigned sRoomFixCount;
/* This frame's zones. */
static ZoneDepth3DSZone sZones[ZONE_DEPTH_3DS_MAX_PER_ROOM];
static unsigned sZoneCount;

static void LookUpRoom(unsigned area, unsigned room) {
    sRoomArea = area;
    sRoomId = room;
    sRoomFixCount = 0;
    for (unsigned i = 0; i < kFixCount && sRoomFixCount < ZONE_DEPTH_3DS_MAX_PER_ROOM; ++i)
        if (sFixes[i].area == area && sFixes[i].room == room && ZoneDepth3DS_FixValid(&sFixes[i]))
            sRoomFixes[sRoomFixCount++] = (uint16_t)i;
}

static unsigned ReadIo16(unsigned offset) {
    return (unsigned)gIoMem[offset] | ((unsigned)gIoMem[offset + 1u] << 8);
}

void ZoneDepth3DS_BeginFrame(bool world) {
    sZoneCount = 0;
    if (!world || kFixCount == 0u) return;
    const unsigned area = gRoomControls.area, room = gRoomControls.room;
    if (area != sRoomArea || room != sRoomId) LookUpRoom(area, room);
    for (unsigned n = 0; n < sRoomFixCount; ++n) {
        const ZoneDepth3DSFix* fix = &sFixes[sRoomFixes[n]];
        /* The layer's own offsets in the registers the frame is drawn from (BGnHOFS, BGnVOFS): the rectangle follows its
         * pixels, screen shake and a camera a tick ahead included. */
        const int frameX = ZoneDepth3DS_FrameScroll(gRoomControls.scroll_x, gRoomControls.origin_x,
                                                    ReadIo16(0x10u + 4u * fix->bg), 0);
        const int frameY = ZoneDepth3DS_FrameScroll(gRoomControls.scroll_y, gRoomControls.origin_y,
                                                    ReadIo16(0x12u + 4u * fix->bg), ZONE_DEPTH_3DS_VOFS_BIAS);
        ZoneDepth3DSZone* zone = &sZones[sZoneCount];
        if (!ZoneDepth3DS_ScreenRect(fix, (int)gRoomControls.origin_x - frameX, (int)gRoomControls.origin_y - frameY,
                                     &zone->rect))
            continue;
        zone->bg = fix->bg;
        zone->toBg = fix->toBg;
        ++sZoneCount;
    }
}

unsigned ZoneDepth3DS_Zones(const ZoneDepth3DSZone** zones) {
    *zones = sZones;
    return sZoneCount;
}
