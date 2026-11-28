#pragma once
/* Rooms whose building graphics the game swaps by camera position (the Minish village,
 * src/manager/minishVillageTileSetManager.c; Hyrule Town, hyruleTownTileSetManager.c): one set
 * of tiles (and palettes) fits in VRAM at a time, chosen by the zone in view. The GBA's screen
 * never shows two zones at once; a 400x240 view does, and the zone not loaded came out in the
 * other's tiles (issue #26). The port keeps every set, copied from the ROM, and a map of which
 * zone each 8x8 cell of the room belongs to, so a wide view can draw each cell from its own set:
 * the shadow tilemaps carry a variant per cell (Port_ZoneTiles_Variant), the renderer takes the
 * tile from Port_ZoneTiles_Source and the palette from the variant's. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PORT_ZONE_SLOTS = 3, PORT_ZONE_VARIANTS = 16 };

/* What a cell's variant changes: per slot, the set's copy of that slot's character data (NULL:
 * what VRAM holds), and BG palettes (paletteMask bit n: palette n from palettes[n * 16]). */
typedef struct PortZoneVariant {
    const uint8_t* slotData[PORT_ZONE_SLOTS];
    uint16_t paletteMask;
    const uint16_t* palettes;
} PortZoneVariant;

typedef struct PortZoneTiles {
    /* Character data ranges of BG VRAM each slot swaps: [start[r], start[r] + length) for r = 0, 1,
     * read in that order from slotData. */
    uint32_t slotStart[PORT_ZONE_SLOTS][2];
    uint32_t slotLength[PORT_ZONE_SLOTS];
    unsigned slotCount;
    /* Variant 0 is "as VRAM holds", for cells outside every zone. */
    PortZoneVariant variants[PORT_ZONE_VARIANTS];
    unsigned variantCount;
    /* Bumped whenever the sets or palettes are rebuilt, so cached cells decoded from them go. */
    uint32_t generation;
} PortZoneTiles;

/* Once a tick, from Port_Widescreen_UpdateShadows: builds the sets for the room the game is in
 * (when it has one of those managers), frees them when it leaves. */
void Port_ZoneTiles_Update(void);
/* The current room's zones, or NULL when it has none. */
const PortZoneTiles* Port_ZoneTiles_Active(void);
/* The variant of the room's 8x8 cell (column, row from the room's origin): 0 outside every zone. */
uint8_t Port_ZoneTiles_Variant(int column, int row);
/* Beside each BG's shadow tilemap (port_linked_stubs.c, the same rows and stride): the variant of
 * each entry. Only while Port_ZoneTiles_ShadowLive(bg); kept here, out of the game's globals. */
uint8_t* Port_ZoneTiles_ShadowVariants(int bg);
void Port_ZoneTiles_SetShadowLive(int bg, bool live);
const uint8_t* Port_ZoneTiles_ShadowLive(int bg);

/* Where a tile of `bytes` at BG character offset `offset` comes from for a cell of `variant`:
 * the set's copy, or NULL for VRAM. */
static inline const uint8_t* Port_ZoneTiles_Source(const PortZoneTiles* zones, unsigned variant, uint32_t offset,
                                                  uint32_t bytes) {
    if (zones == NULL || variant == 0 || variant >= zones->variantCount) return NULL;
    for (unsigned slot = 0; slot < zones->slotCount; ++slot) {
        const uint8_t* data = zones->variants[variant].slotData[slot];
        if (data == 0) continue;
        uint32_t before = 0;
        for (unsigned r = 0; r < 2; ++r) {
            const uint32_t start = zones->slotStart[slot][r];
            const uint32_t length = zones->slotLength[slot] / 2u;
            if (offset >= start && offset + bytes <= start + length) return data + before + (offset - start);
            before += length;
        }
    }
    return NULL;
}
