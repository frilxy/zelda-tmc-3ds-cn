#include "port_zone_tiles.h"

#include "area.h"
#include "entity.h"
#include "global.h"
#include "main.h"
#include "manager.h"
#include "room.h"
#include "port_gba_mem.h"
#include "cpu/mode1.h"

#include <stdlib.h>
#include <string.h>

/* src/manager/minishVillageTileSetManager.c, hyruleTownTileSetManager.c */
extern const u16* Port_MinishVillageZoneRegions(void);
extern const u32* Port_MinishVillageZoneGfx(u32 set);
extern u32 Port_MinishVillageZonePaletteGroup(u32 set);
extern const u16* Port_HyruleTownZoneRegions(u32 slot);
extern void Port_HyruleTownZoneGfx(u32 group, const u8** src1, u32* dest1, const u8** src2, u32* dest2);
/* src/common.c */
extern u32 Port_ResolveBgPaletteGroup(u32 group, u16* out);
#ifdef PC_PORT
extern const u8* gGlobalGfxAndPalettes;
#else
extern const u8 gGlobalGfxAndPalettes[];
#endif

enum { KIND_NONE, KIND_MINISH_VILLAGE, KIND_HYRULE_TOWN };
enum { MAP_TILES = 128, MAX_SETS = 6, VRAM_BASE = 0x06000000 };

static PortZoneTiles sZones;
static int sKind;
static u32 sRoomKey = 0xffffffffu;
static uint8_t* sSetData[MAX_SETS];
static uint16_t sSetPalettes[MAX_SETS][256];
static uint16_t sSetPaletteMask[MAX_SETS];
static uint8_t sCellVariant[MAP_TILES * MAP_TILES];

static void Release(void) {
    for (unsigned set = 0; set < MAX_SETS; ++set) {
        free(sSetData[set]);
        sSetData[set] = NULL;
    }
    memset(sCellVariant, 0, sizeof(sCellVariant));
    sZones.slotCount = 0;
    sZones.variantCount = 0;
    sKind = KIND_NONE;
}

/* One chunk of a set into its slot's copy: VRAM address -> where the slot keeps it. */
static bool CopyChunk(unsigned set, unsigned slot, const u8* src, u32 dest, u32 size) {
    const u32 offset = dest - VRAM_BASE;
    const u32 half = sZones.slotLength[slot] / 2u;
    for (unsigned r = 0; r < 2; ++r) {
        const u32 start = sZones.slotStart[slot][r];
        if (offset >= start && offset + size <= start + half) {
            const void* from = port_resolve_copy_src(src, size);
            if (from == NULL) return false;
            memcpy(sSetData[set] + r * half + (offset - start), from, size);
            return true;
        }
    }
    return false;
}

/* The group a zone table (CheckRegionsOnScreen's format: group, x, y, width, height; 0xff ends)
 * gives the room point (x, y), the first that holds it; 0xff for none. */
static unsigned RegionGroup(const u16* regions, u32 x, u32 y) {
    for (; regions != NULL && *regions != 0xff; regions += 5) {
        if (x >= regions[1] && x < (u32)regions[1] + regions[3] && y >= regions[2] && y < (u32)regions[2] + regions[4])
            return regions[0];
    }
    return 0xff;
}

/* The group of the region nearest to the room point (x, y) within `radius` pixels (Chebyshev distance
 * to the region's rectangle); 0xff for none. The village's tables leave gaps: the game only swaps
 * graphics for the regions on screen, and in its 240x160 view a gap cell next to a zone is never seen
 * before that zone's set is loaded. A wider view shows it, so it takes the neighbour's set. */
