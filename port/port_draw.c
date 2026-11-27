/*
 * port_draw.c — C ports of IWRAM overlay drawing functions.
 *
 * The original game copies ARM assembly code to IWRAM at startup
 * (from ROM at sub_080B197C). These functions process sprite frame
 * data from gFrameObjLists and write OAM entries into gOAMControls.
 *
 * Ported functions:
 *   - ram_DrawDirect    (arm_DrawDirect @ 0x080B280C)
 *   - ram_sub_080ADA04  (arm_sub_080ADA04)
 *   - ram_DrawEntities  (arm_DrawEntities @ 0x080B23F0)
 *   - DrawEntity        (port of 0x0800404C)
 *   - CheckOnScreen     (port of 0x080040A8)
 *   - sub_080B2874      (core sprite piece renderer — static)
 */

#include "color.h"
#include "entity.h"
#include "enemy.h"
#include "global.h"
#include "main.h"
#include "room.h"
#include "screen.h"
#include "sound.h"
#include "structures.h"
#include "vram.h"
#include "object.h"
#include "npc.h"
#include "player.h"

#include "port_widescreen.h"
#include "port_rom.h"
#include "cpu/mode1.h"

extern int Port_Config_GetStereoWorld(void); /* 3D WORLD on (stereo_world_on); only the 3DS build defines it */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* Forward declaration of RenderSpritePieces (defined below) */
static void RenderSpritePieces(const u8* data, s16 baseX, s16 baseY, u32 flags, u16 extra);

/* Set true only while rendering the player; drives the swamp-sink body clip. */
static int sRenderingPlayer = 0;
/* The stereo 3D depth of what is being rendered, quarter pixels (virtuappu_mode1_obj_depth, docs/stereo-3d.md):
 * what Link holds up or has just thrown stands out of the screen. */
static u8 sRenderingDepth = 0;
/* Set while an entity's pieces are written: the upper pieces of a tall sprite stand out a little (Port_Config_StereoBodyDepthQ),
 * the lowest stays on the ground's plane, so it leans out instead of floating. */
static int sRenderingBody = 0;
/* The pieces of an entity drawn in several calls (a carpenter: body, head, tool) lean as one: JoinMeasureParts measures the box
 * of all the parts first (sJoinMeasure), then each part leans from it (sJoinKnown). */
static int sJoinMeasure = 0, sJoinKnown = 0;
static s32 sJoinFeet, sJoinTop, sJoinLeft, sJoinRight;
/* The entity whose pieces are being written (1..127, 0 outside one): its OAM entries are tagged with it. Its shadow, drawn
 * in the deferred pass, is tagged with it plus SPRITE_SUPPORT_3DS_SHADOW (platform/3ds/source/sprite_support_3ds.h): the
 * GPU model gives a shadow the support of the surface under it, never its entity's height. sLastGroup is the entity drawn
 * last, whose shadow ProcessEntityForDrawNative defers. */
#include "../platform/3ds/source/sprite_support_3ds.h"
static u8 sRenderingGroup = 0;
static u8 sLastGroup = 0;
/* The big Chuchu is three entities stacked 14-18 px apart, each with its own feet: measured from those, the depth would
 * start again at 0 in every part. Its ground is the lowest bottom edge among its pieces (the shadow's), taken from the
 * frame before (the draw order of the parts is not known) and refreshed as they are drawn. */
static int sRenderingBoss = 0;
static s32 sBossGround = -0x7fffffff, sBossGroundNext = -0x7fffffff;
static s32 sBossTop = 0x7fffffff, sBossTopNext = 0x7fffffff; /* and its highest top edge, the same way */

#ifdef TMC_3DS
/* Per-entity corrections of the stereo depth, hand-curated (platform/3ds/source/port_sprite_depth.inc, optional):
 *   PORT_SPRITE_DEPTH(kind, id, type, mode)   kind: ENEMY, OBJECT...; id: the kind's own id (BOSS_DOOR...);
 *                                             type: the entity's type or PORT_ANY_TYPE
 *   PORT_DEPTH_UP:     the normal rule (what an entity without an entry gets): the ground is the bottom of its
 *                      pieces and the depth rises upwards
 *   PORT_DEPTH_DOWN or PORT_DEPTH_INVERT: the sprite hangs from the top edge (a door in a south wall): the ground is
 *                      the top of its pieces and the depth rises downwards
 *   PORT_DEPTH_LEFT:   the sprite hangs on a wall to its left (a curtain on a west wall): the ground is the right edge of
 *                      its pieces and the depth rises towards the left of the screen
 *   PORT_DEPTH_RIGHT:  the mirror (a wall to its right): ground at the left edge, the depth rising towards the right
 *   PORT_DEPTH_FLAT:   no lean at all
 *   PORT_DEPTH_WALL:   picked from where the entity stands in its room: within PORT_WALL_MARGIN pixels of the left, right or
 *                      bottom edge it is LEFT, RIGHT or DOWN (the nearest edge wins), anywhere else UP. For door-like
 *                      sprites that sit in a wall whichever way they face
 *   PORT_DEPTH_FIXED(n): no lean, the whole sprite at n quarter pixels (0..20) over the entity's own depth: for sprites that
 *                      must sit on a surface whose height is known (the boots and hands of a craftsman on his table)
 *   PORT_DEPTH_RAISED: no lean, the whole sprite at one raised depth: what the normal rule gives its top edge (the
 *                      entity's own depth plus Port_Config_StereoBodyDepthQ of its whole height, feet to the top of
 *                      its highest piece; 20 quarters at most)
 * A side lean is written as the depth at each piece's right edge (virtuappu_mode1_obj_depth) and at its left edge
 * (virtuappu_mode1_obj_depth_top, with SPRITE_LEAN_3DS_COLUMNS set, and SPRITE_LEAN_3DS_RISES_RIGHT for
 * PORT_DEPTH_RIGHT, on every piece of the sprite): the GPU model spreads it over the tile columns. */
enum {
    PORT_DEPTH_UP = 0, PORT_DEPTH_INVERT = 1, PORT_DEPTH_DOWN = PORT_DEPTH_INVERT, PORT_DEPTH_FLAT = 2,
    PORT_DEPTH_LEFT = 3, PORT_DEPTH_RIGHT = 4, PORT_DEPTH_RAISED = 5, PORT_DEPTH_WALL = 6, PORT_DEPTH_FIXED_BASE = 0x40, PORT_ANY_TYPE = 0xff
};
/* PORT_DEPTH_FIXED(n): the whole sprite at one explicit depth of n quarter pixels over the entity's own (its jump's), no lean. */
#define PORT_DEPTH_FIXED(n) (PORT_DEPTH_FIXED_BASE | (n))
/* The bits of the top depth that say the entry leans across its columns and which way (SPRITE_LEAN_3DS_COLUMNS,
 * SPRITE_LEAN_3DS_RISES_RIGHT) and the raised depth (SpriteLean3DS_RaisedDepth). */
#include "../platform/3ds/source/sprite_lean_3ds.h"
typedef struct { u8 kind, id, type, mode; } PortSpriteDepthFix;
#if defined(__has_include)
#if __has_include("../platform/3ds/source/port_sprite_depth.inc")
#define PORT_SPRITE_DEPTH(kind, id, type, mode) { (u8)(kind), (u8)(id), (u8)(type), (u8)(mode) },
#define PORT_PART_FLAT(kind, id, part)
static const PortSpriteDepthFix sSpriteDepthFixes[] = {
#include "../platform/3ds/source/port_sprite_depth.inc"
};
#undef PORT_SPRITE_DEPTH
#undef PORT_PART_FLAT
#define PORT_SPRITE_FIXES ((int)(sizeof(sSpriteDepthFixes) / sizeof(sSpriteDepthFixes[0])))
/* PORT_PART_FLAT(kind, id, part): one part of an entity drawn as several (gUnk_020000C0, `part` 0-3) stays flat, with no
 * lean, and does not count in the box the other parts lean from (a carpenter's plank lies on the ground). */
typedef struct { u8 kind, id, part; } PortPartFlatFix;
#define PORT_SPRITE_DEPTH(kind, id, type, mode)
#define PORT_PART_FLAT(kind, id, part) { (u8)(kind), (u8)(id), (u8)(part) },
static const PortPartFlatFix sPartFlats[] = {
#include "../platform/3ds/source/port_sprite_depth.inc"
};
#undef PORT_SPRITE_DEPTH
#undef PORT_PART_FLAT
#define PORT_PART_FLATS ((int)(sizeof(sPartFlats) / sizeof(sPartFlats[0])))
#endif
#endif
#ifndef PORT_SPRITE_FIXES
typedef struct { u8 kind, id, part; } PortPartFlatFix;
static const PortSpriteDepthFix sSpriteDepthFixes[1];
static const PortPartFlatFix sPartFlats[1];
#define PORT_SPRITE_FIXES 0
#define PORT_PART_FLATS 0
#endif
static int sRenderingInvert = 0, sRenderingFlat = 0;
static int sRenderingSide = 0; /* PORT_DEPTH_LEFT or PORT_DEPTH_RIGHT: the depth rises across the sprite, not up or down */
static int sRenderingFixedQ = -1; /* PORT_DEPTH_FIXED(n): n quarters over the entity's own depth, -1 when off */
static int sRenderingRaised = 0; /* PORT_DEPTH_RAISED: one depth for the whole sprite, its top edge's */
/* PORT_DEPTH_WALL: the wall the entity stands in, from its place in the room (the nearest of the left, right and bottom
 * edges within PORT_WALL_MARGIN pixels; the top edge keeps the normal rule). */
enum { PORT_WALL_MARGIN = 24 };
static int WallDepthMode(const Entity* entity) {
    const int x = entity->x.HALF.HI - gRoomControls.origin_x, y = entity->y.HALF.HI - gRoomControls.origin_y;
    const int left = x, right = gRoomControls.width - x, bottom = gRoomControls.height - y;
    int best = PORT_WALL_MARGIN + 1, mode = PORT_DEPTH_UP;
    if (left >= 0 && left < best) { best = left; mode = PORT_DEPTH_LEFT; }
    if (right >= 0 && right < best) { best = right; mode = PORT_DEPTH_RIGHT; }
    if (bottom >= 0 && bottom < best) { best = bottom; mode = PORT_DEPTH_DOWN; }
    return mode;
}

static int PartIsFlat(const Entity* entity, int part) {
    for (int i = 0; i < PORT_PART_FLATS; ++i)
        if (sPartFlats[i].kind == entity->kind && sPartFlats[i].id == entity->id && sPartFlats[i].part == part) return 1;
    return 0;
}

static int SpriteDepthMode(const Entity* entity) {
    for (int i = 0; i < PORT_SPRITE_FIXES; ++i) {
        const PortSpriteDepthFix* f = &sSpriteDepthFixes[i];
        if (f->kind == entity->kind && f->id == entity->id && (f->type == PORT_ANY_TYPE || f->type == entity->type))
            return f->mode == PORT_DEPTH_WALL ? WallDepthMode(entity) : f->mode;
    }
    return 0;
}
#endif

/* The file select drawn at 400x240 (Port_Widescreen_FileSelectWide): the game keeps placing its
 * sprites on the GBA's 240x160 screen; each drawn thing (an entity, a DrawDirect) is moved into
 * the middle of the view as one, after its pieces are written (WideSpritesEnd). Its pieces are
 * kept unclipped meanwhile, with their positions here. */
static int sWideCollect;
static s16 sWidePieceX[0x80], sWidePieceY[0x80];
static u8 sWidePieceW[0x80], sWidePieceH[0x80];
extern PlayerState gPlayerState;
/* Region-select a ROM offset by the loaded ROM's game code (defined below). */
static u32 RegionRomOffset(u32 usa, u32 eu, u32 jp);

