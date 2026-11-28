#pragma once

#include "cpu/mode1.h"
#include "zone_depth_3ds.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The GPU renderer draws every frame on both models (owner's rule, 2026-10-11:
 * everything goes through the GPU). Upstream kept it to the Old 3DS. What it
 * cannot draw yet falls back to the software PPU for that frame: the New 3DS
 * Full View layouts (400x240 outdoors, 200x120 at 2x indoors), PLAN P3.4. */
static inline bool PpuGpu3DS_ShouldUse(bool initialized, bool disabled, bool fullView) {
    (void)fullView; /* PLAN P3.4: Full View too */
    return initialized && !disabled;
}

enum {
    /* Each background's map-space geometry owns a fixed slice of the command
     * buffer, so a layer that has not changed keeps last frame's vertices
     * while its neighbours are rebuilt. */
    PPU_GPU3DS_MAP_MAX_QUADS = 1024,
    PPU_GPU3DS_MAP_SLICE_VERTICES = PPU_GPU3DS_MAP_MAX_QUADS * 4,
    PPU_GPU3DS_MAP_SLICE_INDICES = PPU_GPU3DS_MAP_MAX_QUADS * 6,
    PPU_GPU3DS_MAP_TILE_SNAPSHOT = 24576,
    PPU_GPU3DS_ATLAS_SIDE = 512,
    PPU_GPU3DS_TILE_SIDE = 8,
    PPU_GPU3DS_SLOT_COUNT = 4096,
    PPU_GPU3DS_ATLAS_PIXELS = PPU_GPU3DS_ATLAS_SIDE * PPU_GPU3DS_ATLAS_SIDE,
    /* Power of two, sized so the chains stay at roughly one entry per key. */
    PPU_GPU3DS_CACHE_BUCKETS = 8192,
    PPU_GPU3DS_CACHE_NIL = 0xffff
};

typedef enum PpuGpu3DSLayer {
    PPU_GPU3DS_BG0,
    PPU_GPU3DS_BG1,
    PPU_GPU3DS_BG2,
    PPU_GPU3DS_BG3,
    PPU_GPU3DS_OBJ,
    PPU_GPU3DS_BACKDROP
} PpuGpu3DSLayer;

typedef enum PpuGpu3DSPaletteDomain {
    PPU_GPU3DS_PALETTE_BG,
    PPU_GPU3DS_PALETTE_OBJ
} PpuGpu3DSPaletteDomain;

typedef struct PpuGpu3DSTileKey {
    uint32_t vramOffset;
    uint8_t paletteBank;
    bool bpp8;
    PpuGpu3DSPaletteDomain domain;
} PpuGpu3DSTileKey;

typedef struct PpuGpu3DSCacheEntry {
    PpuGpu3DSTileKey key;
    /* The 64 source bytes used to live here. Every hash-bucket walk compares a
     * key and follows hashNext, so inlining the copy dragged ~80 bytes through
     * a 32 KiB L1 with no L2 behind it to read twelve. They now live in a
     * parallel array touched only when a key actually matches, which puts four
     * probe records in a 32-byte cache line instead of half of one. */
    uint32_t paletteGeneration;
    uint32_t lastUseFrame;
    /* Intrusive links: hashNext chains this slot inside its bucket, and the
     * lru pair orders every slot from most to least recently used so both
     * lookup and eviction stay O(1) instead of scanning all 4096 slots. */
    uint16_t hashNext;
    uint16_t lruPrev, lruNext;
    bool valid;
    bool dirty;
    /* Held by retained map geometry: eviction must not hand this slot to a
     * different tile while quads are still pointing at it. */
    bool pinned;
    /* Every texel is transparent, so a quad sampling it can be left out
     * entirely instead of being drawn and discarded by the alpha test. */
    bool transparent;
} PpuGpu3DSCacheEntry;

/* A background's map-space geometry, kept across frames while the tilemap,
 * the tiles it names and the palette all stay put. */