static unsigned NearestRegionGroup(const u16* regions, u32 x, u32 y, u32 radius) {
    unsigned best = 0xff;
    u32 bestDistance = radius + 1;
    for (; regions != NULL && *regions != 0xff; regions += 5) {
        const u32 right = (u32)regions[1] + regions[3] - 1u, bottom = (u32)regions[2] + regions[4] - 1u;
        const u32 dx = x < regions[1] ? regions[1] - x : x > right ? x - right : 0u;
        const u32 dy = y < regions[2] ? regions[2] - y : y > bottom ? y - bottom : 0u;
        const u32 distance = dx > dy ? dx : dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = regions[0];
        }
    }
    return best;
}

/* The variant of a cell whose slots take these sets (0xff: VRAM's), added when new. */
static uint8_t VariantFor(const unsigned* setOfSlot) {
    for (unsigned v = 1; v < sZones.variantCount; ++v) {
        bool same = true;
        for (unsigned slot = 0; slot < sZones.slotCount && same; ++slot)
            same = sZones.variants[v].slotData[slot] == (setOfSlot[slot] < MAX_SETS ? sSetData[setOfSlot[slot]] : NULL);
        if (same) return (uint8_t)v;
    }
    if (sZones.variantCount >= PORT_ZONE_VARIANTS) return 0;
    PortZoneVariant* variant = &sZones.variants[sZones.variantCount];
    memset(variant, 0, sizeof(*variant));
    for (unsigned slot = 0; slot < sZones.slotCount; ++slot) {
        const unsigned set = setOfSlot[slot];
        if (set >= MAX_SETS) continue;
        variant->slotData[slot] = sSetData[set];
        if (sSetPaletteMask[set] != 0) {
            variant->paletteMask = sSetPaletteMask[set];
            variant->palettes = sSetPalettes[set];
        }
    }
    return (uint8_t)sZones.variantCount++;
}

static void MapCells(const u16* const* slotRegions, u32 gapRadius) {
    const int columns = gRoomControls.width / 8 < MAP_TILES ? gRoomControls.width / 8 : MAP_TILES;
    const int rows = gRoomControls.height / 8 < MAP_TILES ? gRoomControls.height / 8 : MAP_TILES;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            unsigned sets[PORT_ZONE_SLOTS];
            bool any = false;
            for (unsigned slot = 0; slot < sZones.slotCount; ++slot) {
                sets[slot] = RegionGroup(slotRegions[slot], (u32)column * 8u + 4u, (u32)row * 8u + 4u);
                if (sets[slot] == 0xff && gapRadius != 0)
                    sets[slot] = NearestRegionGroup(slotRegions[slot], (u32)column * 8u + 4u, (u32)row * 8u + 4u,
                                                    gapRadius);
                any |= sets[slot] != 0xff;
            }
            sCellVariant[row * MAP_TILES + column] = any ? VariantFor(sets) : 0;
        }
    }
}

static bool BuildMinishVillage(void) {
    enum { SETS = 5 };
    sZones.slotCount = 1;
    sZones.slotStart[0][0] = 0x0000;
    sZones.slotStart[0][1] = 0x8000;
    sZones.slotLength[0] = 0x8000;
    for (unsigned set = 0; set < SETS; ++set) {
        sSetData[set] = malloc(sZones.slotLength[0]);
        if (sSetData[set] == NULL) return false;
        const u32* gfx = Port_MinishVillageZoneGfx(set);
        for (unsigned chunk = 0; chunk < 8; ++chunk)
            if (!CopyChunk(set, 0, &gGlobalGfxAndPalettes[gfx[chunk * 2]], gfx[chunk * 2 + 1], 0x1000)) return false;
        sSetPaletteMask[set] = (uint16_t)Port_ResolveBgPaletteGroup(Port_MinishVillageZonePaletteGroup(set),
                                                                     sSetPalettes[set]);
    }
    const u16* regions[PORT_ZONE_SLOTS] = { Port_MinishVillageZoneRegions() };
    MapCells(regions, 128);
    return true;
}