/* Shoes-overlay frame table (loaded from ROM 0x080B2B58, see comment below
 * the table definition). Forward-declared here because ProcessEntityForDraw
 * uses them and is defined above the loader. */
static const u8* sShoesOverlayPtrs[32];
static int sShoesOverlayTableLoaded;
static void LoadShoesOverlayTableFromRom(void);
extern u32 gFrameObjLists[50016];

static inline u32 ReadU32Unaligned(const void* p) {
    u32 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* ---- Struct layout invariant (compile-time) ----
 * ResolveEntitySpriteParams reads the 4 sprite fields at offset 0x18 (GBA) as
 * a single u32; any padding between them breaks sprite rendering completely. */
_Static_assert(offsetof(Entity, spriteRendering) == offsetof(Entity, spriteSettings) + 1,
               "Entity.spriteRendering must immediately follow spriteSettings");
_Static_assert(offsetof(Entity, palette) == offsetof(Entity, spriteSettings) + 2,
               "Entity.palette must be 2 bytes after spriteSettings");
_Static_assert(offsetof(Entity, spriteOrientation) == offsetof(Entity, spriteSettings) + 3,
               "Entity.spriteOrientation must be 3 bytes after spriteSettings");

/* ---- Size/clipping table from ROM overlay (ram_0x80b2be8) ----
 * 60 entries × 4 bytes = 240 bytes.
 * Each entry: { xAnchor, yAnchor, xSize, ySize }
 * Sub-tables at offsets 0, 0x30, 0x60, 0x90, 0xC0 correspond to
 * different anchor modes (no anchor, x-anchor, y-anchor, xy-anchor,
 * double-size affine).
 */
static u8 sSizeTable[240];
static int sSizeTableLoaded = 0;

extern const u8 kOverlaySizeData[240];

static int IsValidSizeTable(const u8* data, u32 size) {
    /* This overlay table describes the fixed GBA OBJ shapes and sizes.  It is
     * region-invariant in the retail USA and EU ROMs.  Validate the complete
     * table so a bad regional offset cannot silently turn nearby code pointers
     * into clipping anchors again. */
    return data != NULL && size >= sizeof(sSizeTable) &&
           memcmp(data, kOverlaySizeData, sizeof(sSizeTable)) == 0;
}

/* Called from port_rom.c after ROM is loaded */
void Port_LoadOverlayData(const u8* romData, u32 romSize, u32 overlayOffset) {
    /* Size table at region-specific ROM offset, 240 bytes */
    if (romData != NULL && overlayOffset <= romSize && sizeof(sSizeTable) <= romSize - overlayOffset &&
        IsValidSizeTable(&romData[overlayOffset], romSize - overlayOffset)) {
        memcpy(sSizeTable, &romData[overlayOffset], 240);
        sSizeTableLoaded = 1;
    } else {
        memcpy(sSizeTable, kOverlaySizeData, sizeof(sSizeTable));
        sSizeTableLoaded = 1;
        fprintf(stderr,
                "WARNING: OBJ size table at ROM 0x%X is invalid; using the canonical table.\n",
                overlayOffset);
    }
}

/* Called from port_rom.c — load overlay data from compile-time const blob */
void Port_LoadOverlayDataFromConst(const u8* data, u32 size) {
    if (IsValidSizeTable(data, size)) {
        memcpy(sSizeTable, data, 240);
        sSizeTableLoaded = 1;
    }
}

/* ====================================================================
 *  ram_UpdateEntities — Port of arm_UpdateEntities @ 0x080B21B0
 * ==================================================================== */

/* Entity update function declarations (implemented in respective .c files or stubs) */
extern void PlayerUpdate(Entity* entity);
extern void EnemyUpdate(Entity* entity);
extern void ProjectileUpdate(Entity* entity);
extern void ObjectUpdate(Entity* entity);
extern void NPCUpdate(Entity* entity);
extern void ItemUpdate(Entity* entity);
extern void ManagerUpdate(Entity* entity);
extern void DeleteThisEntity(void);

/* Update function table indexed by EntityKind (0-9) */
typedef void (*EntityUpdateFn)(Entity*);

static void DeleteThisEntityWrapper(Entity* unused) {
    DeleteThisEntity();
}

/* Initialized dynamically due to static init issues on Windows/MinGW */
static EntityUpdateFn sEntityUpdateFuncs[10] = { 0 };
static int sEntityUpdateFuncsInited = 0;

static void InitEntityUpdateFuncs(void) {
    if (sEntityUpdateFuncsInited)
        return;
    sEntityUpdateFuncs[0] = DeleteThisEntityWrapper; /* 0 = invalid */
    sEntityUpdateFuncs[1] = PlayerUpdate;            /* 1 = PLAYER */
    sEntityUpdateFuncs[2] = DeleteThisEntityWrapper; /* 2 = invalid */
    sEntityUpdateFuncs[3] = EnemyUpdate;             /* 3 = ENEMY */
    sEntityUpdateFuncs[4] = ProjectileUpdate;        /* 4 = PROJECTILE */
    sEntityUpdateFuncs[5] = DeleteThisEntityWrapper; /* 5 = invalid */
    sEntityUpdateFuncs[6] = ObjectUpdate;            /* 6 = OBJECT */
    sEntityUpdateFuncs[7] = NPCUpdate;               /* 7 = NPC */
    sEntityUpdateFuncs[8] = ItemUpdate;              /* 8 = PLAYER_ITEM */
    sEntityUpdateFuncs[9] = ManagerUpdate;           /* 9 = MANAGER */
    sEntityUpdateFuncsInited = 1;
}

extern UpdateContext gUpdateContext;

/* Per-entity enemy-target cache. arm_UpdateEntities zeroes this before every
 * entity update (the `mov r0,#0; str r0,[r7]` where r7 = &gEnemyTarget, per the
 * gUnk_080026A4 parameter block, asm/src/code_08001A7C.s:863-873). The cache
 * (sub_08049DF4) returns it when non-NULL, so without the per-entity reset a
 * permissive enemy's target leaks into a stricter one (e.g. an enemy that should
 * only target a minish Link). */
extern Entity* gEnemyTarget;

/* External function called after each entity update */
extern void UpdateCollision(Entity* entity);

/* Forward declaration of helper that resets the entity sprite draw
 * lists. Full definition (and the sDrawLists storage it operates on)
 * is later in this file. Used by ram_UpdateEntities to clear the lists
 * at the *start* of the entity-update phase so that during
 * GAMEMAIN_CHANGEAREA / similar non-update phases, the previously
 * registered entities continue to render at their last positions
 * (matches GBA behavior; without this, door arch / Link sprites
 * vanish on the second frame of fade-out). */
static void ClearEntityDrawLists(void);

/* ram_UpdateEntities (port of arm_UpdateEntities)
 *
 * Arguments:
 *   mode = 0: update all entities (lists 0-7)
 *   mode = 1: update managers only (list 8)
 */
static int sUpdateEntitiesCalls = 0;
static int sEntitiesUpdated = 0;

static jmp_buf sEntityUpdateJmpBuf;
static int sEntityUpdateJmpBufValid = 0;

void ram_UpdateEntities(u32 mode) {
    sUpdateEntitiesCalls++;

    /* #93 chase: per-frame integrity check on tracked orchestrator
     * entities. Detects state changes that bypass the standard
     * delete/unlink paths. */
    {
        extern void Port_CheckOrchIntegrity(unsigned phase, const char* where);
        Port_CheckOrchIntegrity(mode, "before-update");
    }

    /* Initialize function table on first call */
    InitEntityUpdateFuncs();

    int startList, endList;

    if (mode == 0) {
        /* Entities: lists 0-7 (excluding manager list) */
        startList = 0;
        endList = 8;
        /* Clear sprite draw lists at the start of the entity-update
         * phase, NOT at the end of ram_DrawEntities. This matches GBA
         * behavior: each entity's update re-registers itself via
         * DrawEntity, and during GAMEMAIN_CHANGEAREA (no UpdateEntities
         * call) the draw lists persist so previously-registered entities
         * continue to render at their last positions. Without this,
         * during fade-out a door arch / door slab / Link sprites all
         * vanish on frame 2+ of the transition, exposing the
         * transparent BG hole at the doorway as the fading backdrop
         * color.. */
        ClearEntityDrawLists();
    } else {
        /* Managers: only list 8 */
        startList = 8;
        endList = 9;
    }

    for (int listIdx = startList; listIdx < endList; listIdx++) {
        LinkedList* list = &gEntityLists[listIdx];
        Entity* entity = list->first;

        while (entity != NULL && entity != (Entity*)list) {
            /* Sanity-check: entity must be a known entity address —
             * inside gEntities, gAuxPlayerEntities, the manager pool,
             * == &gPlayerEntity, or a gEntityLists head sentinel.
             * Anything else is a stale pointer (v1 quicksave fallout
             * or list corruption); bail out before the deref instead
             * of SIGSEGVing. Reuses Port_IsValidEntityAddr from
             * src/entity.c which already encodes the full valid
             * region set — checking only gEntities (as an earlier
             * version of this guard did) rejects gPlayerEntity and
             * makes Link invisible. */
            {
                extern int Port_IsValidEntityAddr(const void* p);
                int in_pool = Port_IsValidEntityAddr(entity);
                if (!in_pool) {
                    for (int k = 0; k < 9; ++k) {
                        if (entity == (Entity*)&gEntityLists[k]) {
                            in_pool = 1;
                            break;
                        }
                    }
                }
                if (!in_pool) {
                    fprintf(stderr,
                            "[ram_UpdateEntities] stale entity pointer %p in "
                            "list %d — bailing out (likely v1 quicksave or "
                            "list corruption)\n",
                            (void*)entity, listIdx);
                    break;
                }
            }
            /* Clear the enemy-target cache before this entity's update, exactly
             * as arm_UpdateEntities does (str #0 to gEnemyTarget) before storing
             * current_entity. Applies to both entity and manager passes. */
            gEnemyTarget = NULL;

            /* Save current entity in context */
            gUpdateContext.current_entity = entity;

            /* Call appropriate update function based on entity kind */
            u8 kind = entity->kind;

            sEntitiesUpdated++;

            if (kind < 10) {

                sEntityUpdateJmpBufValid = 1;
                if (setjmp(sEntityUpdateJmpBuf) == 0) {
                    /* Normal path: call entity update */
                    sEntityUpdateFuncs[kind](entity);
                }
                sEntityUpdateJmpBufValid = 0;
            }

            /* GBA arm_UpdateEntities (asm/src/intr.s:742-746) reloads
             * current_entity AFTER the update and computes the successor as
             * current_entity->next — it does NOT snapshot entity->next before
             * the update (the earlier port did, which diverges from console).
             * Reading the successor late matters in two ways:
             *   - self-delete: UnlinkEntity sets current_entity = ent->prev and
             *     relinks prev->next = ent->next, so current_entity->next is the
             *     correct successor (src/entity.c:927-963);
             *   - an entity that deletes its OWN successor during its update: the
             *     late read skips the now-unlinked node, whereas a pre-captured
             *     `next` walks into the freed/recycled entity.
             * Capture current_entity once (matches the single `ldr r0,[r11,#8]`)
             * and reuse it for the collision check and the advance. */
            Entity* cur = gUpdateContext.current_entity;

            /* Update collision if entity is still alive (cmp r0,r4; bleq) */
            if (cur == entity) {
                UpdateCollision(entity);
            }

            /* Advance: current_entity->next, read late (ldr r4,[r0,#4]).
             * UnlinkEntity never nulls current_entity mid-list, so NULL here
             * means corruption — stop the walk; the top-of-loop guard re-validates
             * the result on the next iteration either way. */
            entity = (cur != NULL) ? cur->next : NULL;
        }
    }

    /* Clear current entity context */
    gUpdateContext.current_entity = NULL;
}

void ram_ClearAndUpdateEntities(void) {
    if (sEntityUpdateJmpBufValid) {
        longjmp(sEntityUpdateJmpBuf, 1);
    }
    /* If not in an entity update context, just return (shouldn't happen normaly ( I hope :-) ) */
}

static const u8* LookupFrameData(u16 spriteIndex, u8 frameIndex) {
    const size_t frameObjSize = Port_FrameObjListsSizeForRegion(gRomRegion);
    const u8* base = (const u8*)gFrameObjLists;

    if ((u32)spriteIndex >= Port_FrameObjCountForRegion(gRomRegion)) {
        return NULL;
    }

    u32 off1 = gFrameObjLists[spriteIndex];
    if ((size_t)off1 > frameObjSize - sizeof(u32)) {
        return NULL;
    }

    size_t frameEntry = (size_t)off1 + (size_t)frameIndex * sizeof(u32);
    if (frameEntry > frameObjSize - sizeof(u32)) {
        return NULL;
    }

    u32 off2 = ReadU32Unaligned(base + frameEntry);
    if ((size_t)off2 >= frameObjSize) {
        return NULL;
    }

    return base + off2;
}

/* ---- Core sprite piece renderer (port of sub_080B2874) ----
 *
 * Frame data format:
 *   byte 0: piece count
 *   Then for each piece, 5 bytes:
 *     byte 0: signed x offset
 *     byte 1: signed y offset
 *     byte 2: shape/size/flags
 *       bits 7-6: OAM shape (0=square, 1=h-rect, 2=v-rect)
 *       bits 5-4: OAM size (0-3)
 *       bits 3-2: flip bits (XOR'd with base flags)
 *       bit 1: unused
 *       bit 0: if set, clear palette bits from attr2
 *     byte 3: tile index low byte
 *     byte 4: tile index high / palette addend
 */
#ifdef TMC_3DS
/* The box a sprite's pieces cover: the bottom edge of the lowest (its feet), the top edge of the highest, the left and right
 * edges (for a side lean). Extends what is in the out parameters. */
static void ScanPieceBox(const u8* data, u8 count, u32 flags, s16 baseX, s16 baseY, const u8* sizeTab, u32 tableOff,
                         s32* feetBottom, s32* topMin, s32* leftMin, s32* rightMax) {
    const u8* scan = data;
    for (int i = 0; i < count; i++, scan += 5) {
        s8 scanYoff = (s8)scan[1];
        if (!(flags & 0x300) && (flags & 0x20000000))
            scanYoff = -scanYoff;
        const u32 scanSize = (scan[2] & 0xF0) >> 2;
        if ((size_t)tableOff + scanSize + 3 >= sizeof(sSizeTable))
            continue;
        const u8* scanEntry = sizeTab + scanSize;
        const s32 bottom = (s32)scanYoff + (s32)baseY - (s32)scanEntry[1] + (s32)scanEntry[3];
        s8 scanXoff = (s8)scan[0];
        if (!(flags & 0x300) && (flags & 0x10000000))
            scanXoff = -scanXoff;
        const s32 left = (s32)scanXoff + (s32)baseX - (s32)scanEntry[0];
        if (left < *leftMin)
            *leftMin = left;
        if (left + (s32)scanEntry[2] > *rightMax)
            *rightMax = left + (s32)scanEntry[2];
        if (bottom > *feetBottom)
            *feetBottom = bottom;
        if (bottom - (s32)scanEntry[3] < *topMin)
            *topMin = bottom - (s32)scanEntry[3];
        if (sRenderingBoss && bottom - (s32)scanEntry[3] < sBossTopNext)
            sBossTopNext = bottom - (s32)scanEntry[3];
    }
}
#endif

static void RenderSpritePieces(const u8* data, /* pointer to frame data (count byte + pieces) */
                               s16 baseX,      /* OAMCommand.x */
                               s16 baseY,      /* OAMCommand.y */
                               u32 flags,      /* *(u32*)&cmd->_4 : affine/flip/mode flags */
                               u16 extra       /* cmd->_8 : base tile + priority + palette */
) {
    u8 count = *data++;
    if (count == 0)
        return;
    /* Bound the piece read to the frame-object buffer. `count` (and the 5-byte
     * pieces that follow) come from gFrameObjLists, which LookupFrameData only
     * range-checks up to the count byte itself — a corrupt/truncated ROM can
     * leave `count` claiming more pieces than remain, reading past the array.
     * When `data` lies inside gFrameObjLists, clamp to the bytes available.
     * (Overlay callers pass small fixed internal buffers, not ROM data.) */
    {
        const u8* fobBase = (const u8*)gFrameObjLists;
        const u8* fobEnd = fobBase + Port_FrameObjListsSizeForRegion(gRomRegion);
        if (data >= fobBase && data < fobEnd) {
            size_t maxPieces = (size_t)(fobEnd - data) / 5u;
            if ((size_t)count > maxPieces)
                count = (u8)maxPieces;
            if (count == 0)
                return;
        }
    }
    if (gOAMControls.updated >= 0x80)
        return;

    /* Determine size table sub-table offset based on flags */
    u32 tableOff;
    if (flags & 0x300) {
        /* Affine mode: bit 8 set */
        if ((flags & 0x300) != 0x100)
            tableOff = 0xC0; /* double-size affine */
        else
            tableOff = 0;
    } else {
        /* Normal mode: offset based on bits 28-29 */
        u32 v = (flags >> 24) & 0x30;
        tableOff = v + (v << 1); /* v * 3 */
    }
    const u8* sizeTab = &sSizeTable[tableOff];
#ifdef TMC_3DS
    if (sJoinMeasure) {
        if (!sRenderingFlat)
            ScanPieceBox(data, count, flags, baseX, baseY, sizeTab, tableOff, &sJoinFeet, &sJoinTop, &sJoinLeft, &sJoinRight);
        return;
    }
#endif

    u8 updated = gOAMControls.updated;

    /* --- Swamp-sink per-pixel waterline clip (ViruaPPU obj-clip-y patch) ---
     * GBA hides Link's lower body in the swamp by omitting his lower sprite rows
     * (the swamp BG is opaque over his whole sprite, so OBJ priority can't do a
     * partial hide). We mark Link's OAM entries and a waterline screen-Y; ViruaPPU
     * drops those entries' pixels at scanline >= waterline, so the line rises
     * feet->head smoothly as the sink timer climbs. Reset once per frame (the
     * first emitted sprite has updated==0, since FlushSprites zeroes it). */
    extern PlayerEntity gPlayerEntity;
    if (updated == 0) {
        sBossGround = sBossGroundNext;
        sBossGroundNext = -0x7fffffff;
        sBossTop = sBossTopNext;
        sBossTopNext = 0x7fffffff;
        memset(virtuappu_mode1_obj_clip_mark_staged, 0, MODE1_GBA_OAM_COUNT);
        memset(virtuappu_mode1_obj_y_negative_staged, 0, MODE1_GBA_OAM_COUNT);
        memset(virtuappu_mode1_obj_depth_staged, 0, MODE1_GBA_OAM_COUNT);
        memset(virtuappu_mode1_obj_depth_top_staged, 0, MODE1_GBA_OAM_COUNT);
        memset(virtuappu_mode1_obj_group_staged, 0, MODE1_GBA_OAM_COUNT);
        virtuappu_mode1_obj_clip_enable_staged = 0;
    }
    int sSwampClipActive = (sRenderingPlayer && gPlayerState.floor_type == SURFACE_SWAMP &&
                            gPlayerState.jump_status == 0 && gPlayerEntity.base.z.HALF.HI <= 0);
    if (sSwampClipActive) {
        /* Waterline rises feet->head as the sink timer climbs. Tune here:
         * bigger divisor = slower cover; lower cap = less of him buried. */
        s32 sub = 2 + ((s32)gPlayerState.surfaceTimer / 48); /* slow, shallow climb */
        if (sub > 8)
            sub = 8; /* keep him ~knee/waist deep at most — never head-only */
        virtuappu_mode1_obj_clip_y_staged = (int)baseY - sub;
        virtuappu_mode1_obj_clip_enable_staged = 1;
    }
    /* OAM entries start at offset 0x20 in OAMControls, each 8 bytes */
    u8* oamBase = (u8*)&gOAMControls.oam[0];
    u8* ip = oamBase + updated * 8;

#ifdef TMC_3DS
    /* The bottom edge of the lowest piece: the sprite's feet. */
    extern int Port_Config_StereoBodyDepthQ(int height);
    /* A sprite of several pieces leans out with its height over its feet; so does a single tall piece (the big Chuchu is
     * one 64 px piece per part): see the per-piece test below. */
    const int bodyShear = sRenderingBody && !sRenderingFlat;
    s32 feetBottom = -0x7fffffff;
    s32 topMin = 0x7fffffff; /* the highest top edge of its pieces: the ground of a sprite that hangs (PORT_DEPTH_INVERT) */
    /* Its left and right edges: the ground of a sprite on a side wall (PORT_DEPTH_RIGHT, PORT_DEPTH_LEFT). */
    s32 leftMin = 0x7fffffff, rightMax = -0x7fffffff;
    if (bodyShear) {
        ScanPieceBox(data, count, flags, baseX, baseY, sizeTab, tableOff, &feetBottom, &topMin, &leftMin, &rightMax);
        if (sJoinKnown) {
            /* A part of an entity of several: the lean is the whole entity's (JoinMeasureParts). */
            feetBottom = sJoinFeet;
            topMin = sJoinTop;
            leftMin = sJoinLeft;
            rightMax = sJoinRight;
        }
        if (sRenderingBoss) {
            if (feetBottom > sBossGroundNext)
                sBossGroundNext = feetBottom;
            if (sBossGround > feetBottom)
                feetBottom = sBossGround;
        }
    }
    /* PORT_DEPTH_RAISED: what the normal rule gives the sprite's top edge (its whole height over its feet), for all of
     * it, whether or not the normal rule would lean it (it does not lean one piece 16 px tall or less). */
    const u8 raisedQ = (u8)SpriteLean3DS_RaisedDepth(
        (int)sRenderingDepth,
        sRenderingFixedQ >= 0 ? sRenderingFixedQ
        : bodyShear && sRenderingRaised && feetBottom > topMin ? Port_Config_StereoBodyDepthQ((int)(feetBottom - topMin)) : 0);
#endif

    for (int i = 0; i < count; i++) {
        if (updated >= 0x80) {
            gOAMControls.updated = 0x80;
            return;
        }

        /* Read 5-byte piece */
        s8 xoff = (s8)data[0];
        s8 yoff = (s8)data[1];
        u8 shapeInfo = data[2];
        u8 tileLow = data[3];
        u8 tileHigh = data[4];
        data += 5;

        /* Apply flip if not in affine mode */
        if (!(flags & 0x300)) {
            if (flags & 0x20000000)
                yoff = -yoff; /* v-flip */
            if (flags & 0x10000000)
                xoff = -xoff; /* h-flip */
        }

        /* Screen position */
        s32 y = (s32)yoff + (s32)baseY;
        s32 x = (s32)xoff + (s32)baseX;

        /* Size table lookup for clipping */
        u32 sizeIdx = (shapeInfo & 0xF0) >> 2; /* 4-byte entries */
        if ((size_t)tableOff + sizeIdx + 3 >= sizeof(sSizeTable)) {
            continue;
        }
        const u8* se = sizeTab + sizeIdx;

        /* Clipping */
        y -= (s32)se[1]; /* subtract y anchor */
        u8 pieceDepth = sRenderingDepth;
        u8 pieceDepthTop = sRenderingDepth;
#ifdef TMC_3DS
        u8 pieceColumns = 0;
        if (bodyShear && (sRenderingRaised || sRenderingFixedQ >= 0)) {
            pieceDepth = pieceDepthTop = raisedQ;
        } else if (bodyShear && sRenderingSide) {
            /* On a side wall: the same rise as the normal rule, measured from the edge it hangs on across the sprite's
             * width (in the GPU renderer, column by column of tiles). The depth at the piece's right edge goes in the
             * entry's depth, the one at its left edge in its top depth. */
            const s32 left = x - (s32)se[0], right = left + (s32)se[2];
            const s32 fromLeft = sRenderingSide == PORT_DEPTH_RIGHT ? left - leftMin : rightMax - right;
            const int near = (int)(fromLeft > 0 ? fromLeft : 0); /* the piece's edge nearer the ground */
            const int nearQ = Port_Config_StereoBodyDepthQ(near), farQ = Port_Config_StereoBodyDepthQ(near + (int)se[2]);
            const int sumRight = (int)sRenderingDepth + (sRenderingSide == PORT_DEPTH_RIGHT ? farQ : nearQ);
            const int sumLeft = (int)sRenderingDepth + (sRenderingSide == PORT_DEPTH_RIGHT ? nearQ : farQ);
            pieceDepth = (u8)(sumRight > 20 ? 20 : sumRight);
            pieceDepthTop = (u8)(sumLeft > 20 ? 20 : sumLeft);
            /* On every piece, even one whose edges have the same depth: the renderer splits the shifts of all the pieces
             * of the sprite between the eyes the same way, so neighbouring columns never part (SpriteLean3DS_EyeShift). */
            pieceColumns = (u8)(SPRITE_LEAN_3DS_COLUMNS | (sRenderingSide == PORT_DEPTH_RIGHT ? SPRITE_LEAN_3DS_RISES_RIGHT : 0));
        } else if (bodyShear && (count > 1 || se[3] > 16 || sRenderingInvert || sJoinKnown)) {
            /* The depth rises with the height over the feet: at the piece's bottom edge, and at its top edge, so the
             * GPU renderer can give each tile row of the piece its own (the lean is then not one step per piece). */
            const s32 above = feetBottom - (y + (s32)se[3]); /* how far this piece's bottom is over the feet */
            /* The big Chuchu spreads the same rise over its whole height (ground to the top of its head). */
            const s32 bossHeight = sRenderingBoss && sBossGround > sBossTop ? sBossGround - sBossTop : 0;
            const int aboveBottom = (int)(above > 0 ? above : 0), aboveTop = aboveBottom + (int)se[3];
            const int bottomQ = above <= 0 ? 0
                                : bossHeight > 16 ? (int)(aboveBottom * 16 / bossHeight > 16 ? 16 : aboveBottom * 16 / bossHeight)
                                                  : Port_Config_StereoBodyDepthQ(aboveBottom);
            /* The lowest piece of the boss is its shadow (a dark ellipse in a 64 px piece of its own): flat on the ground, so
             * it stays at 0; otherwise it would be drawn in front of the feet that stand on it. */
            const int topQ = sRenderingBoss && above <= 0 ? 0 : bossHeight > 16 ? (int)(aboveTop * 16 / bossHeight > 16 ? 16 : aboveTop * 16 / bossHeight)
                                             : Port_Config_StereoBodyDepthQ(aboveTop);
            int sumBottom = (int)sRenderingDepth + bottomQ, sumTop = (int)sRenderingDepth + topQ;
            if (sRenderingInvert) {
                /* Hanging from its top edge: the depth is nil there and grows downwards. */
                const int belowTop = (int)(y - topMin > 0 ? y - topMin : 0);
                sumBottom = (int)sRenderingDepth + Port_Config_StereoBodyDepthQ(belowTop + (int)se[3]);
                sumTop = (int)sRenderingDepth + Port_Config_StereoBodyDepthQ(belowTop);
            }
            pieceDepth = (u8)(sumBottom > 20 ? 20 : sumBottom);
            pieceDepthTop = (u8)(sumTop > 20 ? 20 : sumTop);
            /* ...and it does not rise with the boss's jump either: its shadow stays where the ground is. */
            if (sRenderingBoss && above <= 0) pieceDepth = pieceDepthTop = 0;
        }
#endif
        if (sWideCollect) {
            /* Placed and clipped by WideSpritesEnd. */
            x -= (s32)se[0];
            sWidePieceX[updated] = (s16)x;
            sWidePieceY[updated] = (s16)y;
            sWidePieceW[updated] = se[2];
            sWidePieceH[updated] = se[3];
        } else if (y >= Port_Widescreen_ProducerViewHeight()) {
            continue;
        } else if (y + (s32)se[3] <= 0) {
            continue;
        } else {
            x -= (s32)se[0]; /* subtract x anchor */
            if (x >= Port_Widescreen_ProducerViewWidth()) {
                continue;
            }
            if (x + (s32)se[2] <= 0) {
                continue;
            }
        }

        /* Build combined attr0|attr1 as 32-bit word (little-endian):
         *   bits 0-7:   attr0.y
         *   bits 8-15:  attr0 flags (from 'flags' parameter)
         *   bits 14-15: attr0.shape
         *   bits 16-24: attr1.x
         *   bits 25-29: attr1.matrixNum/flip (from 'flags')
         *   bits 30-31: attr1.size
         */
        virtuappu_mode1_obj_y_negative_staged[updated] = y < 0;
        u32 oamWord = (u32)(y & 0xFF);            /* y position */
        oamWord |= (u32)((x & 0x1FF)) << 16;      /* x position */
        oamWord |= flags;                         /* base flags */
        oamWord |= (u32)(shapeInfo & 0xC0) << 8;  /* shape → attr0 bits 14-15 */
        oamWord ^= (u32)(shapeInfo & 0x3C) << 26; /* flip/size → attr1 bits 12-15 */

        memcpy(ip, &oamWord, sizeof(oamWord));
        ip += 4;

        /* Build attr2: tile number + priority + palette */
        u16 attr2 = (u16)tileLow + extra;
        if (shapeInfo & 1) {
            attr2 &= 0x0FFF; /* clear palette bits */
        }
        attr2 += (u16)tileHigh << 8;

        memcpy(ip, &attr2, sizeof(attr2));
        ip += 4; /* skip affineParam (2 bytes attr2 + 2 bytes padding) */

        /* Swamp sink: mark this player OAM entry for the per-pixel waterline
         * clip in ViruaPPU (see RenderSpritePieces top + port_gba_mem). */
        if (sSwampClipActive)
            virtuappu_mode1_obj_clip_mark_staged[updated & 0x7F] = 1;
        /* The HUD's sprites (priority 0) take no depth from their pieces' height: they keep the entity's own (none for a HUD element), and with it the
         * layer's, which is in front of the world's whatever the world does. */
        if (((attr2 >> 10) & 3u) == 0) {
            pieceDepth = pieceDepthTop = sRenderingDepth;
#ifdef TMC_3DS
            pieceColumns = 0;
#endif
        }
        /* Every piece of the big Chuchu is drawn with the layers in front (bit 7), moved or not: split between the base and
         * the front, its parts would cover one another in the wrong order. */
        virtuappu_mode1_obj_depth_staged[updated & 0x7F] = (u8)(pieceDepth | (sRenderingBoss && Port_Config_GetStereoWorld() ? 0x80 : 0));
#ifdef TMC_3DS
        virtuappu_mode1_obj_depth_top_staged[updated & 0x7F] = (u8)(pieceDepthTop | pieceColumns);
#else
        virtuappu_mode1_obj_depth_top_staged[updated & 0x7F] = pieceDepthTop;
#endif
        virtuappu_mode1_obj_group_staged[updated & 0x7F] = sRenderingGroup;

        updated++;
    }

    gOAMControls.updated = updated;
}

/* How far a thing spanning [lo, hi) on one axis of the GBA's screen (size `native`) is moved in a
 * view `margin` larger on each side: by `margin` while it is all on the GBA's screen, and across
 * the edge it is crossing faster, so that it leaves the view as it leaves the GBA's screen (the
 * file rows the game parks at x = -72 leave by the 400x240 view's edge, not in its middle). The
 * one-pixel sliver some leave on the GBA's edge (those rows' outline) is not carried over. */
static s32 WideCarry(s32 lo, s32 hi, s32 native, s32 margin) {
    const s32 size = hi - lo;
    if (size <= 1 || (lo < 0 && hi > native)) return margin;
    if (lo < 0) return hi <= 1 ? -hi : margin * (hi - 1) / (size - 1);
    if (hi > native) return lo >= native - 1 ? native + 2 * margin - lo : margin + margin * (hi - native) / (size - 1);
    return margin;
}

/* The pause menu's sprites (Port_Widescreen_PauseWide) move into the middle as they are: what the game slides
 * past the GBA's screen (the L and R buttons when pressed) stays whole; what is wholly off it stays hidden. */
static int sWidePause;
/* Port_Draw_SetPauseHudBeside: the pause item page's A, B, X and Y are put where the game's HUD has them in the 400
 * view, right of the GBA's screen and X above its top, so their pieces are kept wherever they fall in the view. */
static int sPauseHudBeside;

void Port_Draw_SetPauseHudBeside(int on) {
    sPauseHudBeside = on != 0;
}

static int WideSpritesBegin(void) {
    if (sWideCollect) return -1;
    sWidePause = !Port_Widescreen_FileSelectWide() && Port_Widescreen_PauseWide();
    if (!sWidePause && !Port_Widescreen_FileSelectWide()) return -1;
    sWideCollect = 1;
    return gOAMControls.updated;
}

int Port_Draw_WideGroupBegin(void) {
    return WideSpritesBegin();
}

/* The pieces written since WideSpritesBegin, moved into the 400x240 view as one and clipped to it. */
static void WideSpritesEnd(int first) {
    if (first < 0) return;
    sWideCollect = 0;
    const int end = gOAMControls.updated < 0x80 ? gOAMControls.updated : 0x80;
    if (first >= end) return;
    s32 left = 0x7fff, right = -0x8000, top = 0x7fff, bottom = -0x8000;
    for (int i = first; i < end; i++) {
        if (sWidePieceX[i] < left) left = sWidePieceX[i];
        if (sWidePieceX[i] + sWidePieceW[i] > right) right = sWidePieceX[i] + sWidePieceW[i];
        if (sWidePieceY[i] < top) top = sWidePieceY[i];
        if (sWidePieceY[i] + sWidePieceH[i] > bottom) bottom = sWidePieceY[i] + sWidePieceH[i];
    }
    if (sWidePause && !sPauseHudBeside && (right <= 0 || left >= 240 || bottom <= 0 || top >= 160)) {
        gOAMControls.updated = (u8)first;
        return;
    }
    const s32 dx = sWidePause ? 80 : WideCarry(left, right, 240, 80);
    const s32 dy = sWidePause ? 40 : WideCarry(top, bottom, 160, 40);
    u8* oam = (u8*)&gOAMControls.oam[0];
    int out = first;
    for (int i = first; i < end; i++) {
        const s32 x = sWidePieceX[i] + dx, y = sWidePieceY[i] + dy;
        if (x >= 400 || x + sWidePieceW[i] <= 0 || y >= 240 || y + sWidePieceH[i] <= 0) continue;
        /* attr0-attr2 only: the fourth halfword of each entry is an affine parameter. */
        u16 attr[3];
        memcpy(attr, oam + i * 8, sizeof(attr));
        attr[0] = (u16)((attr[0] & ~0xFFu) | ((u32)y & 0xFFu));
        attr[1] = (u16)((attr[1] & ~0x1FFu) | ((u32)x & 0x1FFu));
        memcpy(oam + out * 8, attr, sizeof(attr));
        virtuappu_mode1_obj_y_negative_staged[out] = y < 0;
        virtuappu_mode1_obj_clip_mark_staged[out] = virtuappu_mode1_obj_clip_mark_staged[i];
        virtuappu_mode1_obj_depth_staged[out] = virtuappu_mode1_obj_depth_staged[i];
        virtuappu_mode1_obj_depth_top_staged[out] = virtuappu_mode1_obj_depth_top_staged[i];
        virtuappu_mode1_obj_group_staged[out] = virtuappu_mode1_obj_group_staged[i];
        out++;
    }
    gOAMControls.updated = (u8)out;
}

void Port_Draw_WideGroupEnd(int first) {
    WideSpritesEnd(first);
}

/* The pause menu's title banner (INVENTORY, MAP...) at 400x240: two 64x32 sprites at the GBA screen's top-left whose
 * graphic is cut flat on the left (the GBA's edge hid it) and rounded on the right. Its left end is closed with the
 * right end mirrored: four sprites, one per tile row, showing the tile column(s) of the right end flipped, put after
 * the banner in OAM so the banner stays over the body pixels they share. Called before CopyOAM. */
static int BannerPixel(const struct OamData* sprite, int x, int y) {
    extern u8 gVram[0x18000];
    const u16* attr = (const u16*)sprite;
    const unsigned tile = (attr[2] & 0x3FFu) + (unsigned)(y >> 3) * 8u + (unsigned)(x >> 3); /* 1D, 64 wide */
    const u8 b = gVram[0x10000u + tile * 32u + (unsigned)(y & 7) * 4u + (unsigned)(x & 7) / 2u];
    return (x & 1) ? b >> 4 : b & 15;
}

void Port_Draw_PauseBannerCap(void) {
    if (!Port_Widescreen_PauseWide()) return; /* the game maps sprites 1D throughout */
    const int count = gOAMControls.updated < 0x80 ? gOAMControls.updated : 0x80;
    /* 64x32 (shape 1, size 3), not affine, on the GBA screen's top row: the leftmost is the banner's left half,
     * wherever a page change slides it, and the one 64 to its right its other half. */
    int left = -1, right = -1, leftX = 0;
    for (int i = 0; i < count; i++) {
        const u16* attr = (const u16*)&gOAMControls.oam[i];
        if ((attr[0] & 0xC300u) != 0x4000u || (attr[1] & 0xC000u) != 0xC000u || (attr[0] & 0xFFu) != 40) continue;
        const int x = (int)(attr[1] & 0x1FFu) >= 400 ? (int)(attr[1] & 0x1FFu) - 512 : (int)(attr[1] & 0x1FFu);
        if (left < 0 || x < leftX) left = i, leftX = x;
    }
    for (int i = 0; i < count && left >= 0; i++) {
        const u16* attr = (const u16*)&gOAMControls.oam[i];
        if ((attr[0] & 0xC300u) != 0x4000u || (attr[1] & 0xC000u) != 0xC000u || (attr[0] & 0xFFu) != 40) continue;
        if ((int)(attr[1] & 0x1FFu) == ((leftX + 64) & 0x1FF)) right = i;
    }
    if (left < 0 || count + 4 > 0x80) return;
    const u16* banner = (const u16*)&gOAMControls.oam[left];
    const int width = right >= 0 && (((const u16*)&gOAMControls.oam[right])[2] & 0xF000u) == (banner[2] & 0xF000u) ? 128 : 64;
    /* The banner's columns as patterns of colour indices, 32 rows each. */
    #define BANNER_PX(x, y) ((x) < 64 ? BannerPixel(&gOAMControls.oam[left], (x), (y)) : BannerPixel(&gOAMControls.oam[right], (x) - 64, (y)))
    /* The banner: the first run of columns with something in them (the second sprite may be another graphic, as on
     * the map page). */
    int first = -1, last = -1;
    for (int x = 0; x < width; x++) {
        int any = 0;
        for (int y = 0; y < 32 && !any; y++) any = BANNER_PX(x, y) != 0;
        if (any && first < 0) first = x;
        if (!any && first >= 0) break;
        if (any) last = x;
    }
    if (first < 0 || last - first < 24) return;
    /* Open on the left: its first column is the body's (the same as the next). */
    for (int y = 0; y < 32; y++)
        if (BANNER_PX(first, y) != BANNER_PX(first + 1, y)) return;
    /* The right end: the columns after the last one that still is the body's, compared on its outline only (the rows
     * not of the fill colour of its middle row: the text may run up to the end). */
    const int fill = BANNER_PX(first, 16);
    int end = last;
    while (end > first) {
        int body = 1;
        for (int y = 0; y < 32 && body; y++)
            if (BANNER_PX(first, y) != fill) body = BANNER_PX(end, y) == BANNER_PX(first, y);
        if (body) break;
        end--;
    }
    end++;
    if (last - end + 1 < 2 || last - end + 1 > 8) return;
    /* The tile column(s) holding it: one if it fits in the last, else two (16 wide). */
    const int tileW = (end >> 3) == (last >> 3) ? 8 : 16;
    const int c0 = (last >> 3) * 8 + 8 - tileW;
    const int capW = last - end + 1;
    /* Flipped, column c0 + i shows at sx + tileW - 1 - i; the outermost (last) lands capW left of the first. */
    const int sx = (banner[1] & 0x1FF) + first - capW - tileW + 1 + last - c0;
    const int owner = c0 < 64 ? left : right;
    const u16* src = (const u16*)&gOAMControls.oam[owner];
    const unsigned baseTile = (src[2] & 0x3FFu) + (unsigned)((c0 & 63) >> 3);
    #undef BANNER_PX
    u8* oam = (u8*)&gOAMControls.oam[0];
    int out = count;
    for (int row = 0; row < 4; row++) {
        u16 attr[3];
        attr[0] = (u16)((banner[0] & ~0xC0FFu) | (tileW == 16 ? 0x4000u : 0u) | (u16)((40 + row * 8) & 0xFF));
        attr[1] = (u16)(((banner[1] & ~0xF1FFu) | 0x1000u | ((u32)sx & 0x1FFu))); /* size 0: 8x8 or 16x8; h-flip */
        attr[2] = (u16)((src[2] & ~0x3FFu) | ((baseTile + (unsigned)row * 8u) & 0x3FFu));
        memcpy(oam + out * 8, attr, sizeof(attr));
        virtuappu_mode1_obj_y_negative_staged[out] = 0;
        virtuappu_mode1_obj_clip_mark_staged[out] = virtuappu_mode1_obj_clip_mark_staged[left];
        virtuappu_mode1_obj_depth_staged[out] = virtuappu_mode1_obj_depth_staged[left];
        virtuappu_mode1_obj_depth_top_staged[out] = virtuappu_mode1_obj_depth_top_staged[left];
        virtuappu_mode1_obj_group_staged[out] = virtuappu_mode1_obj_group_staged[left];
        out++;
    }
    gOAMControls.updated = (u8)out;
}

/* ---- ram_DrawDirect (port of arm_DrawDirect @ 0x080B280C) ----
 *
 * Signature: void ram_DrawDirect(OAMCommand* cmd, u32 spriteIndex, u32 frameIndex)
 *
 * Looks up sprite frame data from gFrameObjLists using self-relative
 * offset chains, then renders sprite pieces into gOAMControls.oam[].
 */
void ram_DrawDirect(OAMCommand* cmd, u32 spriteIndex, u32 frameIndex) {
    if (frameIndex == 0xFF)
        return;

    /* Look up frame data using self-relative offsets:
     * 1. gFrameObjLists[spriteIndex] → byte offset to frame table
     * 2. At that offset + frameIndex*4, read another u32 → byte offset to data
     * 3. base + that offset → frame data pointer
     */
    const u8* frameData = LookupFrameData((u16)spriteIndex, (u8)frameIndex);
    if (!frameData) {
        return;
    }

    u8 count = frameData[0];
    if (count == 0)
        return;
    if (gOAMControls.updated >= 0x80)
        return;

    /* Extract OAMCommand fields */
    s16 baseX = cmd->x;
    s16 baseY = cmd->y;
    u32 cmdFlags = 0;
    memcpy(&cmdFlags, &cmd->_4, sizeof(cmdFlags)); /* _4 | (_6 << 16) */
    u16 cmdExtra = cmd->_8;

    const int wide = WideSpritesBegin();
    RenderSpritePieces(frameData, baseX, baseY, cmdFlags, cmdExtra);
    WideSpritesEnd(wide);
}

/* ---- ram_sub_080ADA04 (port of arm_sub_080ADA04) ----
 *
 * Signature: void ram_sub_080ADA04(OAMCommand* cmd, void* frameData)
 *
 * Same as DrawDirect but takes a pre-resolved frame data pointer
 * instead of looking it up from gFrameObjLists.
 */
void ram_sub_080ADA04(OAMCommand* cmd, void* frameDataPtr) {
    const u8* frameData = (const u8*)frameDataPtr;
    if (frameData == NULL) /* Port_ResolveRomData can hand us NULL; the GBA ROM path never did */
        return;
    u8 count = frameData[0];
    if (count == 0)
        return;
    if (gOAMControls.updated >= 0x80)
        return;

    s16 baseX = cmd->x;
    s16 baseY = cmd->y;
    u32 cmdFlags = 0;
    memcpy(&cmdFlags, &cmd->_4, sizeof(cmdFlags));
    u16 cmdExtra = cmd->_8;

    const int wide = WideSpritesBegin();
    RenderSpritePieces(frameData, baseX, baseY, cmdFlags, cmdExtra);
    WideSpritesEnd(wide);
}

/* ====================================================================
 *  ENTITY DRAW LISTS — PC replacement for gUnk_081326EC IWRAM buffers
 * ==================================================================== */

/*
 * On GBA, gUnk_081326EC holds 5 IWRAM pointers to draw list buffers.
 * On PC those addresses are meaningless, so we allocate our own buffers.
 *
 * Layout of each list (matches GBA memory layout):
 *   byte 0:        count (max 64)
 *   bytes 1-3:     padding
 *   bytes 4..259:  up to 64 entity pointers (4 bytes each on GBA / ptr-sized on PC)
 */
typedef struct {
    u8 count;
    u8 _pad[3];
    Entity* entries[64];
} EntityDrawList;

/* 4 main draw lists + 1 deferred (shadow) list */
static EntityDrawList sDrawLists[5];

/* Reset all entity sprite draw lists. See forward declaration earlier
 * in this file for the rationale. */
static void ClearEntityDrawLists(void) {
    for (int i = 0; i < 4; i++) {
        sDrawLists[i].count = 0;
    }
}

/* Deferred draw entry (packed as on GBA: two shorts per entry) */
typedef struct {
    s16 packed0; /* (screenX << 6) | shadowType */
    s16 packed1; /* (screenY << 6) | priority   */
    u8 group;    /* port: the entity's group (sRenderingGroup), for its shadow's stereo support */
} DeferredEntry;

typedef struct {
    u8 count;
    u8 _pad[3];
    DeferredEntry entries[64];
} DeferredDrawList;

static DeferredDrawList sDeferredList;

/* ---- CheckOnScreen (port of ASM at 0x080040A8) ---- */
u32 CheckOnScreen(Entity* entity) {
    s32 x = (s32)entity->x.HALF.HI - (s32)gRoomControls.scroll_x;
    x += 0x3F;
    /* Runtime-gated widescreen: the right edge only widens while the WIP
     * option is enabled and the current room can fill the wider viewport. */
    if ((u32)x >= (u32)Port_Widescreen_GameplayViewWidth() + 0x7E)
        return 0;

    s32 y = (s32)entity->y.HALF.HI - (s32)gRoomControls.scroll_y;
    y += (s32)entity->z.HALF.HI;
    y += 0x3F;
    if ((u32)y >= (u32)Port_Widescreen_GameplayViewHeight() + 0x7E)
        return 0;

    return 1;
}

/* ---- DrawEntity (port of ASM at 0x0800404C) ----
 *
 * Called from ObjectUpdate / NpcUpdate / EnemyUpdate / etc.
 * Checks visibility, then adds the entity to the appropriate draw list.
 */
extern u8 gUnk_02024048; /* pending sound count */
extern u16 gUnk_02021F20[8];

void DrawEntity(Entity* entity) {
    u8 draw = *(u8*)&entity->spriteSettings & 3; /* bits 0-1 */
    if (draw == 0) {
        gUnk_02024048 = 0;
        return;
    }

    if (draw != 3) {
        /* sub_080040A2: check bit 1 of spriteSettings byte */
        u8 rawSS = *(u8*)&entity->spriteSettings;
        if (!(rawSS & 2)) {
            /* Not "draw always" — check on-screen */
            if (!CheckOnScreen(entity)) {
                gUnk_02024048 = 0;
                return;
            }
        }
        /* else bit 1 set (draw==2 or draw with ss2), treat as on-screen */
    }

    /* Determine draw list index from spriteRendering.b3 (bits 6-7 of byte 0x19) */
    u8 rawSR = *(u8*)&entity->spriteRendering;
    u32 listIdx = (rawSR & 0xC0) >> 6;
    EntityDrawList* list = &sDrawLists[listIdx];

    if (list->count < 64) {
        list->entries[list->count] = entity;
        list->count++;
    }

    /* Flush pending SFX queue (matches the original DrawEntity behavior). */
    {
        u8 pending = gUnk_02024048;
        gUnk_02024048 = 0;
        while (pending != 0) {
            pending--;
            SoundReq(gUnk_02021F20[pending]);
        }
    }
}

/* ---- ResolveOamDrawPriority (port of ASM at 0x080B2478) ----
 *
 * Sorts entity pointers in a draw list by (y-position + priority).
 * Uses shell sort with halving gap sequence, matching the original
 * ARM implementation.
 */
static void ResolveOamDrawPriority(EntityDrawList* list) {
    u32 n = list->count;
    if (n <= 1)
        return;

    u32 gap = n - 1;
    while (gap > 0) {
        for (u32 i = 0; i < n - gap; /* empty */) {
            u32 j = i;
            while (1) {
                u32 k = j + gap;
                if (k >= n)
                    break;
                Entity* a = list->entries[j];
                Entity* b = list->entries[k];
                /* Priority key from ASM:
                 * pos = (entity->y.WORD + 0x80000000) >> 3
                 * prio = ~entity->spritePriority.raw (inverted so 0 = highest)
                 * key = pos | (prio << 29)
                 * Higher key = draw LATER (lower draw priority).
                 * Swap if a's key < b's key (a should draw after b).
                 */
                u32 keyA = ((u32)(a->y.WORD + 0x80000000U) >> 3) | ((u32)(~(*(u8*)&a->spritePriority)) << 29);
                u32 keyB = ((u32)(b->y.WORD + 0x80000000U) >> 3) | ((u32)(~(*(u8*)&b->spritePriority)) << 29);
                if (keyA >= keyB)
                    break; /* a has equal or higher key — correct order */
                /* Shift b down, insert a at k */
                list->entries[k] = a;
                list->entries[j] = b;
                if (j < gap)
                    break;
                j -= gap;
            }
            i++;
        }
        gap >>= 1;
    }
}

/* ---- ResolveEntitySpriteParams (port of sub_080B299C) ----
 *
 * Computes drawing parameters from entity fields:
 *   screenX, screenY, flags (attr0/attr1 bits), extra (attr2 bits)
 */
static void ResolveEntitySpriteParams(Entity* entity, s32* outX, s32* outY, u32* outFlags, u16* outExtra) {
    /* Iframes palette override */
    u8 iframesPal = 0;
    if (entity->iframes > 0) {
        iframesPal = ((u8*)&gOAMControls)[0x0E]; /* gOAMControls._0[6] */
    }

    /* Load 32-bit word at offset 0x18: spriteSettings|spriteRendering|palette|spriteOrientation */
    u32 ip = 0;
    memcpy(&ip, &entity->spriteSettings, sizeof(ip));

    /* Build attr2 base (extra): tile + priority + palette */
    u16 sb = entity->spriteVramOffset;
    u8 palRaw = entity->palette.raw;
    palRaw |= iframesPal;
    palRaw &= 0x0F;
    sb |= (u16)palRaw << 12;

    u8 orientRaw = *(u8*)&entity->spriteOrientation;
    sb |= (u16)(orientRaw & 0xC0) << 4; /* priority bits → attr2 bits 10-11 */

    /* Screen position */
    s32 x = (s32)entity->x.HALF.HI + (s32)(s8)entity->spriteOffsetX;
    s32 y = (s32)entity->y.HALF.HI + (s32)entity->z.HALF.HI + (s32)entity->spriteOffsetY;

    /* Scroll subtraction (skip for draw mode 2) */
    if ((ip & 3) != 2) {
        x -= (s16)gOAMControls._4;
        y -= (s16)gOAMControls._6;
    }

    /* Build flags word */
    u32 r8 = ip & 0x3E003F00U;
    r8 |= (u32)gOAMControls.spritesOffset << 12;

    u8 frameSS = entity->frameSpriteSettings;
    u8 ssRaw = *(u8*)&entity->spriteSettings;
    u8 flipBits = (frameSS ^ ssRaw) & 0xC0;
    r8 |= (u32)flipBits << 22; /* bits 28-29 */

    *outX = x;
    *outY = y;
    *outFlags = r8;
    *outExtra = sb;
}

/* ---- LookupAndRenderNormal (port of _080B27E4 + sub_080B27F4) ---- */

static void LookupAndRenderNormal(Entity* entity, s32 x, s32 y, u32 flags, u16 extra) {
    u8 frameIdx = entity->frameIndex;
    if (frameIdx == 0xFF)
        return;

    u16 sprIdx = (u16)entity->spriteIndex;
    const u8* frameData = LookupFrameData(sprIdx, frameIdx);
    if (!frameData) {
        return;
    }

    RenderSpritePieces(frameData, (s16)x, (s16)y, flags, extra);
}

/* ---- DrawEntitySprites (port of _080B2718 + sub-paths) ----
 *
 * Handles the three entity rendering modes:
 *   spriteAnimation[2] == 0: normal sprite via gFrameObjLists
 *   spriteAnimation[2] < 0:  direct frame data from entity->myHeap
 *   spriteAnimation[2] > 0:  multi-part from gUnk_020000C0
 */
/* What stands out in stereo 3D (owner, 2026-10-20): the treasure Link shows when he gets one (LinkHoldingItem), 4 px,
 * and the object he lifts to throw (the carried entity's child while heldObject is 3, lifting, or 4, holding;
 * UpdateCarriedObject) with a depth that follows its height over the floor: 4 px over his head, less as it rises
 * and, once thrown, as it falls, 0 when it lands and breaks. Its height over his head is learnt while he holds it. */
enum { HELD_DEPTH_Q = 16 /* 4 px */ };
static const Entity* sCarried;
static const Entity* sThrown;
static u8 sThrownKind, sThrownId;
static s32 sHoldHeight = 16;

static u8 DepthForHeight(const Entity* entity) {
    const s32 height = -(s32)entity->z.HALF.HI;
    if (height <= 0) return 0;
    const s32 q = height * HELD_DEPTH_Q / sHoldHeight;
    return (u8)(q > HELD_DEPTH_Q ? HELD_DEPTH_Q : q);
}

static u8 SpriteDepth(const Entity* entity) {
    extern PlayerEntity gPlayerEntity;
    if (entity->kind == OBJECT && entity->id == LINK_HOLDING_ITEM) return HELD_DEPTH_Q;
    const Entity* carried = (u8)(gPlayerState.heldObject - 3) < 2 && gPlayerEntity.carriedEntity != NULL
                                ? gPlayerEntity.carriedEntity->child
                                : NULL;
    if (carried != NULL) {
        sCarried = carried;
        sThrown = NULL;
        if (gPlayerState.heldObject == 4 && -(s32)carried->z.HALF.HI > 4) sHoldHeight = -(s32)carried->z.HALF.HI;
    } else if (sCarried != NULL) {
        /* Let go of: thrown (or dropped). Followed while it is in the air. */
        sThrown = sCarried;
        sThrownKind = sCarried->kind;
        sThrownId = sCarried->id;
        sCarried = NULL;
    }
    if (entity == carried) return DepthForHeight(entity);
    if (entity == sThrown) {
        /* The slot can be another entity's by now; a landed one is on the floor for good. */
        if (entity->kind != sThrownKind || entity->id != sThrownId || entity->z.HALF.HI >= 0) {
            sThrown = NULL;
            return 0;
        }
        return DepthForHeight(entity);
    }
#ifdef TMC_3DS
    /* A Keese flies without ever leaving z 0 (type 0: it fakes the height with spriteOffsetY) or sits at z -16 all the
     * time (the other types), resting and sleeping included. Its flight is its action (1 = KEESE_ACTION_FLY): in the air
     * it stands out as an entity 16 px up, at rest and asleep it is on the ground's plane. */
    if (entity->kind == ENEMY && entity->id == KEESE) {
        extern int Port_Config_StereoHeightDepthQ(int height);
        return entity->action == 1 ? (u8)Port_Config_StereoHeightDepthQ(16) : 0;
    }
    /* Standing on the ground (z 0) a sprite stays on the ground's plane, so it does not float; raised off it
     * (jumping, falling, flying) it stands out with its height (stereo_world_height in tmc3ds.ini). */
    extern int Port_Config_StereoHeightDepthQ(int height);
    return (u8)Port_Config_StereoHeightDepthQ(-(s32)entity->z.HALF.HI);
#else
    return 0;
#endif
}

static void DrawEntitySpritesOf(Entity* entity, s32 x, s32 y, u32 flags, u16 extra);

static void DrawEntitySprites(Entity* entity, s32 x, s32 y, u32 flags, u16 extra) {
    sRenderingDepth = SpriteDepth(entity);
    sRenderingBody = 1;
    sRenderingBoss = entity->kind == ENEMY && entity->id == CHUCHU_BOSS;
#ifdef TMC_3DS
    {
        const int mode = SpriteDepthMode(entity);
        sRenderingInvert = mode == PORT_DEPTH_INVERT;
        sRenderingFlat = mode == PORT_DEPTH_FLAT;
        sRenderingSide = mode == PORT_DEPTH_LEFT || mode == PORT_DEPTH_RIGHT ? mode : 0;
        sRenderingRaised = mode == PORT_DEPTH_RAISED;
        sRenderingFixedQ = (mode & PORT_DEPTH_FIXED_BASE) != 0 ? (mode & 0x3f) : -1;
    }
#endif
    /* Its jump (1 px at most) must not use up the room the rise from feet to head needs under the world's cap: with the
     * whole of it, every part reached the cap at the top of a jump and the ramp was lost. */
    if (sRenderingBoss && sRenderingDepth > 4) sRenderingDepth = 4;
    static u8 sNextGroup = 0;
    sNextGroup = SpriteSupport3DS_NextGroup(sNextGroup);
    sRenderingGroup = sLastGroup = sNextGroup;
    DrawEntitySpritesOf(entity, x, y, flags, extra);
    sRenderingGroup = 0;
    sRenderingBody = 0;
    sRenderingBoss = 0;
#ifdef TMC_3DS
    sRenderingInvert = sRenderingFlat = sRenderingSide = sRenderingRaised = 0;
    sRenderingFixedQ = -1;
#endif
    sRenderingDepth = 0; /* what is drawn next (the HUD, DrawDirect) stays on the screen plane */
}

#ifdef TMC_3DS
/* An object drawn as several entities at one spot leans as one, like the parts of one entity (sJoinMeasure): the giant
 * twig of the Crenel paths (area 0x12) is three (types 0, 0x40, 0x41), and the middle one, leant from its own feet 8 px
 * above the others', stood at another depth and moved a pixel apart from them. Each of them measures the pieces of all
 * of them (at their own screen positions) and leans from that box. */
static int JoinSiblingEntities(Entity* entity, s32 x, s32 y) {
    if (entity->kind != OBJECT || entity->id != GIANT_TWIG) return 0;
    sJoinFeet = -0x7fffffff;
    sJoinTop = 0x7fffffff;
    sJoinLeft = 0x7fffffff;
    sJoinRight = -0x7fffffff;
    sJoinMeasure = 1;
    for (LinkedList* list = gEntityLists; list < &gEntityLists[9]; ++list) {
        if (list->first == NULL) continue;
        for (Entity* e = list->first; e != (Entity*)list && e != NULL; e = e->next) {
            if (e->kind != entity->kind || e->id != entity->id || e->x.HALF.HI != entity->x.HALF.HI ||
                e->y.HALF.HI != entity->y.HALF.HI || (s8)e->spriteAnimation[2] != 0)
                continue;
            s32 sx, sy;
            u32 sf;
            u16 se;
            ResolveEntitySpriteParams(e, &sx, &sy, &sf, &se);
            LookupAndRenderNormal(e, sx, sy, sf, se);
        }
    }
    sJoinMeasure = 0;
    sJoinKnown = sJoinFeet > -0x7fffffff;
    if (sJoinKnown) {
        /* Where this one is drawn, against where it was measured (the caller's position may carry offsets). */
        s32 ox, oy;
        u32 of;
        u16 oe;
        ResolveEntitySpriteParams(entity, &ox, &oy, &of, &oe);
        sJoinFeet += y - oy;
        sJoinTop += y - oy;
        sJoinLeft += x - ox;
        sJoinRight += x - ox;
    }
    return sJoinKnown;
}
#endif

static void DrawEntitySpritesOf(Entity* entity, s32 x, s32 y, u32 flags, u16 extra) {
    s8 renderMode = (s8)entity->spriteAnimation[2]; /* offset 0x28 */

    extern PlayerEntity gPlayerEntity;
    /* Set for ALL render paths (the player uses the multi-part path renderMode==1),
     * so the swamp-sink OAM marking below covers Link's composite sprite. */
    sRenderingPlayer = (entity == &gPlayerEntity.base);

    if (renderMode == 0) {
        /* Normal sprite rendering */
#ifdef TMC_3DS
        if (sRenderingBody && !sRenderingFlat && JoinSiblingEntities(entity, x, y)) {
            LookupAndRenderNormal(entity, x, y, flags, extra);
            sJoinKnown = 0;
            return;
        }
#endif
        LookupAndRenderNormal(entity, x, y, flags, extra);
    } else if (renderMode < 0) {
        /* Direct frame data from myHeap */
        const u8* frameData = (const u8*)entity->myHeap;
        if (frameData) {
            RenderSpritePieces(frameData, (s16)x, (s16)y, flags, extra);
        }
    } else {
        /* Multi-part from gUnk_020000C0 */
        struct_gUnk_020000C0* slot = &gUnk_020000C0[(u8)renderMode];
        u16 cleanExtra = extra & 0x0FFF; /* clear palette bits */

        /* The parts of one entity (a carpenter's body, head and tool) lean as one: measured first, over all of them, then
         * each leans from the whole box instead of from its own feet (the head started again at 0). Not Link (tuned by
         * hand) nor the big Chuchu (its own ground, below). */
#ifdef TMC_3DS
        const int join = sRenderingBody && !sRenderingFlat && !sRenderingPlayer && !sRenderingBoss;
#else
        const int join = 0;
#endif
        for (int pass = join ? 0 : 1; pass < 2; ++pass) {
#ifdef TMC_3DS
            if (pass == 0) {
                sJoinFeet = -0x7fffffff;
                sJoinTop = 0x7fffffff;
                sJoinLeft = 0x7fffffff;
                sJoinRight = -0x7fffffff;
                sJoinMeasure = 1;
            } else if (join) {
                sJoinMeasure = 0;
                sJoinKnown = sJoinFeet > -0x7fffffff;
            }
#endif
            for (int part = 0; part < 4; part++) {
                struct_gUnk_020000C0_1* sub = &slot->unk_00[part];
                if (!(*(u8*)&sub->unk_00 & 1)) /* not active */
                    break;

                if (*(u8*)&sub->unk_00 & 2) {
                    /* Sub-entity mode — entity pointer at offset 0x0C */
                    Entity* subEntity = (Entity*)sub->unk_0C;
                    if (subEntity) {
                        s32 sx, sy;
                        u32 sf;
                        u16 se;
                        ResolveEntitySpriteParams(subEntity, &sx, &sy, &sf, &se);
                        u8 extraYOff = ((u8*)&gOAMControls)[0x12]; /* fp[0x12] */
                        sy += extraYOff;
                        LookupAndRenderNormal(subEntity, sx, sy, sf, se);
                    }
                } else {
                    /* Direct sub-sprite */
                    u8 subSprSlot = sub->unk_01;
                    if (subSprSlot == 0xFF)
                        continue; /* skip to next part (don't return!) */

                    /* Apply sub-part overrides */
                    u32 partFlags = flags;
                    s32 partX = x;
                    s32 partY = y;
                    u16 partExtra = cleanExtra;

                    u8 flipOverride = sub->unk_04.BYTES.byte0; /* GBA: LDRB r2, [r4, #4] */
                    partFlags ^= (u32)flipOverride << 28;

                    s8 xOff = (s8)((sub->unk_04.WORD >> 16) & 0xFF); /* byte at +6 */
                    s8 yOff = (s8)((sub->unk_04.WORD >> 24) & 0xFF); /* byte at +7 */

                    if (!(partFlags & 0x10000000))
                        partX += xOff;
                    else
                        partX -= xOff;
                    if (!(partFlags & 0x20000000))
                        partY += yOff;
                    else
                        partY += yOff; /* Note: GBA uses ADD for both vflip cases */

                    u8 palBits = (u8)((sub->unk_04.WORD >> 8) & 0xFF); /* byte at +5 */
                    partExtra |= (u16)palBits << 12;

                    u8 tileOff = (u8)(sub->unk_08.WORD & 0xFF); /* byte at +8 */
                    partExtra += tileOff;

                    u16 sprIdx = sub->unk_02 & 0xFFFF;

                    /* Look up and render — GBA uses sub-part's own frame index
                     * (r0 = sub->unk_01, loaded at 080B2754), NOT entity->frameIndex */
                    if (subSprSlot != 0xFF) {
                        const u8* frameData = LookupFrameData(sprIdx, subSprSlot);
                        if (frameData) {
#ifdef TMC_3DS
                            const int savedFlat = sRenderingFlat;
                            if (PartIsFlat(entity, part))
                                sRenderingFlat = 1;
#endif
                            RenderSpritePieces(frameData, (s16)partX, (s16)partY, partFlags, partExtra);
#ifdef TMC_3DS
                            sRenderingFlat = savedFlat;
#endif
                        }
                    }
                }
            }
        }
#ifdef TMC_3DS
        sJoinMeasure = sJoinKnown = 0;
#endif
    }
}

/* ---- ProcessEntityForDraw (port of sub_080B255C, USA path) ----
 *
 * Handles one entity: resolves params, checks shadow flags, renders.
 */
static void ProcessEntityForDrawNative(Entity* entity);

static void ProcessEntityForDraw(Entity* entity) {
    const int wide = WideSpritesBegin();
    ProcessEntityForDrawNative(entity);
    WideSpritesEnd(wide);
}

static void ProcessEntityForDrawNative(Entity* entity) {
    s32 x, y;
    u32 flags;
    u16 extra;

    ResolveEntitySpriteParams(entity, &x, &y, &flags, &extra);

    /* Check shadow flag (bit 3 of spritePriority byte, offset 0x29) */
    s8 prioRaw = *(s8*)&entity->spritePriority;
    if (!(prioRaw & 8)) {
        /* No shadow — just draw the entity sprites */
        DrawEntitySprites(entity, x, y, flags, extra);
        return;
    }

    /* Has shadow flag — check z for shadow rendering */
    s16 zVal = entity->z.HALF.HI;
    if (zVal < 0) {
        /* z < 0: skip shadow sprite, just draw entity */
        DrawEntitySprites(entity, x, y, flags, extra);
    } else {
        /* z >= 0: GBA renders a "shoes overlay" sprite over the entity's
         * feet when standing on shallow water (act_tile 0x0F) or tall
         * grass (act_tile 0x2F) — covers Link's lower body so he visually
         * "wades" through the surface (#24). Use ram_0x80b2b58 frame data
         * indexed by spriteSettings shadow bits + an animation frame.
         * For other act_tiles the overlay is skipped (just regular draw). */
        if (!sShoesOverlayTableLoaded)
            LoadShoesOverlayTableFromRom();

        extern u32 GetActTileAtTilePos(u16 tilePos, u8 layer);
        s32 ex = (s32)entity->x.HALF.HI - (s32)gRoomControls.origin_x;
        s32 ey = (s32)entity->y.HALF.HI - (s32)gRoomControls.origin_y;
        u32 tilePos = ((u32)ex & 0x3F0u) | ((((u32)ey & 0x3F0u)) << 6);
        u32 actTile = GetActTileAtTilePos((u16)(tilePos >> 4), entity->collisionLayer);

        if (actTile == 0x0Fu || actTile == 0x2Fu) {
            u8 ssRaw = *(u8*)&entity->spriteSettings;
            /* GBA uses (ss & 0x30) directly as a BYTE offset into the pointer
             * table (add r2, r1, r2, lsl #1 ; ldr [table, r2]), NOT (ss>>2). */
            u32 ssBits = (u32)ssRaw & 0x30u;
            u8 frame;
            s32 overlayY = y;
            if (actTile == 0x0Fu) {
                /* Shallow water — animate on global frame counter (gOAMControls
                 * field_0x1 holds an animation tick on GBA). Bias overlay 2px
                 * lower like GBA does. */
                u8 fld1 = ((u8*)&gOAMControls)[1];
                frame = (u8)(((fld1 & 0x18u) + 0x80u) >> 2);
                overlayY += 2;
            } else {
                /* Tall grass — position-derived "random" frame so neighbouring
                 * patches don't all wave in sync. Read the INTEGER pixel-position
                 * bytes x.HALF.HI ^ y.HALF.HI (entity[0x2e] ^ entity[0x32],
                 * asm/src/intr.s:1390). The previous code read the FRACTIONAL
                 * bytes (x.HALF.LO>>8 = entity[0x2d]), so the frame jittered with
                 * sub-pixel motion instead of being stable per tile. */
                u8 xb = (u8)entity->x.HALF.HI;
                u8 yb = (u8)entity->y.HALF.HI;
                frame = (u8)((xb ^ yb) & 6u);
            }
            /* ldr table[(ss&0x30) + (frame<<1)] is a byte offset into a
             * 32-pointer table (asm/src/intr.s:1392-1395); pointer index is
             * that >>2. Water frames (32..38) map to indices 16..31 — the old
             * 16-entry table with (row>>2 + frame<<1) put them out of range, so
             * the water overlay never drew. */
            u32 idx = (ssBits + ((u32)frame << 1)) >> 2;
            if (idx < 32u && sShoesOverlayPtrs[idx] != NULL) {
                u16 overlayExtra = (u16)(extra & 0x0C00u); /* keep priority bits only */
                RenderSpritePieces(sShoesOverlayPtrs[idx], (s16)x, (s16)overlayY, 0, overlayExtra);
            }
        }
        DrawEntitySprites(entity, x, y, flags, extra);
    }

    /* Deferred list handling — add shadow/underlay entry */
    u8 prioByte = *(u8*)&entity->spritePriority;
    if (prioByte & 0x10) {
        if (zVal >= 0)
            return; /* skip */
    }
    if (prioByte & 0x20) {
        if (gOAMControls.field_0x1 & 1)
            return;
    }

    s32 deferY = y;
    if (zVal < 0)
        deferY -= zVal;

    if (sDeferredList.count >= 64)
        return;
    sDeferredList.count++;
    DeferredEntry* de = &sDeferredList.entries[sDeferredList.count - 1];
    u16 prioBits = (extra >> 10) & 3;
    de->packed1 = (s16)(u16)(((u32)(u16)deferY << 6) | (u32)prioBits);
    u8 shadowType = (*(u8*)&entity->spriteSettings & 0x30) >> 4;
    de->packed0 = (s16)(u16)(((u32)(u16)x << 6) | (u32)shadowType);
    de->group = sLastGroup;
}

/* ---- ProcessDrawList (port of sub_080B2534) ---- */
static void ProcessDrawList(EntityDrawList* list) {
    for (u32 i = 0; i < list->count; i++) {
        if (gOAMControls.updated >= 0x80)
            return;
        ProcessEntityForDraw(list->entries[i]);
    }
}

/* ---- ProcessDeferredList (port of _080B26B4) ----
 *
 * Renders shadow/underlay sprites from the deferred list.
 * Uses ram_0x80b2bd8 (shadow frame data table) which needs
 * to be loaded from ROM overlay data.
 */
/* GBA-original keeps a 4-pointer table at ROM 0x080B2BD8 that maps each
 * shadow type (small / medium / large / special) to sprite-frame-data
 * blobs. The pointers in the table are IWRAM addresses (0x0300xxxx) into
 * the runtime-copied overlay region. The PC port skips the IWRAM overlay
 * copy (see InitOverlays), so those IWRAM addresses point at
 * uninitialized memory — shadows silently disappeared (issue #10).
 *
 * Translate IWRAM ↔ ROM via the linker-derived delta: a known anchor is
 * `ram_sub_080B2248` at IWRAM 0x03005FBC, ROM 0x080B2248
 * → delta = 0x080B2248 - 0x03005FBC = 0x050AC28C.
 *
 * This delta is region-specific (works for USA; EU/JP may differ). */
static const u8* sShadowFramePtrs[4] = { NULL, NULL, NULL, NULL };
static int sShadowTableLoaded = 0;

/* Companion table at ROM 0x080B2B58 (32 IWRAM-relative pointers, same
 * relocation delta) — sprite-frame data for the "shoes overlay" the GBA
 * draws over Link's feet when he steps onto shallow water (act-tile 0x0F)
 * or tall grass (act-tile 0x2F). Without it, Link's shoes don't get
 * obscured by water/grass and the effect is invisible (#24).
 *
 * The GBA indexes it as a BYTE-offset pointer table:
 *   ptr = table[(spriteSettings & 0x30) + (frame << 1)]   (then >>2 for index)
 * Grass frames (0..6) land in indices 0..15; water frames (32..38) land in
 * indices 16..31 — which is why the table needs all 32 entries (a 16-entry
 * table dropped the water overlay entirely). */
/* Definitions for the forward-declared shoes-overlay table above. */
static const u8* sShoesOverlayPtrs[32] = { NULL };
static int sShoesOverlayTableLoaded = 0;

static const u8* TranslateIwramOrRomPointer(u32 ptr) {
    /* IWRAM-resident frame data (shadow/shoes tables) is DMA'd from ROM at boot;
     * map the IWRAM address back to its ROM source (the .iwram section LMA - VMA
     * delta). The delta is region-specific — using the USA delta against an EU/JP
     * ROM reads garbage frame data, so the shadow pass renders ~90-180-piece junk
     * sprites that flood OAM. Deltas derived + verified per region (each resolves
     * the shadow/shoes tables to valid small piece counts). */
    if (ptr >= 0x03000000u && ptr < 0x03008000u) {
        const u32 delta = RegionRomOffset(0x050AC28Cu, 0x050AB7ECu, 0x050AC02Cu);
        u32 romFull = ptr + delta;
        if (romFull >= 0x08000000u && romFull < 0x08000000u + gRomSize) {
            return gRomData + (romFull - 0x08000000u);
        }
    } else if (ptr >= 0x08000000u && ptr < 0x08000000u + gRomSize) {
        return gRomData + (ptr - 0x08000000u);
    }
    return NULL;
}

static u32 ReadRomU32LE(u32 offset) {
    return (u32)gRomData[offset] | ((u32)gRomData[offset + 1] << 8) | ((u32)gRomData[offset + 2] << 16) |
           ((u32)gRomData[offset + 3] << 24);
}

/*
 * Region-select a ROM offset by the loaded ROM's game code (@0xAC). The shadow
 * and shoes-overlay frame-pointer tables live in an unnamed data block whose
 * address differs per region; using the USA offset against an EU/JP ROM reads
 * garbage ROM pointers, so DrawEntitySprites' shadow pass renders ~181-piece
 * garbage sprites that flood OAM (the whole screen fills with junk). Offsets
 * derived + byte-verified from the retail maps (the resolved entries are the
 * same IWRAM frame pointers in every region). */
static u32 RegionRomOffset(u32 usa, u32 eu, u32 jp) {
    if (gRomData != NULL && gRomSize >= 0xB0 && gRomData[0xAC] == 'B' && gRomData[0xAD] == 'Z' &&
        gRomData[0xAE] == 'M') {
        if (gRomData[0xAF] == 'P')
            return eu;
        if (gRomData[0xAF] == 'J')
            return jp;
    }
    return usa;
}

static void LoadShadowTableFromRom(void) {
    const u32 kShadowTableRomOffset = RegionRomOffset(0xB2BD8u, 0xB2300u, 0xB2978u);

    if (sShadowTableLoaded || gRomData == NULL || gRomSize <= kShadowTableRomOffset + 16u) {
        sShadowTableLoaded = 1;
        return;
    }
    for (int i = 0; i < 4; i++) {
        sShadowFramePtrs[i] = TranslateIwramOrRomPointer(ReadRomU32LE(kShadowTableRomOffset + i * 4));
    }
    sShadowTableLoaded = 1;
}

static void LoadShoesOverlayTableFromRom(void) {
    const u32 kShoesTableRomOffset = RegionRomOffset(0xB2B58u, 0xB2280u, 0xB28F8u);
    if (sShoesOverlayTableLoaded || gRomData == NULL || gRomSize <= kShoesTableRomOffset + 128u) {
        sShoesOverlayTableLoaded = 1;
        return;
    }
    for (int i = 0; i < 32; i++) {
        sShoesOverlayPtrs[i] = TranslateIwramOrRomPointer(ReadRomU32LE(kShoesTableRomOffset + i * 4));
    }
    sShoesOverlayTableLoaded = 1;
}

static void ProcessDeferredList(void) {
    if (sDeferredList.count == 0)
        return;
    if (!sShadowTableLoaded)
        LoadShadowTableFromRom();

    for (u32 i = 0; i < sDeferredList.count; i++) {
        if (gOAMControls.updated >= 0x80)
            return;
        DeferredEntry* de = &sDeferredList.entries[i];
        s32 screenX = de->packed0 >> 6;
        u32 listType = de->packed0 & 7;
        s32 screenY = de->packed1 >> 6;
        u32 prioBits = de->packed1 & 3;
        u16 extra = (u16)(prioBits << 10);

        if (listType >= 4) {
            continue;
        }

        const u8* frameData = sShadowFramePtrs[listType];
        if (frameData == NULL)
            continue;
        const int wide = WideSpritesBegin();
        /* On the surface under it (its own support, at most its entity's), never at the entity's height:
         * sRenderingDepth stays 0. */
        sRenderingGroup = de->group != 0 ? (u8)(de->group | SPRITE_SUPPORT_3DS_SHADOW) : 0;
        RenderSpritePieces(frameData, (s16)screenX, (s16)screenY, 0, extra);
        sRenderingGroup = 0;
        WideSpritesEnd(wide);
    }
}

/* ---- ram_DrawEntities (port of arm_DrawEntities @ 0x080B23F0) ----
 *
 * Main entry point: iterates the 4 entity draw lists, sorts by
 * priority, renders each entity's sprites into gOAMControls.oam[].
 */

void ram_DrawEntities(void) {
    if (gOAMControls.updated >= 0x80)
        return;

    for (int i = 0; i < 4; i++) {
        EntityDrawList* list = &sDrawLists[i];
        if (list->count == 0)
            continue;

        /* Clear deferred list for this batch */
        sDeferredList.count = 0;

        /* Sort entities by y + priority */
        ResolveOamDrawPriority(list);

        /* Process each entity */
        ProcessDrawList(list);

        /* Render deferred shadow/underlay sprites */
        ProcessDeferredList();

        /* NOTE: do NOT clear list->count here. The list is cleared at
         * the start of ram_UpdateEntities (mode=0) so registrations
         * persist into ChangeArea/Init frames, where DrawEntities
         * re-renders the same entities at their last positions. See
         * comment in ram_UpdateEntities. */
    }
}

/* ---- Second-screen raw-data accessors (port_second_screen_dungeonmap.c) ----
 * Read-only faces over this file's private sprite plumbing so the second
 * screen can redraw the game's own DrawDirect marker frames without touching
 * OAM or any live state. Appended at the end of the file, and kept
 * append-only, so hand-merging stays a non-event. */

/* Frame piece list for a DrawDirect-style sprite (count byte + 5-byte
 * pieces; format documented at RenderSpritePieces above), or NULL.
 * outMaxPieces is how many whole pieces actually fit inside gFrameObjLists
 * after the count byte — the same clamp RenderSpritePieces applies, exported
 * so out-of-file renderers can't read past the array on a corrupt or
 * truncated ROM. */
const u8* Port_GetDirectSpriteFrame(u32 spriteIndex, u32 frameIndex, u32* outMaxPieces) {
    const u8* frameData;

    if (outMaxPieces != NULL) {
        *outMaxPieces = 0;
    }
    if (spriteIndex > 0xFFFF || frameIndex > 0xFF) {
        return NULL;
    }
    frameData = LookupFrameData((u16)spriteIndex, (u8)frameIndex);
    if (frameData != NULL && outMaxPieces != NULL) {
        const u8* fobEnd =
            (const u8*)gFrameObjLists + Port_FrameObjListsSizeForRegion(gRomRegion);
        *outMaxPieces = (u32)((fobEnd - (frameData + 1)) / 5);
    }
    return frameData;
}

/* The ROM overlay's OAM size/anchor table (60 x {xAnchor, yAnchor, width,
 * height} bytes; sub-tables per flip/affine mode — see sSizeTable at the top
 * of this file). NULL until the ROM has been loaded. */
const u8* Port_GetSpriteSizeTable(void) {
    return sSizeTableLoaded ? sSizeTable : NULL;
}