typedef struct PpuGpu3DSRetainedMap {
    /* Tilemap-and-geometry digest, and the palette generation, kept apart so a
     * rebuild can say which one moved. They were one value; any palette bank
     * changing then invalidated every layer indiscriminately, and that could
     * not be told apart from a real tilemap edit. */
    uint32_t signature;
    uint32_t paletteSignature;
    uint32_t firstIndex;
    uint16_t rowLo, colLo, rows, cols;
    uint16_t bgcnt;
    uint16_t slotCount;
    uint16_t slots[PPU_GPU3DS_MAP_MAX_QUADS];
    /* Which 4bpp palette banks these quads actually sample, one bit per bank.
     * PpuGpu3DS_CacheTile makes a tile depend on exactly one bank generation
     * (or, at 8bpp, on the 256-colour generation alone), so a layer depends on
     * the union over its tiles and nothing more. 0 means not yet known, which
     * falls back to depending on everything. */
    uint16_t bankMask;
    /* Byte range of the character data these quads sample. A 64-bit digest of
     * it lives in the cache: reading VRAM once beats matching it against a
     * full copy, and far beats checking each tile against its scattered
     * cache entry. */
    uint32_t tileFirst, tileLast;
    bool valid;
} PpuGpu3DSRetainedMap;

/* A background's view ring: the tiles the view shows, decoded in place into a
 * texture of their own, so the layer is drawn as one quad per band instead of
 * one per 8x8 tile. The texture holds the BG's 64 x 32 tile map transposed
 * (map column = texture row) so a column of cells, the thing a horizontal
 * scroll replaces, is contiguous in memory; the GPU's REPEAT wrap makes it a
 * ring in both directions. Cell (column A, row R) lives at
 * (A & 63) * 32 + (R & 31), 64 pixels each, Morton ordered. */
enum {
    PPU_GPU3DS_RING_COLS = 64,
    PPU_GPU3DS_RING_ROWS = 32,
    PPU_GPU3DS_RING_CELLS = PPU_GPU3DS_RING_COLS * PPU_GPU3DS_RING_ROWS,
    PPU_GPU3DS_RING_TEX_WIDTH = PPU_GPU3DS_RING_ROWS * 8,
    PPU_GPU3DS_RING_TEX_HEIGHT = PPU_GPU3DS_RING_COLS * 8,
    PPU_GPU3DS_RING_CHUNK = 256,
    PPU_GPU3DS_RING_CHUNKS = 256,
};

typedef struct PpuGpu3DSRing {
    /* Texture memory, owned by the platform layer; NULL leaves the layer on
     * the per-tile path. */
    uint16_t* pixels;
    /* Per cell: the map entry, the low bits of its palette generation and a
     * valid bit (0 = empty), compared as one word. */
    uint32_t key[PPU_GPU3DS_RING_CELLS];
    /* Per cell: the zone variant its tile was decoded from (wsShadowZone), 0 for VRAM. */
    uint8_t variant[PPU_GPU3DS_RING_CELLS];
    /* What each 256-byte chunk of character data held when the cells that
     * sample it were last decoded. */
    uint32_t chunkDigest[PPU_GPU3DS_RING_CHUNKS];
    uint32_t charBase;
    /* Chunks below this (from charBase's) have been sampled by a cell, so
     * only they are watched for change. */
    unsigned chunkEnd;
    bool bpp8, contextValid;
    /* Cells written since the platform last flushed them: the span, and each
     * cell, so a platform building into a copy of the texture can move just
     * those (a vertical scroll's row spans the whole texture but is 64 cells). */
    uint32_t dirtyFirst, dirtyLast;
    uint32_t dirtyCells[PPU_GPU3DS_RING_CELLS / 32];
    bool dirty;
} PpuGpu3DSRing;

enum { PPU_GPU3DS_MEMO_SIZE = 512, PPU_GPU3DS_RING_MEMO_SIZE = 1024 };