static bool BuildHyruleTown(void) {
    enum { GROUPS = 6 };
    sZones.slotCount = 3;
    for (unsigned slot = 0; slot < 3; ++slot) {
        sZones.slotStart[slot][0] = 0x1000u * slot;
        sZones.slotStart[slot][1] = 0x8000u + 0x1000u * slot;
        sZones.slotLength[slot] = 0x2000;
    }
    for (unsigned group = 0; group < GROUPS; ++group) {
        const u8 *src1, *src2;
        u32 dest1, dest2;
        Port_HyruleTownZoneGfx(group, &src1, &dest1, &src2, &dest2);
        sSetData[group] = malloc(0x2000);
        if (sSetData[group] == NULL) return false;
        if (!CopyChunk(group, group / 2u, src1, dest1, 0x1000) || !CopyChunk(group, group / 2u, src2, dest2, 0x1000))
            return false;
    }
    const u16* regions[PORT_ZONE_SLOTS] = { Port_HyruleTownZoneRegions(0), Port_HyruleTownZoneRegions(1),
                                            Port_HyruleTownZoneRegions(2) };
    /* The town's tables leave gaps too (slot 1: 256 px between its two columns, slot 0: 128 px between its two rows):
     * a wide view shows a gap cell before the game has loaded its neighbour's set, as in the Minish village. 256 px
     * is the widest gap, so every gap cell takes the nearest region's set. */
    MapCells(regions, 256);
    return true;
}

static int RoomKind(void) {
    if (gMain.task != TASK_GAME) return KIND_NONE;
    if (DeepFindEntityByID(MANAGER, MINISH_VILLAGE_MANAGER) != NULL) return KIND_MINISH_VILLAGE;
    if (DeepFindEntityByID(MANAGER, HYRULE_TOWN_TILESET_MANAGER) != NULL) return KIND_HYRULE_TOWN;
    return KIND_NONE;
}

void Port_ZoneTiles_Update(void) {
    const int kind = RoomKind();
    /* Hyrule Town's tables differ for the festival (the same rooms in AREA_FESTIVAL_TOWN). */
    const u32 key = (u32)kind << 16 | (u32)gRoomControls.area << 8 | gRoomControls.room;
    if (key == sRoomKey) return;
    sRoomKey = key;
    Release();
    if (kind == KIND_NONE) return;
    sZones.variantCount = 1;
    memset(&sZones.variants[0], 0, sizeof(sZones.variants[0]));
    const bool built = kind == KIND_MINISH_VILLAGE ? BuildMinishVillage() : BuildHyruleTown();
    if (!built) {
        Release();
        return;
    }
    sKind = kind;
    ++sZones.generation;
}

const PortZoneTiles* Port_ZoneTiles_Active(void) {
    return sKind != KIND_NONE ? &sZones : NULL;
}

uint8_t Port_ZoneTiles_Variant(int column, int row) {
    if (sKind == KIND_NONE || column < 0 || row < 0 || column >= MAP_TILES || row >= MAP_TILES) return 0;
    return sCellVariant[row * MAP_TILES + column];
}

static uint8_t sShadowVariants[MODE1_GBA_BG_COUNT][MODE1_WS_SHADOW_ROWS * MODE1_WS_SHADOW_COLS];
static bool sShadowLive[MODE1_GBA_BG_COUNT];

uint8_t* Port_ZoneTiles_ShadowVariants(int bg) {
    return bg >= 0 && bg < MODE1_GBA_BG_COUNT ? sShadowVariants[bg] : NULL;
}

void Port_ZoneTiles_SetShadowLive(int bg, bool live) {
    if (bg >= 0 && bg < MODE1_GBA_BG_COUNT) sShadowLive[bg] = live;
}

const uint8_t* Port_ZoneTiles_ShadowLive(int bg) {
    return bg >= 0 && bg < MODE1_GBA_BG_COUNT && sShadowLive[bg] && sKind != KIND_NONE ? sShadowVariants[bg] : NULL;
}