typedef struct PpuGpu3DSCache {
    PpuGpu3DSCacheEntry entries[PPU_GPU3DS_SLOT_COUNT];
    /* Tiles already looked up in this frame: VRAM and the palettes cannot
     * change within a frame and a slot used in it is never evicted or
     * refreshed in it, so a repeat lookup can skip the bucket walk, the
     * 32-byte compare and the LRU move. Valid while memoEpoch matches. */
    struct {
        PpuGpu3DSTileKey key;
        uint16_t slot;
        uint32_t epoch;
    } memo[PPU_GPU3DS_MEMO_SIZE];
    uint32_t memoEpoch;
    uint16_t buckets[PPU_GPU3DS_CACHE_BUCKETS];
    /* Slots decoded since the last upload, so the backend flushes only what
     * changed rather than walking every slot. */
    uint16_t dirtySlots[PPU_GPU3DS_SLOT_COUNT];
    uint16_t dirtyCount;
    uint16_t lruHead, lruTail;
    uint16_t bgPalette[MODE1_PALETTE_COLORS];
    uint16_t objPalette[MODE1_PALETTE_COLORS];
    uint32_t bgBankGeneration[16];
    uint32_t objBankGeneration[16];
    /* Packed 4bpp banks. Tiles decoded back to back almost always share a
     * bank, so the sixteen packs that begin a decode are done once per bank
     * per change instead of once per tile. Indexed [domain][bank]. */
    /* Ranges verified against VRAM already this frame. Retained layers share
     * character data heavily -- one layer's range is routinely a subset of
     * another's -- so the same bytes were compared two and three times over. */
    uint32_t verifiedFirst[MODE1_GBA_BG_COUNT];
    uint32_t verifiedLast[MODE1_GBA_BG_COUNT];
    uint32_t verifiedFrame;
    unsigned verifiedCount;
    /* Parallel to entries[]: see PpuGpu3DSCacheEntry. */
    uint8_t sources[PPU_GPU3DS_SLOT_COUNT][64];
    uint16_t bankLut[2][16][16];
    uint32_t bankLutGeneration[2][16];
    bool bankLutValid[2][16];
    uint32_t bg256Generation;
    uint32_t obj256Generation;
    uint32_t frame;
    uint64_t hits, decodes;
    /* Raised when every slot is already claimed by the frame being built. */
    bool exhausted;
    PpuGpu3DSRetainedMap retained[MODE1_GBA_BG_COUNT];
    /* Two per background: [bg * 2] holds the entries of its VRAM map, [bg * 2 + 1]
     * those of the shadow tilemap (columns past x=240, or all of them in Full
     * View). */
    PpuGpu3DSRing ring[MODE1_GBA_BG_COUNT * 2];
    /* Character-data chunk digests of the frame being built, shared by the
     * rings (VRAM does not change while a frame is built). */
    uint32_t chunkDigest[PPU_GPU3DS_RING_CHUNKS];
    uint32_t chunkDigestStamp[PPU_GPU3DS_RING_CHUNKS];
    uint32_t ringStamp;
    /* Cells decoded this frame, by what they were decoded from: a screen of
     * grass names the same few tiles in the same bank over and over, and a
     * fade (every bank changes every frame) re-decodes them all. A hit copies
     * the finished cell instead of decoding it again. */
    struct PpuGpu3DSRingMemo {
        uint32_t stamp, key, ctx;
    } ringMemo[PPU_GPU3DS_RING_MEMO_SIZE];
    uint32_t retainedVertices, retainedIndices;
    /* A 64-bit digest of those ranges rather than a copy of them. The compare
     * then reads VRAM only -- half the traffic of matching two buffers -- and
     * the 96 KiB of snapshots this replaced is 96 KiB no longer competing for
     * a 32 KiB L1 with no L2 behind it. A layer whose tiles outgrow
     * PPU_GPU3DS_MAP_TILE_SNAPSHOT still keeps rebuilding every frame, so the
     * retention decision is unchanged; only how it is checked. */
    uint64_t retainedTileDigest[MODE1_GBA_BG_COUNT];
    bool retainedValid;
} PpuGpu3DSCache;

typedef struct PpuGpu3DSFrameView {
    unsigned width, height;
    bool affine, ioUniform;
    uint16_t frameDispcnt;
    VirtuaPPUMode1GbaMemory memory;
    const uint8_t* ioPerLine;
    const uint16_t* dispcntPerLine;
    const int32_t* affineRefX;
    const int32_t* affineRefY;
    const uint16_t* wsShadow;
    /* Beside wsShadow, each entry's zone (port/port_zone_tiles.h): a cell of a room that swaps
     * building graphics by zone is drawn from its own zone's set and palettes. NULL: none. */
    const uint8_t* wsShadowZone;
    const struct PortZoneTiles* zones;
    int wsShadowBaseTile[4];
    int wsCols, wsShadowHalfwords;
    int wsHudRightAnchor, wsHudRightNativeX;
    int wsMsgShift, wsMsgX0, wsMsgX1, wsMsgY0, wsMsgY1;
    bool bg3Repeat;
    uint8_t bgRepeatMask; /* virtuappu_mode1_bg_repeat_mask */
    /* BGs (bit n = BGn; one at a time) with the picture texture drawn over their
     * tiles across the whole frame: the title's forest as BG1
     * (virtuappu_mode1_bg1_picture, PortPpuGpu3DS_SetBg1Picture), the file
     * select's cave as BG3 (PortPpuGpu3DS_SetIndexedPicture). */
    uint8_t pictureMask;
    /* Side strips of a room narrower than the view (the Crenel paths' shaft, PortPpuGpu3DS_SetScrollPicture), one
     * picture per BG in scrollPictureMask, drawn over that BG's tiles where opaque: the strip of
     * scrollPictureStripWidth columns left of the room and the one right of it, the room scrollPictureGap wide, the
     * left strip's top-left at (scrollPictureX, scrollPictureY[bg]) on the frame, so they move with the camera and
     * each BG's scroll. Picture rows past scrollPictureHeight[bg] repeat from scrollPictureLoopStart[bg] over
     * scrollPictureLoopLength[bg] rows (0: nothing past the end). */
    uint8_t scrollPictureMask;
    int16_t scrollPictureX;
    uint16_t scrollPictureStripWidth, scrollPictureGap;
    int16_t scrollPictureY[4];
    uint16_t scrollPictureHeight[4], scrollPictureLoopStart[4], scrollPictureLoopLength[4];
    /* Bands above and below a room shorter than the view (Crenel's horizontal Minish path, PortPpuGpu3DS_SetScrollPicture
     * with `repeat`), one picture per BG in rowPictureMask, drawn over that BG's tiles where opaque: rowPictureWidth[bg]
     * columns (a power of two, repeated across the frame), the view column of its column 0 at rowPictureX[bg]; two
     * bands, the first rowPictureBandRows[bg][0] picture rows from the top, the second the rows after them, each band's
     * top row at rowPictureBandY[bg][band] on the view. A BG has either side strips or bands, never both. */
    uint8_t rowPictureMask;
    /* BGs (bit n = BGn) whose alpha blend multiplies (PpuGpu3DSBatch::multiply): the shadows of Crenel's horizontal
     * Minish paths (BG3), which the GBA's blend lightened over the walls darker than their colour. */
    uint8_t bgMultiplyMask;
    int16_t rowPictureX[4], rowPictureBandY[4][2];
    uint16_t rowPictureWidth[4], rowPictureBandRows[4][2];
    /* BGs (bit n = BGn) drawn only in the GBA's 240x160 screen centred in a
     * bigger view, at their own scroll (the file select's windows and text,
     * whose 256-wide maps would repeat across it). */
    uint8_t bgNativeMask;
    /* Full View (Wide + Pixel Perfect): the shadow tilemaps cover the whole
     * view from x=0, not only the columns past 240 (virtuappu_mode1_ws_full_view). */
    bool wsFullView;
    /* OAM Y at or past objYWrap, or flagged in objYNegative, is above the top
     * (y - 256): the frame's logical height, as mode1.c's
     * mode1_obj_y_wrap_threshold, and the port's per-entry metadata. */
    int objYWrap;
    const uint8_t* objYNegative;
    bool objClipEnable;
    const uint8_t* objClipMark;
    int objClipY;
    /* Each OAM entry's stereo 3D depth, quarter pixels (virtuappu_mode1_obj_depth): what Link holds up or threw. */
    const uint8_t* objDepth;
    /* The same at each entry's top edge (virtuappu_mode1_obj_depth_top): the depth of a tile row goes from objDepth
     * at the entry's bottom to this at its top; NULL or equal: one depth for the entry. With bit 7 set (and bit 6 for
     * the direction) the entry leans across its columns instead (sprite_lean_3ds.h). */
    const uint8_t* objDepthTop;
    /* Stereo 3D "support": a sprite resting on a background layer that stands out (the objects on a roof) takes that
     * layer's depth for its tiles over it. bgDepthQ[bg][priority] is the layer's depth in quarter pixels at the full
     * slider (0 behind the screen plane); objSupport is false when no such depth applies (3D off, or not the world). */
    bool objSupport;
    /* Which entity wrote each OAM entry (virtuappu_mode1_obj_group; 0: none): an entity's entries share one support. */
    const uint8_t* objGroup;
    uint8_t bgDepthQ[4][4];
    /* The zone depth fixes on this frame (zone_depth_3ds.h): a sprite resting on a zone's pixels takes the zone's plane
     * (toBg's depth) instead of its layer's. NULL / 0: none, nothing is looked up. */
    const ZoneDepth3DSZone* depthZones;
    unsigned depthZoneCount;
    /* A uniform screen fade the renderer applies itself (docs/gpu-fade.md):
     * `memory`'s palettes are then the unfaded ones and the finished picture is
     * blended toward grey `fadeLevel` by fadeWeight/255. 0 weight: none. */
    uint8_t fadeWeight, fadeLevel;
    /* BG3 is driven by HBlank DMA (ripples, dissolves) in a view bigger than the
     * GBA's 240x160: the software PPU then draws it only in that native canvas,
     * centred in the view, sampling source line = line - top
     * (mode1_bg3_native_bounds_active). */
    bool bg3NativeBounds;
    /* BG3HOFS before any HBlank DMA step (virtuappu_mode1_bg3_hofs_base), -1 unknown. */
    int bg3HofsBase;
    /* The iris fade in a wide view (virtuappu_mode1_ws_win0_lines: {left,
     * right} per line), or NULL. The frame is then built without WIN0 and the
     * renderer paints the backdrop colour outside each line's span, which is
     * what the iris's window settings (inside everything, outside nothing)
     * show; splitting the frame into a band per line cost the Old 3DS 40 ms. */
    const uint16_t* irisLines;
    /* A circle the game draws with WIN0 under its own window settings (a dark
     * room's light around Link) in a wide view: {left, right} per line, which
     * replace WIN0H and WIN0V (the game's 8-bit table stops at x=255, y=159);
     * WININ/WINOUT apply as usual. NULL: the registers. */
    const uint16_t* win0Lines;
} PpuGpu3DSFrameView;

typedef struct PpuGpu3DSInterval {
    uint16_t left, right;
} PpuGpu3DSInterval;

typedef struct PpuGpu3DSBand {
    uint16_t firstLine, lineCount;
    uint8_t ioRow;
} PpuGpu3DSBand;

typedef struct PpuGpu3DSVertex {
    /* w was always 1.0. PICA supplies 1.0 for a missing fourth component, and
     * uOffset's w is 0, so dropping it changes nothing on screen and takes the
     * vertex from 24 to 20 bytes -- a sixth off every streaming store the
     * builder makes and off every byte flushed for geometry, which is worth
     * more than it sounds on a core with 32 KiB of L1 and no L2. */
    float x, y, z;
    /* UV as a fixed-point multiple of 1/PPU_GPU3DS_UV_SCALE in atlas-normalized
     * units. This is lossless rather than approximate: ATLAS_SIDE is 512, so a
     * texel centre is (2t+1)/1024, and scaling by 4096 makes that the exact
     * integer 8t+4. 1/4096 is 2^-12, so the shader's rescale is exact in float
     * as well, and packed geometry renders bit-for-bit like the float form.
     * Takes the vertex from 20 to 16 bytes -- four to a 32-byte cache line
     * instead of straddling, which is what actually matters with no L2. */
    int16_t u, v;
} PpuGpu3DSVertex;

enum { PPU_GPU3DS_UV_SCALE = 4096 };

static inline int16_t PpuGpu3DS_PackUV(float uv) {
    const float scaled = uv * (float)PPU_GPU3DS_UV_SCALE;
    return (int16_t)(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
}

static inline float PpuGpu3DS_UnpackUV(int16_t uv) {
    return (float)uv * (1.0f / (float)PPU_GPU3DS_UV_SCALE);
}


typedef enum PpuGpu3DSEffect {
    PPU_GPU3DS_EFFECT_NONE,
    PPU_GPU3DS_EFFECT_ALPHA,
    PPU_GPU3DS_EFFECT_BRIGHTEN,
    PPU_GPU3DS_EFFECT_DARKEN
} PpuGpu3DSEffect;

typedef struct PpuGpu3DSBatch {
    uint32_t firstIndex, indexCount;
    uint16_t firstLine, lineCount, scissorLeft, scissorRight;
    uint8_t layer, priority, windowControl, target2;
    uint8_t effect, eva, evb, evy, objectIndex;
    uint16_t color;
    bool objWindow, semiTransparent;
    /* A sprite's own stereo 3D depth, quarter pixels (objDepth; 0: its layer's). */
    uint8_t depth;
    /* Drawn with the layers in front even with no depth of its own (the pieces of one big sprite stay in one pass, so
     * what the game draws over what keeps its order); bit 7 of the OAM entry's depth (virtuappu_mode1_obj_depth). */
    bool front;
    /* A sprite that leans across its columns (SPRITE_LEAN_3DS_SIDE_*, sprite_lean_3ds.h): its shift goes to one eye
     * only (SpriteLean3DS_EyeShift), so its columns never part. 0 for everything else. */
    uint8_t side;
    /* 0 samples the tile atlas; 1 + n samples view ring n (bg * 2 + variant). */
    uint8_t texture;
    /* Input of append_layer_batches, not read by the renderer: nothing under this layer is not a blend target, so the
     * blend's complement (the layer drawn plain where the pixel under it is not a target) would draw nowhere. */
    bool noComplement;
    /* An alpha-blended batch whose blend multiplies instead (PpuGpu3DSFrameView::bgMultiplyMask): the pixel under it
     * times (its colour + 0.25), so it always darkens what it covers, in its own tint. */
    bool multiply;
    /* Added to every vertex position by the shader. Zero except for
     * background batches that share one map-space copy of the tiles. */
    float offsetX, offsetY;
} PpuGpu3DSBatch;

/* Why a frame could not be expressed as GPU commands, so the fallback rate
 * can be attributed instead of guessed. */
typedef enum PpuGpu3DSBuildReason {
    PPU_GPU3DS_BUILD_OK,
    PPU_GPU3DS_BUILD_ARGUMENTS,
    PPU_GPU3DS_BUILD_UNSUPPORTED,
    PPU_GPU3DS_BUILD_CAPACITY,
    PPU_GPU3DS_BUILD_ATLAS_FULL,
    PPU_GPU3DS_BUILD_GEOMETRY,
    PPU_GPU3DS_BUILD_REASON_COUNT
} PpuGpu3DSBuildReason;

typedef enum PpuGpu3DSMapReject {
    PPU_GPU3DS_MAP_REJECT_AFFINE,
    PPU_GPU3DS_MAP_REJECT_CONTROL,
    PPU_GPU3DS_MAP_REJECT_SCREEN_SPACE,
    PPU_GPU3DS_MAP_REJECT_DISABLED,
    PPU_GPU3DS_MAP_REJECT_TOO_LARGE,
    PPU_GPU3DS_MAP_REJECT_COVERAGE,
    PPU_GPU3DS_MAP_REJECT_COUNT
} PpuGpu3DSMapReject;

/* Why a retained layer had to re-emit its quads. Attribution matters because
 * the remedies are unrelated: a palette-driven rebuild is over-invalidation to
 * be narrowed, a window-driven one is scrolling and inherent, and a tile or
 * tilemap one is the game genuinely changing what is drawn. */
typedef enum PpuGpu3DSMapRebuild {
    PPU_GPU3DS_MAP_REBUILD_NEW,
    PPU_GPU3DS_MAP_REBUILD_TILEMAP,
    PPU_GPU3DS_MAP_REBUILD_PALETTE,
    PPU_GPU3DS_MAP_REBUILD_TILES,
    PPU_GPU3DS_MAP_REBUILD_WINDOW,
    PPU_GPU3DS_MAP_REBUILD_COUNT
} PpuGpu3DSMapRebuild;

typedef struct PpuGpu3DSCommandBuffer {
    PpuGpu3DSVertex* vertices;
    uint16_t* indices;
    PpuGpu3DSBatch* batches;
    size_t vertexCount, vertexCapacity;
    size_t indexCount, indexCapacity;
    size_t batchCount, batchCapacity;
    /* Set when a write was dropped for want of room, so the single build pass
     * can run to completion and be discarded as a whole. */
    bool overflow;
    uint8_t failReason;
    /* Which backgrounds shared one map-space copy of their tiles, and why the
     * others could not. */
    uint8_t mapLayerMask;
    /* Layers whose slice was rewritten this frame, and how much of each slice
     * is live, so only what changed is flushed to the GPU. */
    uint8_t mapDirtyMask;
    uint32_t mapSliceVertices[4];
    uint32_t dynamicFirstVertex, dynamicFirstIndex;
    uint32_t mapLargestQuads;
    uint32_t mapReject[PPU_GPU3DS_MAP_REJECT_COUNT];
    /* Layers kept by refreshing the atlas in place instead of re-emitting
     * identical quads. Against mapRebuild[PALETTE] this says how often the
     * shortcut applied. */
    uint32_t mapRefresh;
    uint32_t mapRebuild[PPU_GPU3DS_MAP_REBUILD_COUNT];
    uint16_t bandCount;
    /* A scaled affine BG2 drawn for the whole screen once a frame; bands with
     * the same matrix and origin reuse its indices under their own scissor. */
    struct {
        bool valid;
        uint16_t bgcnt;
        int32_t pa, pd, refX, refY0;
        uint32_t firstIndex, indexCount;
        uint8_t texture; /* 0 the atlas (a quad a tile), else 1 + the ring it was decoded into */
    } affineShare;
    /* Vertices this frame's affine bands spent on one-quad-a-texel geometry (see build_affine_bg). */
    size_t affineTexelVertices;
    /* A 512x512 wrapping affine BG2 (the Minish Shrine's barrel) is drawn from two rings, one a half
     * of the map's rows each (rings AFFINE_RING and AFFINE_RING + 1), a quad a band and half: the
     * half and screen lines of the segment being built, and which rings are already up to date
     * this frame (with the BG2 control they were decoded for). */
    uint8_t affineRingHalf, affineRingDone;
    uint16_t affineRingBgcnt[2];
    uint16_t affineSegFirst, affineSegEnd;
    /* The batches the last segment appended, so the next one can extend them (see build_affine_bg). */
    size_t affineRunStart, affineRunCount;
    /* What the frame asked for, which on overflow exceeds the capacities. */
    uint32_t requiredVertices, requiredBatches;
} PpuGpu3DSCommandBuffer;

/* Off makes every background walk the tilemap per band, for diffing. */
typedef enum PpuGpu3DSPhase {
    PPU_GPU3DS_PHASE_BANDS,
    PPU_GPU3DS_PHASE_MERGE,
    PPU_GPU3DS_PHASE_MAPS,
    /* Inside MAPS: the tilemap digest that decides whether a layer can keep
     * last frame's geometry. Walking the map is meant to be far cheaper than
     * re-emitting the quads, so this being the bulk of MAPS would mean the
     * reuse test costs more than the work it avoids. */
    PPU_GPU3DS_PHASE_MAPSIG,
    /* Inside MAPS: the tile-pixel snapshot compare that confirms a retained
     * layer's atlas contents are still current. This reads the layer's whole
     * char range out of VRAM and again out of the snapshot every frame, on the
     * path that is supposed to be the cheap one. */
    PPU_GPU3DS_PHASE_MAPRETAIN,
    PPU_GPU3DS_PHASE_SCENE,
    PPU_GPU3DS_PHASE_OBJWIN,
    PPU_GPU3DS_PHASE_REGIONS,
    PPU_GPU3DS_PHASE_BG,
    PPU_GPU3DS_PHASE_OBJ,
    /* Tile decode, wherever it is reached from -- it is nested inside the
     * phase that triggered it. Palette churn forces map rebuilds whose
     * geometry comes out byte-identical, so whether that waste is the
     * re-emitted quads or the re-decoded tiles decides which fix is worth
     * making. */
    PPU_GPU3DS_PHASE_DECODE,
    PPU_GPU3DS_PHASE_RINGDIGEST,
    PPU_GPU3DS_PHASE_RINGCELLS,
    PPU_GPU3DS_PHASE_COUNT
} PpuGpu3DSPhase;
#ifdef PPU_GPU3DS_PROFILE
extern double gPpuGpu3DSPhase[PPU_GPU3DS_PHASE_COUNT];
extern uint32_t gPpuGpu3DSLastBands, gPpuGpu3DSLastVertices, gPpuGpu3DSLastBatches;
#endif

extern uint32_t gPpuGpu3DSRingWhy[4];
extern volatile uint8_t gPpuGpu3DSBandMergeDisable;
extern uint32_t gPpuGpu3DSRingCells, gPpuGpu3DSRingStale, gPpuGpu3DSRingMemoHits;
extern volatile uint8_t gPpuGpu3DSRingDisable, gPpuGpu3DSRingMemoDisable;
extern uint32_t gPpuGpu3DSBankChanges[16];
void PpuGpu3DS_SetMapSpaceEnabled(bool enabled);
/* Gives view ring `index` (bg * 2 + variant) its storage in `pixels` (256 x 512 RGBA5551, 256 KiB); NULL takes it away. */
void PpuGpu3DS_RingSetStorage(PpuGpu3DSCache* cache, unsigned index, uint16_t* pixels);
/* The byte range of the ring's pixels written since the last call, for the
 * platform to flush; false when nothing was. `cells` (PPU_GPU3DS_RING_CELLS
 * bits, may be NULL) receives which cells of it were written. */
bool PpuGpu3DS_RingTakeDirty(PpuGpu3DSCache* cache, unsigned index, size_t* offset, size_t* bytes,
                             uint32_t* cells);
enum { PPU_GPU3DS_RING_COUNT = MODE1_GBA_BG_COUNT * 2 };
/* BG2's native ring: an affine BG2 has no text ring, so a scaled one is decoded there. */
enum { PPU_GPU3DS_AFFINE_RING = 2 * 2 };
/* PpuGpu3DSBatch.texture of a BG drawn from a picture (PpuGpu3DSFrameView::pictureMask): a
 * PPU_GPU3DS_PICTURE_TEX_WIDTH x _HEIGHT RGBA5551 texture, rows from v = 1 down. */
enum { PPU_GPU3DS_PICTURE_TEXTURE = 1 + PPU_GPU3DS_RING_COUNT, PPU_GPU3DS_PICTURE_TEX_WIDTH = 512,
       PPU_GPU3DS_PICTURE_TEX_HEIGHT = 256 };
/* PpuGpu3DSBatch.texture of BG n's side strips (PpuGpu3DSFrameView::scrollPictureMask): this + n, RGBA5551, 256
 * wide (the two strips side by side), as tall as PortPpuGpu3DS_ScrollPictureTexHeight, rows from v = 1 down. */
enum { PPU_GPU3DS_SCROLL_PICTURE_TEXTURE = PPU_GPU3DS_PICTURE_TEXTURE + 1, PPU_GPU3DS_SCROLL_PICTURE_TEX_WIDTH = 256 };
/* The texture height a side-strip picture of `height` rows takes (the next power of two, 1024 at most). */
static inline unsigned PpuGpu3DS_ScrollPictureTexHeight(unsigned height) {
    unsigned h = 8;
    while (h < height && h < 1024u) h <<= 1;
    return h;
}
void PpuGpu3DS_CacheInit(PpuGpu3DSCache* cache);
void PpuGpu3DS_CacheClearDirty(PpuGpu3DSCache* cache);
void PpuGpu3DS_CacheBeginFrame(PpuGpu3DSCache* cache, const uint16_t* bgPalette,
                               const uint16_t* objPalette, uint32_t frame);
/* CacheTile remembers this frame's lookups (PpuGpu3DSCache.memo), which
 * holds because VRAM and the palettes do not change while a frame is built.
 * Anything that changes them between two lookups of one frame calls this. */
void PpuGpu3DS_CacheForgetLookups(PpuGpu3DSCache* cache);
bool PpuGpu3DS_CacheTile(PpuGpu3DSCache* cache, const uint8_t* vram, PpuGpu3DSTileKey key,
                         uint16_t* atlas, uint16_t* outSlot);
uint8_t PpuGpu3DS_MortonIndex(unsigned x, unsigned y);
uint16_t PpuGpu3DS_PackRgba5551(uint16_t gbaColor, bool opaque);
void PpuGpu3DS_SetColorCorrection(bool enabled);
uint16_t PpuGpu3DS_PackAbgr8888(uint32_t abgr);
int32_t PpuGpu3DS_AffineSample(int32_t reference, int16_t coefficient,
                               int screenCoordinate);
int PpuGpu3DS_RemapBgX(const PpuGpu3DSFrameView* frame, unsigned bg,
                       unsigned line, int nativeX);
bool PpuGpu3DS_ShadowEntry(const PpuGpu3DSFrameView* frame, unsigned bg,
                           unsigned row, unsigned column, uint16_t* entry);
size_t PpuGpu3DS_BuildBands(const PpuGpu3DSFrameView* frame, PpuGpu3DSBand out[160]);
size_t PpuGpu3DS_WindowIntervals(unsigned left, unsigned right, unsigned width,
                                 PpuGpu3DSInterval out[2]);
void PpuGpu3DS_FillStaticIndices(uint16_t* indices, size_t capacity);
void PpuGpu3DS_CommandInit(PpuGpu3DSCommandBuffer* cmd, PpuGpu3DSVertex* vertices,
                           size_t vertexCapacity, uint16_t* indices, size_t indexCapacity,
                           PpuGpu3DSBatch* batches, size_t batchCapacity);
bool PpuGpu3DS_CommandReserve(PpuGpu3DSCommandBuffer* cmd, size_t vertices, size_t indices,
                              size_t batches);
bool PpuGpu3DS_BuildCommands(const PpuGpu3DSFrameView* frame, PpuGpu3DSCache* cache,
                             uint16_t* atlas, PpuGpu3DSCommandBuffer* cmd);
