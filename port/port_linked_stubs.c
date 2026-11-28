
#include "area.h"
#include "backgroundAnimations.h"
#include "beanstalkSubtask.h"
#include "collision.h"
#include "color.h"
#include "common.h"
#include "port_rng.h"
#include "entity.h"
#include "enemy.h"
#include "fade.h"
#include "game.h"

#define gMapDataBottomSpecial gMapDataBottomSpecial_HIDDEN
#include "fileselect.h"
#undef gMapDataBottomSpecial
#include "hitbox.h"
#include "kinstone.h"
#include "main.h"
#include "map.h"
#include "manager/diggingCaveEntranceManager.h"
#include "manager/lightRayManager.h"
#include "manager/weatherChangeManager.h"
#include "manager/horizontalMinishPathBackgroundManager.h"
#include "manager/verticalMinishPathBackgroundManager.h"
#include "menu.h"
#include "message.h"
#include "npc.h"
#include "physics.h"
#include "player.h"
#include "room.h"
#include "save.h"
#include "screen.h"
#include "script.h"
#include "sound.h"
#include "structures.h"
#include "subtask.h"

#include "port_entity_ctx.h"
#include "port_collision_fidelity.h"
#include "port_gba_mem.h"
#include "port_config.h"
#include "port_rom.h"
#include "port_runtime_config.h"
#ifdef TMC_3DS
#include "port_zone_tiles.h"
#ifdef TMC_3DS
#include "port_neighbour_rooms.h"
#endif
#endif
#include "port_widescreen.h"
#include "port_pause_3ds.h"
#include "port_hdma.h"
#include "port_widescreen_banner_state.h"
#include "port_horizontal_minish_path.h"

#include <string.h>

extern u32 CalculateDirectionTo(u32, u32, u32, u32);
extern void UpdateScreenShake(void);

uint16_t gPortIntrCheck;
void* gPortIntrVector;
struct SoundInfo* gPortSoundInfoPtr;

// Data globals
GfxSlotList gGFXSlots;
Message gMessage;
TextRender gTextRender;
u16 gPaletteBuffer[0x200];
Input gInput;
u32 gRand;
Screen gScreen;
OAMCommand gOamCmd;
Main gMain;
FadeControl gFadeControl;
OAMControls gOAMControls;
SoundPlayingInfo gSoundPlayingInfo;

u32 gUsedPalettes;

// Pointers
struct_02000010 gUnk_02000010;
u8 gUnk_02000030[0x10]; /* EWRAM marker, 16 bytes gap */
struct_02000040 gUnk_02000040;
void* gUnk_020000B0 = NULL; /* Entity* pointer (8 bytes on 64-bit) */
struct_gUnk_020000C0 gUnk_020000C0[0x30];
/* gUnk_02001A3C aliases gPaletteList[15] in src/color.c. */
u8 gUnk_02006F00[0x4000] __attribute__((aligned(4)));                    /* BG tilemap buffer (16 KB) */
u16 gUnk_0200B640;                                                       /* scroll state scalar */
u16 gUnk_02017830[0x138] __attribute__((aligned(4)));                    /* palette rotation buffer (624 bytes) */
u16 gUnk_02017AA0[0xA00] __attribute__((aligned(4)));                    /* HBlank DMA double buffer, 2×0xA00 bytes */
struct BgAffineDstData gUnk_02017BA0[0x140] __attribute__((aligned(4))); /* BG2 affine ref lines */
LinkedList2* gUnk_02018EA0 = NULL;
struct_02018EB0 gUnk_02018EB0;
s16 gUnk_02018EE0[0x800] __attribute__((aligned(4))); /* window rasterization scratch */
u16 gUnk_02021F00[0x10];                              /* lever timer→length map (32 bytes) */
u8 gUnk_020227DC[0xC];                                /* text number buffer slot 0 */
struct_020227E8 gUnk_020227E8[1];                     /* text variable slot 1 (8 bytes) */
u8 gUnk_020227F0[0x8];                                /* text variable slot 2 (8 bytes) */
u8 gUnk_020227F8[0x8];                                /* text variable slot 3 (8 bytes) */
u8 gUnk_02022800[0x20] __attribute__((aligned(4)));   /* text variable slot 4 (larger backing buffer) */
u16 gUnk_02022830[0xC00] __attribute__((aligned(4))); /* also reused as a temporary MapDataDefinition */
u8 gUnk_02024048 = 0;                                 /* pending sound count (used by DrawEntity) */
u16 gUnk_020246B0[0xC00] __attribute__((aligned(4))); /* scroll tilemap buffer */
u8 gUnk_020342F8[0x100] __attribute__((aligned(4)));  /* delayedEntityLoad array */
struct_02034480 gUnk_02034480;
u8 gUnk_02034492[0x10] __attribute__((aligned(4))); /* u8 array (pauseMenu) */
u8 gUnk_020344A0[8] __attribute__((aligned(4)));    /* figurineMenu */
struct_020354C0 gUnk_020354C0[32];
/* gUnk_02035542 is aliased to gzHeap+2 via macro in common.c (PC_PORT) */
/* Aliased to gEwram[0x36A58] in text.c */
u32 gUnk_02036A58_storage = 0x02036A58;
/* Aliased to gEwram[0x36AD8] in text.c */
u32 gUnk_02036AD8_storage = 0x02036AD8;
u32 gUnk_02036BB8 = 0x02036BB8;
// ========== Global data ==========

// Core systems
UI gUI;
HUD gHUD;
// gMenu / gChooseFileState / gIntroState share memory (EWRAM 0x02000080)
u8 _gMenuSharedStorage[0x40] __attribute__((aligned(8))) = { 0 };
Area gArea;
SaveFile gSave;
VBlankDMA gVBlankDMA;
u8 gUpdateVisibleTiles;

// Player
PlayerEntity gPlayerEntity;
PlayerState gPlayerState;
Entity* gPlayerClones[3];
ScriptExecutionContext gPlayerScriptExecutionContext;

// Entities
GenericEntity gEntities[MAX_ENTITIES];
GenericEntity gAuxPlayerEntities[MAX_AUX_PLAYER_ENTITIES];
LinkedList gEntityLists[9];
LinkedList gEntityListsBackup[9];
u8 gEntCount;
u8 gManagerCount;

// Side table for 64-bit ScriptExecutionContext pointers
// (avoids overflowing PlayerEntity's 4-byte unk_84 field with 8-byte pointers)
ScriptExecutionContext* gEntityScriptCtxTable[PC_MAX_ENTITY_SLOTS] = { 0 };
CarriedEntity gCarriedEntity;
ItemBehavior gActiveItems[MAX_ACTIVE_ITEMS];
PriorityHandler gPriorityHandler;
PossibleInteraction gPossibleInteraction;

// Room / Map
RoomControls gRoomControls;
MapLayer gMapBottom;
MapLayer gMapTop;
RoomTransition gRoomTransition;
RoomVars gRoomVars;
RoomMemory gRoomMemory[8];
RoomMemory* gCurrentRoomMemory;
void** gCurrentRoomProperties;

// Script
ActiveScriptInfo gActiveScriptInfo;
ScriptExecutionContext gScriptExecutionContextArray[0x20];

// Misc
BgAnimation gBgAnimations[MAX_BG_ANIMATIONS];
u8 gTextGfxBuffer[0xD00];
u8 gPaletteBufferBackup[0x400];
u8 gCollidableCount;
Entity* gCollidableList[MAX_COLLIDABLE_ENTITIES]; /* GBA reserves 80 slots — see entity.h */
u32 gUnk_02000020;

// gFrameObjLists — sprite frame data (200KB, self-relative offsets)
u32 gFrameObjLists[50016];
_Static_assert(sizeof(gFrameObjLists) == PORT_FRAME_OBJ_LISTS_CAPACITY_BYTES,
               "frame-object staging capacity must match the runtime bounds");

// gMapData — map data blob backed directly by the loaded ROM.
#ifdef TMC_N64
/* #N64: the ~14 MB ROM map-data window can't live in 8 MB RDRAM. Temporary 1 MB
 * placeholder so the binary links and boots to the title (which doesn't read map
 * data). Phase 3 backs gMapData with the embedded cart ROM (PI/DFS), not a RAM copy. */
u8 gMapData[0x100000] __attribute__((aligned(4))); /* 1 MB placeholder */
#else
u8* gMapData = NULL;
#endif

// gCollisionMtx — On GBA, the collision matrix label sits at 0x080B7B74 with
// only 1210 bytes of named data, but collision.c declares it as
// `extern ColSettings gCollisionMtx[173*34]` (each ColSettings = 12 bytes).
// Code in collision.c redirect-handlers indexing at e.g. 0x11AA accesses
// byte offset 0x11AA*12 = 54264 — way beyond 1210 bytes.  On GBA this
// reads ROM data that follows the label.  We allocate the full 173*34*12
// bytes and copy from ROM during Port_InitCollisionMtx().
u8 gCollisionMtx[173 * 34 * 12];

UpdateContext gUpdateContext;

u8 gInteractableObjects[0x300]
    __attribute__((aligned(8))); /* 32 InteractableObject entries * 24 bytes on 64-bit = 768 = 0x300 */

// === Additional data stubs ===

Palette gPaletteList[0x10];

// Hitbox data is now provided by src/data/hitbox.c (properly initialized)

// NPC data
NPCStruct gNPCData[NPC_DATA_CAPACITY];

/* ---------- Townsperson NPC function pointer tables ---------- */
/* These were .4byte tables in ROM pointing to Thumb function addresses.
 * On PC we define them as proper C function pointer arrays. */
extern void sub_08061BC8(Entity*);
extern void sub_08061C00(Entity*);
extern void sub_08061CEC(/* TownspersonEntity* */ Entity*);
extern void sub_08061D64(/* TownspersonEntity* */ Entity*);
extern void sub_08061E24(/* TownspersonEntity* */ Entity*);
extern void sub_08061E50(/* TownspersonEntity* */ Entity*);

void (*const gUnk_0810B774[])(Entity*) = {
    sub_08061BC8,
    sub_08061C00,
};
void (*const gUnk_0810B77C[])(Entity*) = {
    sub_08061CEC,
    sub_08061D64,
    sub_08061E24,
    sub_08061E50,
};

/* ---------- Pause menu / subtask function pointer tables ---------- */
/* From data/const/subtask.s — .4byte tables referencing C functions. */

/* PauseMenu_Screen_10 dispatch (pauseMenu.c) */
extern void sub_080A59AC(void);
extern void sub_080A59C8(void);
extern void sub_080A5A54(void);
extern void sub_080A5A90(void);
void (*const gUnk_08128D14[])(void) = {
    sub_080A59AC,
    sub_080A59C8,
    sub_080A5A54,
    sub_080A5A90,
};

/* PauseMenu_Screen_9 dispatch (pauseMenu.c) */
extern void sub_080A5AF4(void);
extern void sub_080A5B34(void);
extern void sub_080A5BB8(void);
void (*const gUnk_08128D24[])(void) = {
    sub_080A5AF4,
    sub_080A5B34,
    sub_080A5BB8,
};

/* PauseMenu_Screen_5 dispatch (pauseMenu.c) */
extern void sub_080A5C44(u32, u32, u32); /* actual signature, but called as void(void) from dispatch */
extern void sub_080A5C9C(void);
void (*const gUnk_08128D30[])(void) = {
    (void (*)(void))sub_080A5C44,
    sub_080A5C9C,
};

/* PauseMenu_Screen_7 dispatch (pauseMenu.c) */
extern void sub_080A6024(void);
extern void sub_080A6044(void);
void (*const gUnk_08128D58[])(void) = {
    sub_080A6024,
    sub_080A6044,
};

/* PauseMenu_Screen_8 dispatch (pauseMenu.c) */
extern void sub_080A6108(void);
extern void sub_080A612C(void);
void (*const gUnk_08128DB0[])(void) = {
    sub_080A6108,
    sub_080A612C,
};

/* PauseMenu_Screen_4 dispatch (pauseMenu.c) */
extern void sub_080A6290(void);
extern void sub_080A62E0(void);
void (*const gUnk_08128DCC[])(void) = {
    sub_080A6290,
    sub_080A62E0,
};

/* PauseMenu_Screen_6 dispatch (pauseMenuScreen6.c) */
extern void sub_080A6650(void);
extern void sub_080A667C(void);
void (*const gUnk_08128E78[])(void) = {
    sub_080A6650,
    sub_080A667C,
};

/* Subtask local map hint dispatch (subtaskLocalMapHint.c) */
extern void sub_080A6B04(void);
extern void sub_080A6C1C(void);
void (*const gUnk_08128F1C[])(void) = {
    sub_080A6B04,
    sub_080A6C1C,
};

// Pause menu
PauseMenuOptions gPauseMenuOptions;

SpritePtr gSpritePtrs[512];
/* port_rom.c reads up to this many entries from ROM at startup. The
 * compile-time table caps at 329 but TMC references indices up to
 * ~510 (Moldorm at 484, others). Keep in sync with the gSpritePtrs[]
 * array size above. */
const u32 kMaxSpritePtrs = 512;
/* Updated by port_rom.c after the runtime extension. Used by
 * Port_GetSpritePtr instead of the compile-time spritePtrsCount. */
u32 gSpritePtrsLoadedCount = 0;

// Map data — both are u16[0x4000] tilemaps (0x8000 bytes) reused for file-select overlay
u16 gMapDataBottomSpecial[0x4000] __attribute__((aligned(4)));
u16 gMapDataTopSpecial[0x4000] __attribute__((aligned(4)));
u32 gDungeonMap[0x800];

// Heap
u8 gzHeap[0x1000];

// Kinstone fusion
FuseInfo gFuseInfo;

// Room / tile entities
TileEntity gSmallChests[8];
SpecialTileEntry gTilesForSpecialTiles[MAX_SPECIAL_TILES];

// Collision
// gUnk_03003C70 now defined in src/collision.c

// IWRAM scratch
u8 gUnk_03000420[0x800] __attribute__((aligned(4)));
u8 gUnk_03000C30;
u8 gUnk_03001020[sizeof(Screen)] __attribute__((aligned(4)));

// Sound player data
/* On 64-bit PC: pointer fields grow from 4 to 8 bytes, so MusicPlayerInfo
 * is 96 bytes (vs GBA's 80) and MusicPlayerTrack is 112 (vs 80). The
 * hardcoded `0x50 * N` stride undersizes the buffer — and worse, the
 * gMusicPlayers table in sound.c references tracks up to `&gMPlayTracks[0x51]`
 * (player MUSIC_PLAYER_BGM uses 12 tracks starting at offset 0x46 → ends
 * at 0x51), so the array needs at least 0x52 tracks of storage. The
 * original `[0x50 * 16] = 1280-byte` declaration was wrong for *both*
 * size axes. When MPlayOpen + per-player MPlayStart writes touch tracks
 * outside the buffer, they zero adjacent globals — including
 * gAreaRoomHeaders, which made the windcrest-pin loop in
 * subtaskFastTravel.c NULL-deref (#53 second-stage). Caught with a
 * tripwire pinpointing m4aSoundInit. */
#include "gba/m4a.h"
#define M4A_MAX_INFO_INDEX 0x1C  /* gMPlayInfos uses 0x00..0x1B */
#define M4A_MAX_INFO2_INDEX 0x4  /* gMPlayInfos2 uses 0x0..0x3 */
#define M4A_MAX_TRACK_INDEX 0x52 /* gMPlayTracks uses 0x00..0x51 (BGM = 0x46+12) */
u8 gMPlayInfos[M4A_MAX_INFO_INDEX * sizeof(MusicPlayerInfo)] __attribute__((aligned(8)));
u8 gMPlayInfos2[M4A_MAX_INFO2_INDEX * sizeof(MusicPlayerInfo)] __attribute__((aligned(8)));
u8 gMPlayTracks[M4A_MAX_TRACK_INDEX * sizeof(MusicPlayerTrack)] __attribute__((aligned(8)));
u8 gMPlayMemAccArea[0x10] __attribute__((aligned(4)));

// BGM song headers (ROM data stubs)
#define SONG_STUB(name) const SongHeader name = { 0 }
SONG_STUB(bgmBeanstalk);
SONG_STUB(bgmBeatVaati);
SONG_STUB(bgmBossTheme);
SONG_STUB(bgmCastleCollapse);
SONG_STUB(bgmCastleMotif);
SONG_STUB(bgmCastleTournament);
SONG_STUB(bgmCastorWilds);
SONG_STUB(bgmCaveOfFlames);
SONG_STUB(bgmCloudTops);
SONG_STUB(bgmCredits);
SONG_STUB(bgmCrenelStorm);
SONG_STUB(bgmCuccoMinigame);
SONG_STUB(bgmDarkHyruleCastle);
SONG_STUB(bgmDeepwoodShrine);
SONG_STUB(bgmDiggingCave);
SONG_STUB(bgmDungeon);
SONG_STUB(bgmElementGet);
SONG_STUB(bgmElementTheme);
SONG_STUB(bgmElementalSanctuary);
SONG_STUB(bgmEzloGet);
SONG_STUB(bgmEzloStory);
SONG_STUB(bgmEzloTheme);
SONG_STUB(bgmFairyFountain);
SONG_STUB(bgmFairyFountain2);
SONG_STUB(bgmFestivalApproach);
SONG_STUB(bgmFightTheme);
SONG_STUB(bgmFightTheme2);
SONG_STUB(bgmFileSelect);
SONG_STUB(bgmFortressOfWinds);
SONG_STUB(bgmGameover);
SONG_STUB(bgmHouse);
SONG_STUB(bgmHyruleCastle);
SONG_STUB(bgmHyruleCastleNointro);
SONG_STUB(bgmHyruleField);
SONG_STUB(bgmHyruleTown);
SONG_STUB(bgmIntroCutscene);
SONG_STUB(bgmLearnScroll);
SONG_STUB(bgmLostWoods);
SONG_STUB(bgmLttpTitle);
SONG_STUB(bgmMinishCap);
SONG_STUB(bgmMinishVillage);
SONG_STUB(bgmMinishWoods);
SONG_STUB(bgmMtCrenel);
SONG_STUB(bgmPalaceOfWinds);
SONG_STUB(bgmPicoriFestival);
SONG_STUB(bgmRoyalCrypt);
SONG_STUB(bgmRoyalValley);
SONG_STUB(bgmSavingZelda);
SONG_STUB(bgmSecretCastleEntrance);
SONG_STUB(bgmStory);
SONG_STUB(bgmSwiftbladeDojo);
SONG_STUB(bgmSyrupTheme);
SONG_STUB(bgmTempleOfDroplets);
SONG_STUB(bgmTitleScreen);
SONG_STUB(bgmUnused);
SONG_STUB(bgmVaatiMotif);
SONG_STUB(bgmVaatiReborn);
SONG_STUB(bgmVaatiTheme);
SONG_STUB(bgmVaatiTransfigured);
SONG_STUB(bgmVaatiWrath);
SONG_STUB(bgmWindRuins);
#undef SONG_STUB

// Linker symbols (unused on PC)
u8 RAMFUNCS_END[4];
u8 gCopyToEndOfEwram_End[4];
u8 gCopyToEndOfEwram_Start[4];
u8 gEndOfEwram[4];
u8 sub_080B197C[4];
u8 ram_sub_080B197C[4];
u32 ram_MakeFadeBuff256;

/*
 * C reimplementation of ram_sub_080B197C (ARM IWRAM function).
 * Called when gUpdateVisibleTiles == 1 (initial full-screen tile fill).
 * Copies a 32×23 tile region from mapSpecial to the BG buffer.
 *
 * mapSpecial: u16 tilemap with 128 entries per row (8×8 pixel tiles).
 * bgBuffer:   passed as gBGxBuffer + 0x20 (u16 units, +0x40 bytes).
 *             The GBA code subtracts 0x40 bytes to start writing from row 0.
 */
static void ram_sub_080B197C_c(u16* mapSpecial, u16* bgBuffer) {
    u16 xdiff = gRoomControls.scroll_x - gRoomControls.origin_x;
    u16 ydiff = gRoomControls.scroll_y - gRoomControls.origin_y;

    /* Tile position in 16×16 units → each maps to 2×2 sub-tiles of 8×8.
     * Byte offset = (col16 + row16 * 128) * 4, because each 16×16 tile
     * is 2 sub-tile columns (×2 bytes each = 4 bytes) and each 16×16 row
     * spans 2 sub-tile rows (128 entries × 2 bytes × 2 = 512 bytes). */
    u32 col16 = xdiff >> 4;
    u32 row16 = ydiff >> 4;
    u8* src = (u8*)mapSpecial + (col16 + row16 * 128) * 4;

    /* On GBA gMapDataTopSpecial/gMapDataBottomSpecial are adjacent EWRAM and
     * deep scroll positions deliberately walked this copy off one buffer into
     * its neighbor (stable garbage on the offscreen seam). On PC they are
     * separate globals — clamp each 64-byte row to the 0x8000-byte buffer;
     * skipped rows leave zeros (transparent tiles) on the seam instead. */
    const u8* lo = (const u8*)mapSpecial;
    const u8* hi = lo + 0x8000;

#define COPY_ROW_CLAMPED()                 \
    do {                                   \
        if (src >= lo && src + 64 <= hi) { \
            memcpy(dst, src, 64);          \
        }                                  \
    } while (0)

    /* bgBuffer was passed as gBGxBuffer + 0x20 (in u16 units).
     * The original code does "subs r1, #0x40" to get to row 0. */
    u16* dst = bgBuffer - 0x20;

    if (ydiff < 8) {
        /* First row: copy without advancing src */
        COPY_ROW_CLAMPED(); /* 32 u16 = 64 bytes */
        dst += 32;
        /* 22 more rows: first reuses same src, then advances */
        for (int i = 0; i < 22; i++) {
            COPY_ROW_CLAMPED();
            src += 0x100; /* next 8×8 map row = 128 u16 = 256 bytes */
            dst += 32;
        }
    } else {
        /* Start one map row earlier */
        src -= 0x100;
        /* 23 consecutive rows */
        for (int i = 0; i < 23; i++) {
            COPY_ROW_CLAMPED();
            src += 0x100;
            dst += 32;
        }
    }
#undef COPY_ROW_CLAMPED
}

/* Declared in screenTileMap.c (already compiled as C on PC) */
extern void sub_0807D280(u16* mapSpecial, u16* bgBuffer);
extern void sub_0807D46C(u16* mapSpecial, u16* bgBuffer);
extern void sub_0807D6D8(u16* mapSpecial, u16* bgBuffer);

/*
 * Collision direction masks from gUnk_0800275C (now in data_stubs_autogen.c)
 * Local alias for convenient u16 access.
 */
extern const u8 gUnk_0800275C[64];
#define gCollisionDirectionMasks ((const u16*)gUnk_0800275C)

/* Collision parameter tables used by extended tile collision lookups. */
extern const u8 gUnk_080082DC[];
extern const u8 gUnk_0800833C[];
extern const u8 gUnk_0800839C[];
extern const u8 gUnk_080083FC[];
extern const u8 gUnk_0800845C[];
extern const u8 gUnk_080084BC[];
extern const u8 gUnk_0800851C[];

static const u8* sActiveCollisionParams = gUnk_080082DC;
u32 GetCollisionDataAtTilePos(u32 tilePos, u32 layer);

static const u8* SelectPlayerCollisionTable(void) {
    const u8* table = gUnk_080083FC;
    if (gPlayerState.swim_state != 0) {
        if (gPlayerState.flags & PL_MINISH) {
            table = gUnk_0800839C;
        }
        return table;
    }

    table = gUnk_0800845C;
    if (gPlayerState.jump_status != 0) {
        return table;
    }
    if (gPlayerState.flags & PL_PARACHUTE) {
        return table;
    }
    if (gPlayerState.flags & PL_MINISH) {
        return gUnk_0800833C;
    }

    table = gUnk_080084BC;
    if (gPlayerState.gustJarState == 0 && gPlayerState.heldObject == 0) {
        table = gUnk_0800851C;
        if (gPlayerState.attachedBeetleCount == 0) {
            table = gUnk_080082DC;
        }
    }
    return table;
}

/*
 * TileCollisionLookup — core tile collision lookup (port of sub_080086D8).
 *
 * Takes pixel coordinates (room-relative), looks up the collision data
 * for the tile at that position, and returns 0 (passable) or 1 (blocked).
 *
 * For tile collision types 0x00–0x0F: 2×2 sub-tile collision pattern.
 *   Bit 0 = bottom-right, bit 1 = bottom-left, bit 2 = top-right, bit 3 = top-left.
 *   The sub-tile quadrant is selected by bits 3 of x and y.
 *
 * For tile collision type 0xFF: always blocked.
 * For tile collision types 0x10–0xFE: extended pixel-level collision
 * resolves the packed pointer table through the active ROM profile.
 */
static u32 TileCollisionLookup(u32 px, u32 py, Entity* entity) {
    u32 tilePos = ((px & 0x3F0) >> 4) + ((py & 0x3F0) << 2);
    u8 tileType = (u8)GetCollisionDataAtTilePos(tilePos, entity->collisionLayer);

    if (gPlayerState.swim_state != 0 && gPlayerState.floor_type != 0x18 && tileType < 0x10) {
        tileType = 0x0F;
    }

    if (tileType < 0x10) {
        u8 bits = tileType;
        if ((py & 8) == 0) {
            bits >>= 2;
        }
        if ((px & 8) == 0) {
            bits >>= 1;
        }
        return bits & 1;
    }

    if (tileType == 0xFF) {
        return 1;
    }

    u8 idx = sActiveCollisionParams[tileType - 0x10];
    const u16* table = Port_GetCollisionShapeData(idx);
    if (table == NULL) {
        return 0;
    }

    u16 row = table[py & 0x0F];
    row >>= (0x0F ^ (px & 0x0F));
    return row & 1;
}

/*
 * sub_080085CC — fill entity->collisions by probing 8 points around entity hitbox.
 * (from Thumb asm at 0x080085CC)
 *
 * Probes are placed on the 4 edges of the collision box:
 *   Right edge:  (baseX + halfW, baseY ± vStep)
 *   Left edge:   (baseX - halfW, baseY ± vStep)
 *   Bottom edge: (baseX ± hStep, baseY + halfH)
 *   Top edge:    (baseX ± hStep, baseY - halfH)
 *
 * Hitbox layout (bytes 2–5 of Hitbox struct = unk2[0..3]):
 *   unk2[0] = halfW  (half-width for collision)
 *   unk2[1] = vStep  (vertical probe step)
 *   unk2[2] = hStep  (horizontal probe step)
 *   unk2[3] = halfH  (half-height for collision)
 *
 * Result is a 16-bit mask stored in entity->collisions:
 *   Bit 14: right edge, south probe
 *   Bit 13: right edge, north probe
 *   Bit 10: left edge, south probe
 *   Bit  9: left edge, north probe
 *   Bit  6: bottom edge, east probe
 *   Bit  5: bottom edge, west probe
 *   Bit  2: top edge, east probe
 *   Bit  1: top edge, west probe
 */
void sub_080085CC(Entity* ent) {
    Hitbox* hb = (Hitbox*)port_resolve_addr((uintptr_t)ent->hitbox);
    if (hb == NULL) {
        ent->collisions = 0;
        return;
    }

    sActiveCollisionParams = SelectPlayerCollisionTable();

    s32 baseX = (s32)(u16)ent->x.HALF.HI - (s32)gRoomControls.origin_x + hb->offset_x;
    s32 baseY = (s32)(u16)ent->y.HALF.HI - (s32)gRoomControls.origin_y + hb->offset_y;

    u32 sideX = (u8)hb->unk2[0];
    u32 sideY = (u8)hb->unk2[1];
    u32 innerX = (u8)hb->unk2[2];
    u32 innerY = (u8)hb->unk2[3];

    u16 collisions = 0;
    collisions |= (u16)(TileCollisionLookup((u32)(baseX + (s32)sideX), (u32)(baseY + (s32)sideY), ent) << 14);
    collisions |= (u16)(TileCollisionLookup((u32)(baseX + (s32)sideX), (u32)(baseY - (s32)sideY), ent) << 13);
    collisions |= (u16)(TileCollisionLookup((u32)(baseX - (s32)sideX), (u32)(baseY + (s32)sideY), ent) << 10);
    collisions |= (u16)(TileCollisionLookup((u32)(baseX - (s32)sideX), (u32)(baseY - (s32)sideY), ent) << 9);
    collisions |= (u16)(TileCollisionLookup((u32)(baseX + (s32)innerX), (u32)(baseY + (s32)innerY), ent) << 6);
    collisions |= (u16)(TileCollisionLookup((u32)(baseX - (s32)innerX), (u32)(baseY + (s32)innerY), ent) << 5);
    collisions |= (u16)(TileCollisionLookup((u32)(baseX + (s32)innerX), (u32)(baseY - (s32)innerY), ent) << 2);
    collisions |= (u16)(TileCollisionLookup((u32)(baseX - (s32)innerX), (u32)(baseY - (s32)innerY), ent) << 1);
    ent->collisions = collisions;
}

u32 sub_080086D8(u32 roomX, u32 roomY, const u8* params) {
    sActiveCollisionParams = params;
    return TileCollisionLookup(roomX, roomY, &gPlayerEntity.base);
}

u32 sub_080086B4(u32 roomX, u32 roomY, const u8* params) {
    return sub_080086D8(roomX, roomY, params);
}

/*
 * CalcCollisionDirectionOLD — adjust movement direction based on collision data
 * When an entity is moving in a cardinal direction and hitting a wall,
 * this returns a perpendicular direction to slide along the wall.
 * (from Thumb asm at 0x08002864)
 *
 * r0 = direction (0–31), r1 = collisions (entity->collisions)
 * Returns adjusted direction or original if no slide.
 */
static u32 CalcCollisionDirectionOLD(u32 direction, u32 collisions) {
    u32 quadrant = direction >> 3; /* 0=N, 1=E, 2=S, 3=W */

    switch (quadrant) {
        case 0: /* North */
            if (!(collisions & 0x000E))
                return direction;
            if (!(collisions & 0xE004))
                return 0x08; /* East */
            if (!(collisions & 0x0E02))
                return 0x18; /* West */
            return direction;

        case 1: /* East */
            if (!(collisions & 0xE000))
                return direction;
            if (!(collisions & 0x200E))
                return 0x00; /* North */
            if (!(collisions & 0x40E0))
                return 0x10; /* South */
            return direction;

        case 2: /* South */
            if (!(collisions & 0x00E0))
                return direction;
            if (!(collisions & 0xE040))
                return 0x08; /* East */
            if (!(collisions & 0x0E20))
                return 0x18; /* West */
            return direction;

        case 3: /* West */
            if (!(collisions & 0x0E00))
                return direction;
            if (!(collisions & 0x020E))
                return 0x00; /* North */
            if (!(collisions & 0x04E0))
                return 0x10; /* South */
            return direction;

        default:
            return direction;
    }
}

/*
 * LinearMoveDirectionOLD — move entity by speed in given direction, checking collisions.
 * (from Thumb asm at 0x080027EA)
 *
 * Updates entity->x.WORD and entity->y.WORD using sine table lookup.
 * Returns bitmask: bit 0 = X moved, bit 1 = Y moved.
 */
u32 LinearMoveDirectionOLD(Entity* ent, u32 speed, u32 direction) {
    u32 moved = 0;

    if (direction & 0x80)
        return 0; /* DIR_NOT_MOVING */

    u16 collisions = ent->collisions;

    /* If direction is exactly cardinal (no sub-cardinal bits), try wall sliding */
    if ((direction & 7) == 0) {
        u32 adjusted = CalcCollisionDirectionOLD(direction, collisions);
        if (adjusted != direction) {
            direction = adjusted;
            speed = 0x100; /* slow down when sliding */
        }
    }

    /* Look up collision mask for this direction */
    u16 colMask = gCollisionDirectionMasks[direction & 0x1F];
    u16 masked = collisions & colMask;

    /* X movement */
    if (!(masked & 0xEE00)) {
        s16 sinVal = gSineTable[direction * 8];
        if (sinVal != 0) {
            moved |= 1;
            s32 dx = FixedMul(sinVal, (s16)speed) << 8;
            ent->x.WORD += dx;
        }
    }

    /* Y movement */
    if (!(masked & 0x00EE)) {
        s16 cosVal = gSineTable[direction * 8 + 64];
        if (cosVal != 0) {
            moved |= 2;
            s32 dy = FixedMul(cosVal, (s16)speed) << 8;
            ent->y.WORD -= dy;
        }
    }

    return moved;
}

/*
 * sub_0800857C — player movement wrapper.
 * Calls sub_080085CC (collision tile update) then LinearMoveDirectionOLD.
 * (from Thumb asm at 0x0800857C)
 */
void sub_0800857C(Entity* ent) {
    if (((u8)ent->type2 & 0x80) == 0 && (gPlayerState.jump_status & 0x80) == 0) {
        sub_080085CC(ent);
    }
    LinearMoveDirectionOLD(ent, ent->speed, ent->direction);
}

/*
 * sub_080085B0 — collision tile update wrapper.
 * (from Thumb asm at 0x080085B0)
 * Calls sub_080085CC — tile collision detection for the player.
 */
void sub_080085B0(Entity* ent) {
    sub_080085CC(ent);
}

/*
 * sub_08008AA0 — set player velocity direction from sine table.
 * (from Thumb asm at 0x08008AA0)
 *   Reads gPlayerState.direction, looks up sin/cos, stores in vel_x/vel_y.
 */
void sub_08008AA0(Entity* ent) {
    (void)ent;
    if (gPlayerState.floor_type == 1)
        return;
    u8 dir = gPlayerState.direction;
    if (dir == 0xFF)
        return;
    gPlayerState.vel_x = gSineTable[dir * 8];
    gPlayerState.vel_y = -gSineTable[dir * 8 + 64];
}

/*
 * sub_08008AC6 — check if player should respawn (fallen off edge, etc.)
 * Port of Thumb asm at 0x08008AC6; swim/flag guards and
 * GetNonCollidedSide/RespawnPlayer flow match the original routine.
 */
static u32 GetNonCollidedSide(Entity* ent) {
    u16 c = ent->collisions;
    for (int i = 0; i < 4; i++) {
        if ((c & 0x000E) == 0) {
            return 0;
        }
        c >>= 4;
    }
    return 1;
}

void sub_08008AC6(Entity* ent) {
    if ((gPlayerState.swim_state & 0x0F) != 0) {
        return;
    }
    if ((gPlayerState.flags & gUnk_02000020) != 0) {
        return;
    }
    if (GetNonCollidedSide(ent) != 0) {
        ent->iframes = (s8)0xE2;
        RespawnPlayer();
    }
}

/*
 * GetNextFunction — entity state machine dispatcher
 * Returns 0-5 based on entity combat/action state.
 * (from Thumb asm at 0x0800279C)
 */
u32 GetNextFunction(Entity* this) {
    u8 gustJarState = this->gustJarState;
    u8 contactFlags = this->contactFlags;

    /* Peahat (and other gust-jar-killable enemies) reach Subaction5 with
     * health=0 + gustJarState=0x04 + ENT_COLLIDE clear, expecting the
     * death sequence to take over. The gj=0x04 short-circuit at top would
     * send them right back to OnGrabbed forever — corpse never despawns
     * (#20). When dead, prefer the death-cascade dispatch so GenericDeath
     * runs and the DEATH_FX cleanup chain fires.
     *
     * AcroBandit's chain unwind lives in OnCollision and only runs on the
     * death frame (contactFlags bit 7 + health hits 0); short-circuiting
     * straight to OnDeath would leave the surviving bandits stuck in
     * Action4 with stale parent pointers (#35). Death-fall animation runs
     * via OnKnockback while knockbackDuration is nonzero, so OnKnockback
     * needs to keep dispatching during dying frames too. Fall through to
     * OnDeath only when neither is active. */
    if (this->health == 0) {
        if (!(gustJarState & 4) && (contactFlags & 0x80))
            return 1; /* OnCollision: chain unwind + death-state setup */
        if (this->knockbackDuration != 0)
            return 2; /* OnKnockback: death-fall animation */
        if (this->action == 0 && this->subAction == 0)
            return 0;
        if (gustJarState & 8)
            return 5;
        if (this->confusedTime != 0)
            return 4;
        return 3; /* OnDeath */
    }

    /* GBA-original alive dispatch order: gust-jar grab > contact >
     * knockback > confused > tick. Matches the Thumb asm at 0x0800279C.
     *
     * #54: dizzy-stars FX never leaves a boomerang-stunned enemy. The
     * confusedTime check below was previously omitted, so GenericConfused
     * never ran on living enemies — confusedTime never decremented, the
     * FX_STARS object's parent (the enemy) stayed alive forever, and
     * sub_08084694 (the FX's update) only self-deletes when its parent
     * is gone. Re-introducing the dispatch lets GenericConfused tick
     * confusedTime down and call EnemyDetachFX at the end of stun, which
     * orphans the FX so its next tick deletes it. Empirically all FX_STARS
     * spawners under src/enemy use EnemyCreateFX (stored in
     * entity->child), so the existing detach path covers them. */
    if (gustJarState & 4)
        return 5;

    if (contactFlags & 0x80)
        return 1;

    if (this->knockbackDuration != 0)
        return 2;

    if (this->confusedTime != 0)
        return 4;

    if (this->action == 0 && this->subAction == 0)
        return 0;

    return 0;
}

/*
 * Random — simple 32-bit PRNG (from ARM asm at 0x08000E50)
 *   state = ROR(state * 3, 13);  return state >> 1;
 */
u32 Random(void) {
    gRand = Port_Rng_Advance(gRand);
    return Port_Rng_Output(gRand);
}

/*
 * CheckBits — test whether `count` consecutive bits starting at `bitIndex`
 *             are all set in the byte array `base`.  (from ARM asm at 0x080B20EC)
 *   Returns 1 if ALL bits set, 0 if any bit is clear.
 */
u32 CheckBits(void* base, u32 bitIndex, u32 count) {
    const u8* ptr = (const u8*)base + (bitIndex / 8);
    u32 bit = bitIndex % 8;
    for (u32 i = 0; i < count; i++) {
        if (!(ptr[0] & (1u << bit)))
            return 0;
        bit++;
        if (bit >= 8) {
            bit = 0;
            ptr++;
        }
    }
    return 1;
}

/*
 * Mod — modulo (SWI 0x06 wrapper): returns num % denom
 */
s32 Mod(s32 num, s32 denom) {
    if (denom == 0)
        return 0;
    return num % denom;
}

/*
 * SumDropProbabilities — vector add 3 arrays of 16 s16 values.
 *   out[i] = a[i] + b[i] + c[i]   for i = 0..15
 */
void SumDropProbabilities(s16* out, const s16* a, const s16* b, const s16* c) {
    for (int i = 0; i < 16; i++) {
        out[i] = a[i] + b[i] + c[i];
    }
}

/*
 * SumDropProbabilities2 — vector add, clamp each to >= 0, return scalar sum.
 *   out[i] = max(0, a[i] + b[i] + c[i])   for i = 0..15
 *   returns sum of all out[i]
 */
u32 SumDropProbabilities2(s16* out, const s16* a, const s16* b, const s16* c) {
    u32 sum = 0;
    for (int i = 0; i < 16; i++) {
        s16 val = a[i] + b[i] + c[i];
        if (val < 0)
            val = 0;
        out[i] = val;
        sum += val;
    }
    return sum;
}

/*
 * UpdateScrollVram — copies map tile data from gMapDataBottomSpecial / gMapDataTopSpecial
 * into gBG1Buffer / gBG2Buffer depending on gUpdateVisibleTiles.
 *
 * Replaces the ARM veneer at 0x08000108 and the IWRAM sub_080B197C.
 */
#if MODE1_GBA_WIDTH > 240
static int Port_WidescreenFillScroll(u16* map, u16* buffer);
#endif
void UpdateScrollVram(void) {
    typedef void (*ScrollVramFunc)(u16*, u16*);
    static const ScrollVramFunc funcs[] = {
        NULL,               /* 0: unused (returns immediately) */
        ram_sub_080B197C_c, /* 1: initial full-screen fill */
        sub_0807D280,       /* 2: scroll update mode */
        sub_0807D46C,       /* 3: scroll update mode */
        sub_0807D6D8,       /* 4: scroll update mode */
    };

    u8 mode = gUpdateVisibleTiles;
    if (mode == 0 || mode > 4)
        return;

    ScrollVramFunc func = funcs[mode];

    /* Bottom layer → BG1 */
    if (gMapBottom.bgSettings != NULL) {
#if MODE1_GBA_WIDTH > 240
        if (!Port_WidescreenFillScroll(gMapDataBottomSpecial, gBG1Buffer))
#endif
            func(gMapDataBottomSpecial, &gBG1Buffer[0x20]);
    }

    /* Top layer → BG2 */
    if (gMapTop.bgSettings != NULL) {
#if MODE1_GBA_WIDTH > 240
        if (!Port_WidescreenFillScroll(gMapDataTopSpecial, gBG2Buffer))
#endif
            func(gMapDataTopSpecial, &gBG2Buffer[0x20]);
    }
}

/* The iris fade (src/fade.c) draws WIN0 from an HBlank table of 160 8-bit
 * spans, so it cannot reach past x=255 or y=159. Each table it hands to the
 * VBlank DMA is noted here with its circle; a wider or taller view then draws
 * the same circle across the whole view (Port_Widescreen_Win0Lines). The game
 * double-buffers the tables, so they are told apart by address. */
typedef struct {
    const void* table;
    s32 x, y;
    u32 size; /* the iris's 0..150 opening, or a radius in pixels | WS_CIRCLE_RADIUS_PX */
} PortWsIris;
/* size holds a radius in pixels (a Minish portal's closing circle). A flag bit and
 * not a field: these statics sit among the game's globals, and growing them moves
 * every global after them, which breaks loading states saved by other builds. */
#define WS_CIRCLE_RADIUS_PX 0x80000000u
/* With WS_CIRCLE_RADIUS_PX: a dark room's light (Port_Widescreen_NoteLightCircle), drawn
 * at its own radius under the game's window settings. */
#define WS_CIRCLE_LIGHT 0x40000000u
static PortWsIris sWsIris[2];
/* The circle of the last frame drawn with one: the game tears the HBlank DMA
 * down in the tick that ends the iris (sub_0801E104), but the frame presented
 * next still has the registers of the VBlank before, window on. */
static PortWsIris sWsIrisDrawn;
static u8 sWsIrisNext;
static u8 sWsIrisLatest;

void Port_Widescreen_ForgetIris(const void* table) {
    int i;
    for (i = 0; i < 2; i++) {
        if (sWsIris[i].table == table) {
            sWsIris[i].table = NULL;
        }
    }
    sWsIrisLatest = 0;
}

static void Port_WidescreenNoteCircle(s32 x, s32 y, u32 size) {
    const void* table = gScreen.vBlankDMA.src;
    PortWsIris* slot = &sWsIris[0];
    if (sWsIris[0].table != table) {
        if (sWsIris[1].table == table) {
            slot = &sWsIris[1];
        } else {
            slot = &sWsIris[sWsIrisNext];
            sWsIrisNext ^= 1;
        }
    }
    slot->table = table;
    slot->x = x;
    slot->y = y;
    slot->size = size;
    sWsIrisLatest = 1;
}

void Port_Widescreen_NoteIris(void) {
    Port_WidescreenNoteCircle((s16)gFadeControl.iris_x, (s16)gFadeControl.iris_y, gFadeControl.iris_size);
}

/* The Minish portal's closing circle (src/object/minishPortalCloseup.c) is the
 * iris's window with a radius in pixels: centred on the portal in view pixels,
 * it is drawn across a wide view the same way. */
void Port_Widescreen_NoteWindowCircle(int x, int y, int radius) {
    Port_WidescreenNoteCircle(x, y, (u32)radius | WS_CIRCLE_RADIUS_PX);
}

/* A dark room's light (src/manager/lightManager.c) is a circle of `radius` pixels
 * around Link with the game's own window settings: inside WIN0 no darkness (BG3),
 * outside it all. Drawn across a wide view as it is, not scaled to the view. */
void Port_Widescreen_NoteLightCircle(int x, int y, int radius) {
    Port_WidescreenNoteCircle(x, y, (u32)radius | WS_CIRCLE_RADIUS_PX | WS_CIRCLE_LIGHT);
}

/* The window the game drives now is a noted iris. */
static int Port_WidescreenIrisLive(void) {
    return sWsIrisLatest && (gScreen.lcd.displayControl & (DISPCNT_WIN0_ON | DISPCNT_WIN1_ON | DISPCNT_OBJWIN_ON)) ==
                                DISPCNT_WIN0_ON;
}

static u32 Port_WidescreenIsqrt(u32 v) {
    u32 r = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

int Port_Widescreen_Win0Lines(int width, int height, u16* out) {
    const void* table;
    const PortWsIris* iris = NULL;
    u32 radius;
    int line, i;
    const u16 dispcnt = (u16)(gIoMem[REG_OFFSET_DISPCNT] | (gIoMem[REG_OFFSET_DISPCNT + 1] << 8));
    if ((width <= 240 && height <= 160) || height > 240 ||
        (dispcnt & (DISPCNT_WIN0_ON | DISPCNT_WIN1_ON | DISPCNT_OBJWIN_ON)) != DISPCNT_WIN0_ON) {
        sWsIrisDrawn.table = NULL;
        return PORT_WS_WIN0_NONE;
    }
    table = port_hdma_source_for_dest(&gIoMem[REG_OFFSET_WIN0H]);
    for (i = 0; i < 2 && table != NULL; i++) {
        if (sWsIris[i].table == table)
            iris = &sWsIris[i];
    }
    if (iris == NULL && table == NULL && sWsIrisDrawn.table != NULL)
        iris = &sWsIrisDrawn;
    if (iris == NULL) {
        sWsIrisDrawn.table = NULL;
        return PORT_WS_WIN0_NONE;
    }
    if (!(iris->size & WS_CIRCLE_LIGHT)) {
        /* The iris's settings (SetFadeIris): everything inside, the backdrop alone
         * outside. The GPU renderer draws it as that mask. */
        if ((gIoMem[REG_OFFSET_WINOUT] & 0x3f) != 0)
            return PORT_WS_WIN0_NONE;
        /* The portal's circle hides BG3 inside as well: the software PPU honours that
         * (WININ), the GPU renderer's mask shows BG3 inside. */
        if ((gIoMem[REG_OFFSET_WININ] & 0x3f) != 0x3f && !(iris->size & WS_CIRCLE_RADIUS_PX))
            return PORT_WS_WIN0_NONE;
    }
    sWsIrisDrawn = *iris;
    /* The game opens the iris up to 150 and then drops the window: on 240x160
     * around the middle that clears the corners. Scale 150 to this view's
     * farthest corner from the centre, or an iris away from the middle (Ezlo
     * near a room's edge) jumps from a circle to the whole view at its end. */
    {
        const s32 dx = iris->x > width / 2 ? iris->x : width - iris->x;
        const s32 dy = iris->y > height / 2 ? iris->y : height - iris->y;
        const u32 farthest = Port_WidescreenIsqrt((u32)(dx * dx + dy * dy));
        if (iris->size & WS_CIRCLE_LIGHT) {
            radius = iris->size & ~(WS_CIRCLE_RADIUS_PX | WS_CIRCLE_LIGHT);
        } else if (iris->size & WS_CIRCLE_RADIUS_PX) {
            /* The portal's circle starts at 128 px (MinishPortalCloseup_Init) and
             * closes 2 px a frame. Its start is scaled to this view's farthest
             * corner, so the first frame with the window still shows the whole
             * view and it closes evenly from there: unscaled, a portal near the
             * view's edge started as a half-screen circle; scaled to the GBA's
             * 144 px corner, the first frame already cut a corner off. */
            radius = (iris->size & ~WS_CIRCLE_RADIUS_PX) * farthest / 128u;
        } else {
            radius = iris->size * farthest / 150u + 1u;
        }
    }
    for (line = 0; line < height; line++) {
        const s32 dy = line - iris->y;
        const u32 ady = (u32)(dy < 0 ? -dy : dy);
        s32 left = 0, right = 0;
        if (ady < radius) {
            const s32 half = (s32)Port_WidescreenIsqrt(radius * radius - ady * ady);
            left = iris->x - half;
            right = iris->x + half;
            if (left < 0)
                left = 0;
            if (right > width)
                right = width;
            if (left >= right)
                left = right = 0;
        }
        out[line * 2] = (u16)left;
        out[line * 2 + 1] = (u16)right;
    }
    return (iris->size & WS_CIRCLE_LIGHT) ? PORT_WS_WIN0_WINDOW : PORT_WS_WIN0_MASK;
}

#if defined(MODE1_GBA_WIDTH) && (MODE1_GBA_WIDTH > 240)
#include "cpu/mode1.h"

/*
 * Widescreen Phase 2 (Option A) — port-side shadow tilemap.
 *
 * The PPU (libs/ViruaPPU/src/mode1.c::render_text_bg_line) reads tile
 * entries from these for display cols >= MODE1_GBA_BG_CLIP_X on 32-tile
 * BGs, bypassing VRAM — the 32-tile screenblock only spans 256 px of
 * world and wraps past that, so the reveal columns can't come from VRAM.
 *
 * Storage has Full View capacity in the 400px build. Normal Wide rows remain
 * packed at MODE1_WS_SHADOW_WIDE_COLS, preserving E2's 32*7-cell VBlank work
 * and cache footprint on Old 3DS. Rows are indexed by tile_row (mod 32,
 * mirroring the engine's vertical rolling) and (tile_col - base) mod 32.
 * Populated each
 * scroll-update from gMapData{Bottom,Top}Special (128x128 BG-tiles) at
 * world cols [cam_bg_col + MODE1_GBA_BG_CLIP_X/8 .. + COLS).
 *
 * Gated #if width>240: the native build never compiles this and the PPU
 * shadow pointers stay NULL (render falls back to clip-at-240). */
static u16 sWsShadowBG1[MODE1_WS_SHADOW_ROWS * MODE1_WS_SHADOW_COLS];
static u16 sWsShadowBG2[MODE1_WS_SHADOW_ROWS * MODE1_WS_SHADOW_COLS];

/* Retain only the outgoing viewport before LoadRoomGfx replaces its maps.
 * This is tile data, not a frozen image: the current palettes/animations still
 * apply. Both the native screenblock and reveal use the same world sampler. */
/* Sized for Full View (400x240 plus a tile each side, and the sideways
 * alignment below); normal Wide uses the first 36x24. */
enum { WS_SCROLL_COLS = 96, WS_SCROLL_ROWS = 64, WS_SCROLL_ALIGN_SPEED = 4 };
static struct {
    int valid, area, room, direction;
    int fullView; /* taken in Full View: the scroll keeps the 400x240 view */
    int cols, rows;
    int startX, startY, tileX, tileY;
    /* Full View: where the camera goes across the scroll before it moves along
     * it, and in how many of the scroll's ticks (Port_Widescreen_AdvanceScroll). */
    int alignX, alignY, alignTicks;
    u16 tiles[2][WS_SCROLL_ROWS][WS_SCROLL_COLS];
} sWsScroll;

/* The camera position on one axis that keeps a `view`-wide window inside both
 * the outgoing room [oldOrigin, +oldSize) and the incoming one, closest to
 * `camera`; `camera` itself when no position fits both. */
static int Port_WidescreenScrollAlign(int camera, int view, int oldOrigin, int oldSize, int newOrigin, int newSize) {
    const int lo = oldOrigin > newOrigin ? oldOrigin : newOrigin;
    const int oldMax = oldOrigin + oldSize - view, newMax = newOrigin + newSize - view;
    const int hi = oldMax < newMax ? oldMax : newMax;
    if (lo > hi)
        return camera;
    return camera < lo ? lo : camera > hi ? hi : camera;
}

static Port3DSFullViewMode Port_WidescreenGameplayMode(void);

#ifndef PORT_NEIGHBOUR_NO_PREFERENCE
#define PORT_NEIGHBOUR_NO_PREFERENCE (-0x7fffffff)
#endif
/* Where a Full View rests on a room smaller than it along one axis (0 = x, 1 = y): centred, or off centre where
 * the neighbours drawn beside it (P4.9) fill more of the view. */
static int Port_WidescreenCentredRest(unsigned room, int axis, int view, int prefer) {
#ifdef TMC_3DS
    return Port_NeighbourRooms_Rest(room, axis, view, prefer);
#else
    const RoomResInfo* r = &gArea.roomResInfos[room];
    (void)prefer;
    return axis ? (int)r->map_y - (view - (int)r->pixel_height) / 2 : (int)r->map_x - (view - (int)r->pixel_width) / 2;
#endif
}

/* The scroll's line-up across its axis into a room smaller than the view: where the incoming room rests best,
 * leaving the least black while the view lines up in the outgoing room (P4.9); centred without neighbours. */
static int Port_WidescreenLineUp(unsigned room, int axis, int view, int prefer, int otherStart, int otherView) {
#ifdef TMC_3DS
    return Port_NeighbourRooms_LineUp(room, axis, view, prefer, gRoomControls.room, otherStart, otherView);
#else
    (void)otherStart;
    (void)otherView;
    return Port_WidescreenCentredRest(room, axis, view, prefer);
#endif
}

void Port_Widescreen_BeginScroll(unsigned room, unsigned direction) {
    sWsScroll.valid = 0;
    const int width = Port_Widescreen_GameplayViewWidth();
    if (direction > 3 || gRoomControls.scrollAction > 1 || gArea.unk_0c_0 || room >= MAX_ROOMS)
        return;
    /* Full View scrolls at 400x240 into a room that can hold that view, interiors included
     * (every room is in Full View since P4.6: the castle's rooms scrolled at 266x160 when
     * only exteriors counted); the next room's own content scan then confirms it. Anything
     * else keeps the 266 scroll or none. */
    const int fullView = Port_WidescreenGameplayMode() == PORT_3DS_FULL_VIEW_OUTDOOR_1X &&
                         gRoomControls.area <= AREA_98;
    /* A room drawn as a picture (scroll_flags bit 0: the Minish houses, Gentari's two halves) keeps its tilemap in
     * gMapDataBottomSpecial at the same 128-entry stride once loaded (sub_0807C5F4), so the Full View snapshot
     * holds; the 266 scroll is left without it, as upstream had it. */
    if (!fullView && (gRoomControls.scroll_flags & 1))
        return;
    if (!fullView && (width <= 240 || width > 266 || gArea.roomResInfos[room].pixel_width <= 240 ||
                      gArea.roomResInfos[room].pixel_height < 160 || gRoomControls.height < 160))
        return;
    sWsScroll.valid = 1;
    sWsScroll.fullView = fullView;
    sWsScroll.cols = fullView ? WS_SCROLL_COLS : 36;
    sWsScroll.rows = fullView ? WS_SCROLL_ROWS : 24;
    sWsScroll.area = gRoomControls.area;
    sWsScroll.room = room;
    sWsScroll.direction = direction;
    sWsScroll.startX = gRoomControls.scroll_x;
    sWsScroll.startY = gRoomControls.scroll_y;
    /* Full View: the 400x240 window can stick out of the incoming room across
     * the scroll (a narrower or offset room below/above, a lower or offset one
     * to a side) and show black outside it. The retail camera only lines up
     * after the scroll. Line it up first, inside the outgoing room, then
     * scroll straight. The snapshot below then covers both positions. */
    sWsScroll.alignX = sWsScroll.startX;
    sWsScroll.alignY = sWsScroll.startY;
    sWsScroll.alignTicks = 0;
    if (fullView) {
        const RoomResInfo* next = &gArea.roomResInfos[room];
        int shift;
        if (direction == 0 || direction == 2) {
            /* A room narrower than the view is centred, the rest black: line up on that. */
            sWsScroll.alignX = next->pixel_width < 400
                                   ? Port_WidescreenLineUp(room, 0, 400, sWsScroll.startX, sWsScroll.startY, 240)
                                   : Port_WidescreenScrollAlign(gRoomControls.scroll_x, 400, gRoomControls.origin_x,
                                                                gRoomControls.width, next->map_x, next->pixel_width);
            shift = sWsScroll.alignX - sWsScroll.startX;
            if (shift > (WS_SCROLL_COLS - 53) * 8 || -shift > (WS_SCROLL_COLS - 53) * 8)
                sWsScroll.alignX = sWsScroll.startX, shift = 0;
        } else {
            sWsScroll.alignY = next->pixel_height < 240
                                   ? Port_WidescreenLineUp(room, 1, 240, sWsScroll.startY, sWsScroll.startX, 400)
                                   : Port_WidescreenScrollAlign(gRoomControls.scroll_y, 240, gRoomControls.origin_y,
                                                                gRoomControls.height, next->map_y, next->pixel_height);
            shift = sWsScroll.alignY - sWsScroll.startY;
            if (shift > (WS_SCROLL_ROWS - 33) * 8 || -shift > (WS_SCROLL_ROWS - 33) * 8)
                sWsScroll.alignY = sWsScroll.startY, shift = 0;
        }
        if (shift < 0)
            shift = -shift;
        sWsScroll.alignTicks = (shift + WS_SCROLL_ALIGN_SPEED - 1) / WS_SCROLL_ALIGN_SPEED;
        if (sWsScroll.alignTicks > 20) /* half the shorter (vertical) scroll */
            sWsScroll.alignTicks = 20;
        sWsScroll.cols = 52 + (sWsScroll.alignX - sWsScroll.startX >= 0 ? sWsScroll.alignX - sWsScroll.startX
                                                                         : sWsScroll.startX - sWsScroll.alignX) / 8 + 1;
        sWsScroll.rows = 32 + (sWsScroll.alignY - sWsScroll.startY >= 0 ? sWsScroll.alignY - sWsScroll.startY
                                                                         : sWsScroll.startY - sWsScroll.alignY) / 8 + 1;
        if (sWsScroll.cols > WS_SCROLL_COLS) sWsScroll.cols = WS_SCROLL_COLS;
        if (sWsScroll.rows > WS_SCROLL_ROWS) sWsScroll.rows = WS_SCROLL_ROWS;
    }
    {
        const int left = sWsScroll.alignX < sWsScroll.startX ? sWsScroll.alignX : sWsScroll.startX;
        const int top = sWsScroll.alignY < sWsScroll.startY ? sWsScroll.alignY : sWsScroll.startY;
        sWsScroll.tileX = (left >> 3) - 1;
        sWsScroll.tileY = (top >> 3) - 1;
    }
    for (int layer = 0; layer < 2; ++layer) {
        const u16* map = layer ? gMapDataTopSpecial : gMapDataBottomSpecial;
        for (int y = 0; y < sWsScroll.rows; ++y) {
            const int my = sWsScroll.tileY + y - gRoomControls.origin_y / 8;
            for (int x = 0; x < sWsScroll.cols; ++x) {
                const int mx = sWsScroll.tileX + x - gRoomControls.origin_x / 8;
                sWsScroll.tiles[layer][y][x] = mx >= 0 && mx < gRoomControls.width / 8 && mx < 128 && my >= 0 &&
                                                       my < gRoomControls.height / 8 && my < 128
                                                   ? map[my * 128 + mx]
                                                   : 0;
            }
        }
    }
}

static int Port_WidescreenScrollActive(void) {
    /* A Full View scroll is live from its first tick: the room has been
     * replaced by then (the load runs in the same GameMain_Update), and the
     * 400x240 view samples the outgoing tiles from the snapshot at once. */
    return sWsScroll.valid && gMain.task == TASK_GAME && gRoomControls.area == sWsScroll.area &&
           gRoomControls.room == sWsScroll.room && gRoomControls.scroll_direction == sWsScroll.direction &&
           (gRoomControls.scrollAction == 2 || gRoomControls.scrollAction == 0) &&
           (gRoomControls.scrollSubAction >= 1 || sWsScroll.fullView) &&
           (sWsScroll.fullView || !(gRoomControls.scroll_flags & 1)) &&
           Port_Config_WidescreenEnabled();
}

/* A room-border scroll that keeps Full View (see Port_Widescreen_BeginScroll),
 * through its last tick: the scroll ends (scrollAction 0) one tick before
 * Scroll0 clears reload_flags. */
static int Port_WidescreenFullViewScrollActive(void) {
    return sWsScroll.fullView && Port_WidescreenScrollActive() &&
           (gRoomControls.scrollAction == 2 || gRoomControls.reload_flags != 0);
}

int Port_Widescreen_AdvanceScroll(void) {
    if (!Port_WidescreenScrollActive())
        return 0;
    /* Keep the retail 60/40-tick duration and 15px player carry. Only the
     * camera's endpoint changes to the actual viewport edge. */
    int endX = sWsScroll.startX, endY = sWsScroll.startY;
    int duration = 40;
    const int viewW = sWsScroll.fullView ? 400 : Port_Widescreen_GameplayViewWidth();
    const int viewH = sWsScroll.fullView ? 240 : 160;
    switch (sWsScroll.direction) {
        case 0:
            endY = gRoomControls.origin_y + gRoomControls.height - viewH;
            break;
        case 1:
            endX = gRoomControls.origin_x;
            duration = 60;
            break;
        case 2:
            endY = gRoomControls.origin_y;
            break;
        case 3:
            endX = gRoomControls.origin_x + gRoomControls.width - viewW;
            duration = 60;
            break;
    }
    /* Full View: a room smaller than the view on the scrolling axis ends centred in it. */
    if (sWsScroll.fullView) {
        if ((sWsScroll.direction == 0 || sWsScroll.direction == 2) && gRoomControls.height < viewH)
            endY = Port_WidescreenCentredRest(gRoomControls.room, 1, viewH, PORT_NEIGHBOUR_NO_PREFERENCE);
        if ((sWsScroll.direction == 1 || sWsScroll.direction == 3) && gRoomControls.width < viewW)
            endX = Port_WidescreenCentredRest(gRoomControls.room, 0, viewW, PORT_NEIGHBOUR_NO_PREFERENCE);
    }
    int progress = gRoomControls.unk_18;
    if (progress > duration)
        progress = duration;
    /* Full View: the first alignTicks line the camera up across the scroll
     * (still inside the outgoing room), the rest scroll along it. */
    const int align = sWsScroll.alignTicks < duration ? sWsScroll.alignTicks : 0;
    const int vertical = sWsScroll.direction == 0 || sWsScroll.direction == 2;
    if (vertical) endX = sWsScroll.alignX;
    else endY = sWsScroll.alignY;
    if (align > 0 && progress < align) {
        gRoomControls.scroll_x = sWsScroll.startX + (sWsScroll.alignX - sWsScroll.startX) * progress / align;
        gRoomControls.scroll_y = sWsScroll.startY + (sWsScroll.alignY - sWsScroll.startY) * progress / align;
        return 1;
    }
    const int fromX = vertical ? sWsScroll.alignX : sWsScroll.startX;
    const int fromY = vertical ? sWsScroll.startY : sWsScroll.alignY;
    gRoomControls.scroll_x = fromX + (endX - fromX) * (progress - align) / (duration - align);
    gRoomControls.scroll_y = fromY + (endY - fromY) * (progress - align) / (duration - align);
    return 1;
}

static u16 Port_WidescreenScrollTile(const u16* map, int col, int row) {
    if (col >= 0 && col < gRoomControls.width / 8 && col < 128 && row >= 0 && row < gRoomControls.height / 8 &&
        row < 128)
        return map[row * 128 + col];
    const int x = col + gRoomControls.origin_x / 8 - sWsScroll.tileX;
    const int y = row + gRoomControls.origin_y / 8 - sWsScroll.tileY;
    const u16 kept = x >= 0 && x < sWsScroll.cols && y >= 0 && y < sWsScroll.rows
                         ? sWsScroll.tiles[map == gMapDataTopSpecial][y][x]
                         : 0;
#ifdef TMC_3DS
    /* P4.9: what neither room holds (the outgoing room's snapshot has nothing there) comes from the rooms
     * around the incoming one, so the scroll shows no black where a neighbour exists. */
    if (kept == 0)
        return Port_NeighbourRooms_Tile(map == gMapDataTopSpecial, col, row, 0);
#endif
    return kept;
}

static int Port_WidescreenFillScroll(u16* map, u16* buffer) {
    if (!Port_WidescreenScrollActive())
        return 0;
    const int col = ((gRoomControls.scroll_x - gRoomControls.origin_x) >> 4) * 2;
    const int row = ((gRoomControls.scroll_y - gRoomControls.origin_y) >> 4) * 2 - 1;
    for (int y = 0; y < 23; ++y)
        for (int x = 0; x < 32; ++x)
            buffer[y * 32 + x] = Port_WidescreenScrollTile(map, col + x, row + y);
    return 1;
}

/* ---- Runtime widescreen gate --------------------------------------------
 * `--widescreen_width=N` only reserves a wider framebuffer. True widescreen
 * is still WIP, so a persisted runtime option decides whether gameplay uses
 * the wider camera/reveal. Disabled, non-gameplay, one-screen (<=240px)
 * rooms, and rooms whose painted content can't fill the room-capped view all
 * fall back to a native 240x160 frame (presented pillarboxed, never
 * stretched); rooms between 240px and the window target run true-wide at
 * their full room width. The camera/culling helpers below report 240 in the
 * fallback state so the engine remains GBA-clean. */
/* ---- Per-room content width (issue: wide room, narrow content) ----------
 * gRoomControls.width can exceed the actual painted map (e.g. a 480-px room
 * whose floor ends ~270 px in). Widescreen then parks void columns on screen
 * permanently (a GBA player only saw them transiently at the right edge).
 * Scan both map buffers for the rightmost non-empty tile column inside the
 * room rect; if that content can't fill the wide view even with the camera
 * pinned left, fall back to a native 240 frame (presented pillarboxed) —
 * which shows exactly what GBA showed. Ratcheted per room (only grows) so
 * late-streaming map data can't flip the mode back and forth mid-room. */
static u32 sWsContentKey = 0xffffffffu;
static s32 sWsContentPx = 0;
static s32 sWsContentHeightPx = 0;
static volatile u32 sWsExperimentalSequence;
static volatile u32 sWsExperimentalRoomKey = 0xffffffffu;
static volatile int sWsExperimentalMode = PORT_3DS_FULL_VIEW_FALLBACK;
static u32 sWsExperimentalCameraRoomKey = 0xffffffffu;
static int sWsExperimentalCameraSnapArmed = 1;
static u32 sWsGameplayRoomKey = 0xffffffffu;
static int sWsRoomLoadTicks; /* UpdateShadows ticks since a door changed the room key, counting down */
static int sWsRoomLoadGraceTicks; /* past sWsRoomLoadTicks: the hold may go on while Full View is still latching */
/* A door's arrival with the camera still frozen (no camera_target): Link coming out of the stairs. Cleared when the
 * game gives the camera a target, or the room changes again. */
static int sWsArrivalFrozenCamera;
/* A save state was just loaded: the room key is forgotten (Port_Widescreen_ResetAfterStateLoad), and a state saved
 * while the camera was frozen (Link still walking in, no camera_target) is an arrival like a door's, not a scroll. */
static int sWsStateLoaded;
static int sWsFadeRecent;    /* ticks left since a fade was last on: a door, not a border scroll */
static int sWsChangeRoomTicks; /* UpdateShadows ticks spent in GAMEMAIN_CHANGEROOM so far */
static Port3DSFullViewMode sWsGameplayMode = PORT_3DS_FULL_VIEW_FALLBACK;
static u32 sWsProducerPendingRoomKey = 0xffffffffu;
static Port3DSFullViewMode sWsProducerPendingMode = PORT_3DS_FULL_VIEW_FALLBACK;
static PortWidescreenBannerState sWsEnterRoomBanner;

/* UpdateDisplayControls publishes shadows and geometry as one generation.
 * The 3DS presenter normally consumes it on the same thread, but use a small
 * 32-bit seqlock so diagnostics or future render scheduling cannot observe a
 * new room key paired with the previous room's 400x240 mode. */
static void Port_WidescreenPublish3DSView(u32 roomKey, Port3DSFullViewMode mode) {
    __atomic_fetch_add(&sWsExperimentalSequence, 1u, __ATOMIC_ACQ_REL);
    __atomic_store_n(&sWsExperimentalRoomKey, roomKey, __ATOMIC_RELAXED);
    __atomic_store_n(&sWsExperimentalMode, (int)mode, __ATOMIC_RELAXED);
    __atomic_fetch_add(&sWsExperimentalSequence, 1u, __ATOMIC_RELEASE);
}

static int Port_WidescreenRead3DSView(u32* roomKey, Port3DSFullViewMode* mode) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const u32 before = __atomic_load_n(&sWsExperimentalSequence, __ATOMIC_ACQUIRE);
        if ((before & 1u) != 0u) continue;
        const u32 key = __atomic_load_n(&sWsExperimentalRoomKey, __ATOMIC_RELAXED);
        const int value = __atomic_load_n(&sWsExperimentalMode, __ATOMIC_RELAXED);
        const u32 after = __atomic_load_n(&sWsExperimentalSequence, __ATOMIC_ACQUIRE);
        if (before == after && (after & 1u) == 0u) {
            if (roomKey != NULL) *roomKey = key;
            if (mode != NULL) *mode = (Port3DSFullViewMode)value;
            return 1;
        }
    }
    return 0;
}

/* The Minish portal's falling cutscene (SUBTASK_PORTALCUTSCENE) in Full View: its
 * 256x320 picture (BG1) centred in a 400x240 view, the game's scroll set for it
 * (src/enterPortalSubtask.c), BG1 repeated across the view and its wrapped
 * columns covered by the presenter's side strips (Port_Widescreen_CutsceneStrips).
 * Published under its own key, so no static is added here: these sit among the
 * game's globals, and growing them breaks states saved by other builds (#19). */
#define WS_CUTSCENE_VIEW_KEY 0xC0715CE5u
#define WS_TITLE_VIEW_KEY 0x717E5CE5u
#define WS_FILESELECT_VIEW_KEY 0xF11E5CE5u
#define WS_PAUSE_VIEW_KEY 0xBA05E5CEu

/* The title screen in Full View (Western layout: the forest on BG1): drawn at 400x240 with the
 * GBA's screen centred (src/title.c moves the light rays, sword and sprites), the forest from the
 * extended picture (platform/3ds/source/scene_pictures_3ds.c) as BG1, the light rays repeated
 * across the view and from its top. Only with a picture to draw the forest from. */
int Port_Widescreen_TitleWide(void) {
#ifdef TMC_3DS
    extern bool ScenePictures_TitleAvailable(void);
    /* TitleTask (src/title.c) runs gUI.lastState's handler in state 1: 1 the title, 2 its exit
     * after START. In state 2 the screen the game left fades out while lastState already names the
     * next one: the title fading out stays wide (to white after START, or back to the logos when
     * its timer runs out), the logos fading into the title do not. Told apart by the layers on. */
    if (!Port_Config_3DSFullViewComboEnabled() || gMain.task != TASK_TITLE || gSaveHeader->language == 0 ||
        !ScenePictures_TitleAvailable())
        return 0;
    if (gMain.state == GAMETASK_MAIN) {
        /* The screen fading out still has its layers (DispReset comes after the fade): the title's
         * are mode 1 with the forest on BG1 at screen base 30 (src/title.c); the logos' are not. */
        const unsigned dispcnt = gScreen.lcd.displayControl;
        return (dispcnt & 7) == 1 && (dispcnt & DISPCNT_BG1_ON) &&
               gScreen.bg1.control == (BGCNT_SCREENBASE(30) | BGCNT_PRIORITY(3));
    }
    return gMain.state == GAMETASK_INIT && (gUI.lastState == 1 || gUI.lastState == 2);
#else
    return 0;
#endif
}

int Port_Widescreen_TitleViewLive(void) {
    u32 key;
    Port3DSFullViewMode mode;
    return Port_WidescreenRead3DSView(&key, &mode) && key == WS_TITLE_VIEW_KEY &&
           mode == PORT_3DS_FULL_VIEW_OUTDOOR_1X;
}

int Port_Widescreen_TitleRays(void) {
#ifdef TMC_3DS
    extern int TitleRays3DS_Update(uint8_t* vram);
    return TitleRays3DS_Update(gVram);
#else
    return 0;
#endif
}

/* Which scenes the stereoscopic 3D gives a layer a depth in (platform/3ds/source/stereo_3ds.c):
 * a bit each of the PORT_STEREO_SCENE_* of port_widescreen.h, from the layers the game has on. */
int Port_Stereo_Scenes(void) {
#ifdef TMC_3DS
    int scenes = 0;
    const unsigned dispcnt = gScreen.lcd.displayControl;
    /* The title (its forest on BG1 at screen base 30): BG0 is the light rays, in any view. Read from the
     * registers the shown frame was drawn from, as Port_Widescreen_TitleForestShown does, not from
     * gScreen: the game sets those up in state 0 and the view code rewrites BG0 for the rays. */
    if (gMain.task == TASK_TITLE) {
        const unsigned io = gIoMem[REG_OFFSET_DISPCNT] | (gIoMem[REG_OFFSET_DISPCNT + 1] << 8);
        const unsigned bg1 = gIoMem[REG_OFFSET_BG1CNT] | (gIoMem[REG_OFFSET_BG1CNT + 1] << 8);
        if ((io & DISPCNT_BG0_ON) && (io & DISPCNT_BG1_ON) && bg1 == (BGCNT_SCREENBASE(30) | BGCNT_PRIORITY(3)))
            scenes |= PORT_STEREO_SCENE_TITLE_RAYS;
        /* The Nintendo and Capcom logos (HandleNintendoCapcomLogos): mode 0 with only BG2 (and the empty
         * BG0) on. */
        else if ((io & 7) == 0 && (io & DISPCNT_BG2_ON) && !(io & (DISPCNT_BG1_ON | DISPCNT_BG3_ON)))
            scenes |= PORT_STEREO_SCENE_LOGOS;
    }
    /* The overlays over the world on BG3 at priority 0, alpha-blended as the first target: the fog and the sun rays
     * of the Minish Woods and the barrel house (LightRayManager, BLDCNT 0x3648), the steam and the Cave of Flames'
     * heat haze (0x3e48, the second targets BG3 too). All of them are weather, in front with the HUD (owner,
     * 2026-11-19: "niebla, lluvia y todo lo ambiental"). */
    if ((dispcnt & DISPCNT_BG3_ON) && gScreen.bg3.control == 0x1e04 && (gScreen.controls.layerFXControl & 0xff) == 0x48)
        scenes |= PORT_STEREO_SCENE_LIGHT_RAYS;
    /* The leaves of the Minish Paths corridors (area 0x11, BG1 at priority 1 over the shadows of
     * BG3 and the ground of BG2), vertical (0x1e49) and horizontal (0x1e09). */
    if (gRoomControls.area == 0x11 && (dispcnt & DISPCNT_BG1_ON) && (gScreen.bg1.control & 0x1f03) == 0x1e01)
        scenes |= PORT_STEREO_SCENE_PATH_LEAVES;
    /* The file select (src/fileselect.c): its cave is BG3 at screen base 30, character base 2, priority 3;
     * the banner, the slots and their texts are its sprites, the preview's panel BG2 at priority 2. */
    if (gMain.task == TASK_FILE_SELECT) {
        const unsigned io = gIoMem[REG_OFFSET_DISPCNT] | (gIoMem[REG_OFFSET_DISPCNT + 1] << 8);
        const unsigned bg3 = gIoMem[REG_OFFSET_BG3CNT] | (gIoMem[REG_OFFSET_BG3CNT + 1] << 8);
        if (!(io & DISPCNT_FORCED_BLANK) && (io & DISPCNT_BG3_ON) &&
            (bg3 & 0xdf0f) == (BGCNT_SCREENBASE(30) | BGCNT_CHARBASE(2) | BGCNT_PRIORITY(3)))
            scenes |= PORT_STEREO_SCENE_FILE_SELECT;
    }
    /* The map screens (the pause menu's map, SUBTASK_MAPHINT, SUBTASK_LOCALMAPHINT): the covered parts are
     * sprites in OBJ-window mode (sub_080A6498) and every frame that draws them also sets WINOUT to 0x3d3f;
     * their markers and the fog are on one plane, apart from the HUD's depth that sprites at priority 0 get. */
    if (gMain.task == TASK_GAME && gMain.substate == GAMEMAIN_SUBTASK && gScreen.controls.windowOutsideControl == 0x3d3f)
        scenes |= PORT_STEREO_SCENE_MAP_HINT;
    /* The game over screen (src/gameOverTask.c): the stone panel and its choices are BGs, the GAME OVER
     * letters are sprites. */
    if (gMain.task == TASK_GAMEOVER) scenes |= PORT_STEREO_SCENE_GAME_OVER;
    /* Mount Crenel's storm (WeatherChangeManager): the rain is BG1 at screen base 29, priority 1, in place of the
     * top map layer. */
    if (gRoomControls.area == AREA_MT_CRENEL && gMapTop.bgSettings == NULL && (dispcnt & DISPCNT_BG1_ON) &&
        (gScreen.bg1.control & 0x1f03) == (BGCNT_SCREENBASE(29) | BGCNT_PRIORITY(1)))
        scenes |= PORT_STEREO_SCENE_RAIN;
    /* Playing: the world itself (stereo_world_* in tmc3ds.ini). The layer over Link and his sprites stand out
     * of the ground, which stays on the screen plane. */
    if (gMain.task == TASK_GAME && gMain.substate == GAMEMAIN_UPDATE) scenes |= PORT_STEREO_SCENE_WORLD;
    return scenes;
#else
    return 0;
#endif
}

int Port_Widescreen_ContentRects(PortBlurRect* rects, int max) {
#ifdef TMC_3DS
    static PortBlurRect sLast[8];
    static int sLastCount;
    if (max <= 0 || gMain.task != TASK_GAME || Port_Widescreen_3DSViewMode() != PORT_3DS_FULL_VIEW_OUTDOOR_1X) {
        sLastCount = 0;
        return 0;
    }
    /* The tick a room changes, the frame still shows the old room: the border keeps what it had. */
    if (Port_NeighbourRooms_RoomJustChanged() && sLastCount > 0) {
        const int n = sLastCount < max ? sLastCount : max;
        for (int i = 0; i < n; ++i) rects[i] = sLast[i];
        return n;
    }
    /* During a border scroll the incoming room is the current one and the outgoing one is its neighbour: both
     * rectangles are where the frame's registers put them, so the border follows. */
    const int roomW = (int)gRoomControls.width, roomH = (int)gRoomControls.height;
    if (roomW <= 0 || roomH <= 0 || (roomW >= 400 && roomH >= 240)) {
        sLastCount = 0;
        return 0;
    }
    /* Where the room really lies in the view (a room with neighbours beside it rests off centre, a room
     * wider or taller than the view is cut by it), the part of it that shows. */
    /* The frame on screen was drawn with the background offsets of the registers, which can be a tick behind
     * scroll_x/y while the camera moves: the room's edge is where those put it (the low four bits of the offset
     * are the scroll's, plus 8 on y: the same split Port_WidescreenShadow_Populate reads). */
    const int scrollX = (int)gRoomControls.scroll_x, scrollY = (int)gRoomControls.scroll_y;
    const int seenX = (int)(gIoMem[0x18] | (gIoMem[0x19] << 8)) & 0xf;
    const int seenY = ((int)(gIoMem[0x1A] | (gIoMem[0x1B] << 8)) - 8) & 0xf;
    const int frameScrollX = scrollX + ((seenX - ((scrollX - (int)gRoomControls.origin_x) & 0xf) + 8) & 0xf) - 8;
    const int frameScrollY = scrollY + ((seenY - ((scrollY - (int)gRoomControls.origin_y) & 0xf) + 8) & 0xf) - 8;
    const int roomX = (int)gRoomControls.origin_x - frameScrollX;
    const int roomY = (int)gRoomControls.origin_y - frameScrollY;
    const int x0 = roomX > 0 ? roomX : 0, x1 = roomX + roomW < 400 ? roomX + roomW : 400;
    const int y0 = roomY > 0 ? roomY : 0, y1 = roomY + roomH < 240 ? roomY + roomH : 240;
    if (x1 <= x0 || y1 <= y0 || (x0 == 0 && x1 == 400 && y0 == 0 && y1 == 240)) return 0;
    rects[0] = (PortBlurRect){ x0, y0, x1 - x0, y1 - y0 };
    const int count = 1 + Port_NeighbourRooms_DrawnRects(rects + 1, max - 1, frameScrollX, frameScrollY);
    sLastCount = count < 8 ? count : 8;
    for (int i = 0; i < sLastCount; ++i) sLast[i] = rects[i];
    return count;
#else
    (void)rects; (void)max;
    return 0;
#endif
}

int Port_Widescreen_PortalCutsceneWide(void) {
#ifdef TMC_3DS
    return Port_Config_3DSFullViewComboEnabled() && gMain.task == TASK_GAME &&
           gMain.substate == GAMEMAIN_SUBTASK && gUI.lastState == SUBTASK_PORTALCUTSCENE &&
           gArea.filler3[0] != 0;
#else
    return 0;
#endif
}

int Port_Widescreen_CutsceneViewLive(void) {
    u32 key;
    Port3DSFullViewMode mode;
    return Port_WidescreenRead3DSView(&key, &mode) && key == WS_CUTSCENE_VIEW_KEY &&
           mode == PORT_3DS_FULL_VIEW_OUTDOOR_1X;
}

/* Screens of one plain colour around the middle, drawn 240x160 in Full View: the presenter
 * stretches the frame's edges over the rest of the 400x240 screen (platform_gpu_3ds.c,
 * DrawFrameEdges) instead of black bars around them. The Nintendo/Capcom logos (title task,
 * lastState 0, its handler running in state 1) and the blank frames of the title and file select
 * (no layer on: one colour), and what the title task fades out at 240x160 (state 2). -1 there
 * when the title itself may be 240x160 (Japanese layout, no forest picture): the presenter keeps
 * its last answer, as that screen is still the one shown. */
int Port_Widescreen_StretchEdges(void) {
#ifdef TMC_3DS
    if (!Port_Config_3DSFullViewComboEnabled()) return 0;
    if (gMain.task != TASK_TITLE && gMain.task != TASK_FILE_SELECT) return 0;
    const unsigned dispcnt = gIoMem[REG_OFFSET_DISPCNT] | (gIoMem[REG_OFFSET_DISPCNT + 1] << 8);
    if ((dispcnt & DISPCNT_FORCED_BLANK) || !(dispcnt & 0x1F00)) return 1; /* first: a reset fades a blank one */
    if (gMain.task == TASK_TITLE && gMain.state == GAMETASK_MAIN) {
        /* With the Western title's picture the title is never drawn 240x160, so what fades here at
         * 240x160 is plain: the logos, or whatever a reset or the boot left, faded to white. */
        extern bool ScenePictures_TitleAvailable(void);
        return gSaveHeader->language != 0 && ScenePictures_TitleAvailable() ? 1 : -1;
    }
    return gMain.task == TASK_TITLE && gMain.state == GAMETASK_INIT && gUI.lastState == 0;
#else
    return 0;
#endif
}

/* The file select screen (src/fileselect.c, HandleFileScreenEnter): its cave is BG3 at screen
 * base 30, character base 2, priority 3, never scrolled; read from the registers the shown frame
 * was drawn from. */
int Port_Widescreen_FileSelectCaveShown(void) {
#ifdef TMC_3DS
    if (!Port_Config_3DSFullViewComboEnabled() || gMain.task != TASK_FILE_SELECT) return 0;
    const unsigned dispcnt = gIoMem[REG_OFFSET_DISPCNT] | (gIoMem[REG_OFFSET_DISPCNT + 1] << 8);
    const unsigned bg3cnt = gIoMem[REG_OFFSET_BG3CNT] | (gIoMem[REG_OFFSET_BG3CNT + 1] << 8);
    return !(dispcnt & DISPCNT_FORCED_BLANK) && (dispcnt & DISPCNT_BG3_ON) &&
           bg3cnt == (BGCNT_SCREENBASE(30) | BGCNT_CHARBASE(2) | BGCNT_PRIORITY(3));
#else
    return 0;
#endif
}

/* The pause menu (src/menu/pauseMenu.c): its background of Ezlo caps is BG3 at screen base 30, character
 * base 2, priority 3 (0x1e0b), unscrolled; read from the registers the shown frame was drawn from, while the
 * menu runs (Subtask_PauseMenu tells the soft slots each tick). */
int Port_Widescreen_PauseBackgroundShown(void) {
#ifdef TMC_3DS
    extern bool Port_SoftSlots_IsPauseActive(void);
    if (!Port_Config_3DSFullViewComboEnabled() || gMain.task != TASK_GAME || !Port_SoftSlots_IsPauseActive())
        return 0;
    const unsigned dispcnt = gIoMem[REG_OFFSET_DISPCNT] | (gIoMem[REG_OFFSET_DISPCNT + 1] << 8);
    const unsigned bg3cnt = gIoMem[REG_OFFSET_BG3CNT] | (gIoMem[REG_OFFSET_BG3CNT + 1] << 8);
    const unsigned hofs = (gIoMem[REG_OFFSET_BG3HOFS] | (gIoMem[REG_OFFSET_BG3HOFS + 1] << 8)) & 0x1ff;
    const unsigned vofs = (gIoMem[REG_OFFSET_BG3VOFS] | (gIoMem[REG_OFFSET_BG3VOFS + 1] << 8)) & 0x1ff;
    return !(dispcnt & DISPCNT_FORCED_BLANK) && (dispcnt & DISPCNT_BG3_ON) && hofs == 0 && vofs == 0 &&
           bg3cnt == (BGCNT_SCREENBASE(30) | BGCNT_CHARBASE(2) | BGCNT_PRIORITY(3));
#else
    return 0;
#endif
}

/* The pause menu at 400x240 (Port_Widescreen_PauseWide in port_widescreen.h): with the GPU renderer only, while the
 * game shows its background (gScreen, a tick ahead of the
 * registers Port_Widescreen_PauseBackgroundShown reads). */
int Port_Widescreen_PauseWide(void) {
#ifdef TMC_3DS
    extern bool Port_SoftSlots_IsPauseActive(void);
    if (Port_Pause3DS_Active() || gMain.task != TASK_GAME || !Port_Config_3DSFullViewComboEnabled() ||
        !Port_Config_GpuRenderer() || !Port_SoftSlots_IsPauseActive())
        return 0;
    const unsigned dispcnt = gScreen.lcd.displayControl;
    return !(dispcnt & DISPCNT_FORCED_BLANK) && (dispcnt & DISPCNT_BG3_ON) && gScreen.bg3.xOffset == 0 &&
           gScreen.bg3.yOffset == 0 &&
           gScreen.bg3.control == (BGCNT_SCREENBASE(30) | BGCNT_CHARBASE(2) | BGCNT_PRIORITY(3));
#else
    return 0;
#endif
}

int Port_Widescreen_PauseViewLive(void) {
    u32 key;
    Port3DSFullViewMode mode;
    return Port_WidescreenRead3DSView(&key, &mode) && key == WS_PAUSE_VIEW_KEY &&
           mode == PORT_3DS_FULL_VIEW_OUTDOOR_1X;
}

/* The file select at 400x240 (Port_Widescreen_FileSelectWide in port_widescreen.h): with the GPU
 * renderer only (the software one would repeat BG0-BG2 across the view) and a cave picture to draw
 * around the middle; while the game shows the cave (HandleFileScreenEnter's BG3), so the blank
 * frames before it stay 240x160 with their edges stretched. */
int Port_Widescreen_FileSelectWide(void) {
#ifdef TMC_3DS
    extern bool ScenePictures_CaveAvailable(void);
    if (gMain.task != TASK_FILE_SELECT || !Port_Config_3DSFullViewComboEnabled() || !Port_Config_GpuRenderer() ||
        !ScenePictures_CaveAvailable())
        return 0;
    const unsigned dispcnt = gScreen.lcd.displayControl;
    return !(dispcnt & DISPCNT_FORCED_BLANK) && (dispcnt & DISPCNT_BG3_ON) &&
           gScreen.bg3.control == (BGCNT_SCREENBASE(30) | BGCNT_CHARBASE(2) | BGCNT_PRIORITY(3));
#else
    return 0;
#endif
}

int Port_Widescreen_FileSelectViewLive(void) {
    u32 key;
    Port3DSFullViewMode mode;
    return Port_WidescreenRead3DSView(&key, &mode) && key == WS_FILESELECT_VIEW_KEY &&
           mode == PORT_3DS_FULL_VIEW_OUTDOOR_1X;
}

/* The title screen's forest is on screen (src/title.c: BG1, or BG3 in the Japanese layout, at
 * screen base 30, priority 3), read from the registers the shown frame was drawn from: the
 * presenter then extends it around the 240x160 screen (docs/wide-scene-extension.md). */
int Port_Widescreen_TitleForestShown(void) {
#ifdef TMC_3DS
    if (!Port_Config_3DSFullViewComboEnabled() || gMain.task != TASK_TITLE) return 0;
    const unsigned dispcnt = gIoMem[REG_OFFSET_DISPCNT] | (gIoMem[REG_OFFSET_DISPCNT + 1] << 8);
    const unsigned bg1 = gIoMem[REG_OFFSET_BG1CNT] | (gIoMem[REG_OFFSET_BG1CNT + 1] << 8);
    const unsigned bg3 = gIoMem[REG_OFFSET_BG3CNT] | (gIoMem[REG_OFFSET_BG3CNT + 1] << 8);
    const unsigned forest = BGCNT_SCREENBASE(30) | BGCNT_PRIORITY(3);
    return ((dispcnt & DISPCNT_BG1_ON) && bg1 == forest) || ((dispcnt & DISPCNT_BG3_ON) && bg3 == forest);
#else
    return 0;
#endif
}

int Port_Widescreen_CutsceneStrips(int* pictureX, int* pictureY, int* portal) {
    if (!Port_Widescreen_CutsceneViewLive()) return 0;
    /* The picture Subtask_PortalCutscene_0 loaded (src/enterPortalSubtask.c): PT_2 outside area 2
     * and anything past the table's 4 entries take the dungeon's. Fixed for the whole scene. */
    if (portal) {
        u32 id = gArea.portal_type;
        if ((id == PT_2 && gUI.roomControls.area != 2) || id > 3) id = PT_DUNGEON;
        *portal = (int)id;
    }
    /* Where the picture's top-left lands on screen: BG1's offsets in the registers the shown
     * frame was drawn from (gScreen is already the next tick's, a frame ahead: the strips
     * slid against the picture). 9-bit, so 0x1B0 is -80. */
    const int hofs = (gIoMem[REG_OFFSET_BG1HOFS] | (gIoMem[REG_OFFSET_BG1HOFS + 1] << 8)) & 0x1ff;
    const int vofs = (gIoMem[REG_OFFSET_BG1VOFS] | (gIoMem[REG_OFFSET_BG1VOFS + 1] << 8)) & 0x1ff;
    if (pictureX) *pictureX = -(hofs >= 256 ? hofs - 512 : hofs);
    if (pictureY) *pictureY = -(vofs >= 256 ? vofs - 512 : vofs);
    return 1;
}

void Port_Widescreen_SetEnterRoomBannerActive(int active) {
    Port_WidescreenBannerState_Set(&sWsEnterRoomBanner, active, gRoomControls.area, gRoomControls.room);
}

/* Key identifying the room the content cache belongs to. Checked by
 * FallbackNative so a stale cache (previous room) is never consulted —
 * InitializeCamera runs at room entry BEFORE the next vblank rescan. */
static u32 Port_WidescreenRoomKey(void) {
    return ((u32)gRoomControls.area << 24) | ((u32)gRoomControls.room << 16) |
           ((u32)(u16)gRoomControls.origin_x ^ ((u32)(u16)gRoomControls.origin_y << 1));
}

static s32 Port_WidescreenScanContentPx(void) {
    enum { kMapStride = 128, kMapRows = 128 };
    s32 tiles_w = (s32)gRoomControls.width / 8;
    s32 tiles_h = (s32)gRoomControls.height / 8;
    s32 c, r;
    if (tiles_w > kMapStride)
        tiles_w = kMapStride;
    if (tiles_h > kMapRows)
        tiles_h = kMapRows;
    for (c = tiles_w - 1; c >= 0; c--) {
        const u16* bot = gMapDataBottomSpecial + c;
        const u16* top = gMapDataTopSpecial + c;
        for (r = 0; r < tiles_h; r++) {
            if (bot[(size_t)r * kMapStride] != 0 || top[(size_t)r * kMapStride] != 0) {
                return (c + 1) * 8;
            }
        }
    }
    return 0;
}

static s32 Port_WidescreenScanContentHeightPx(void) {
    enum { kMapStride = 128, kMapRows = 128 };
    s32 tiles_w = (s32)gRoomControls.width / 8;
    s32 tiles_h = (s32)gRoomControls.height / 8;
    s32 c, r;
    if (tiles_w > kMapStride)
        tiles_w = kMapStride;
    if (tiles_h > kMapRows)
        tiles_h = kMapRows;
    for (r = tiles_h - 1; r >= 0; r--) {
        const u16* bot = gMapDataBottomSpecial + (size_t)r * kMapStride;
        const u16* top = gMapDataTopSpecial + (size_t)r * kMapStride;
        for (c = 0; c < tiles_w; c++) {
            if (bot[c] != 0 || top[c] != 0) {
                return (r + 1) * 8;
            }
        }
    }
    return 0;
}

static int Port_WidescreenFixedCanvasSubtask(void) {
    if (gMain.task != TASK_GAME) {
        return 0;
    }
    /* The Deepwood rolling-barrel state renders a fixed 240x160 affine
     * canvas and never runs the normal scrolling path. Cropping it as a
     * 200x120 interior desynchronizes Link from BG1/BG2 and the HDMA curve. */
    if (gMain.substate == GAMEMAIN_BARRELUPDATE) {
        return 1;
    }
    /* ... and its way out: RollingBarrelManager's room-exit callback runs when the door fade starts, so the
     * substate has already left BARRELUPDATE while the barrel's affine picture (mode 1, still the room's
     * HBlank DMA) fades out. Drawn as a normal wide room, the maps the game is clearing at that point came
     * out as noise over the whole 400x240; as the fixed canvas it is the plain fade to white of the
     * original aspect. */
    if (gRoomControls.area == AREA_DEEPWOOD_SHRINE && (gScreen.lcd.displayControl & 7u) == 1u &&
        gFadeControl.active != 0) {
        return 1;
    }
    if (gMain.substate != GAMEMAIN_SUBTASK) {
        return 0;
    }
    /* The new game's opening shot of Link's house (AuxCutscene 1, src/subtask/
     * subtaskAuxCutscene.c: Hyrule Field room 1, a static camera on the open field) is
     * the world drawn as in play, not a fixed picture: it takes the wide view too. So does
     * AuxCutscene 5, Zelda leaving Link's house through its entrance room (HOUSE_INTERIORS_2
     * room 0x10) while the game waits in the Smith's room next door: drawn at 240x160 it
     * lost the neighbour rooms the play around it shows. AuxCutscene 0 too: the Minish Woods seen from outside the
     * Deepwood Shrine when its big door opens (area 0x48 room 0x17), the woods as in play. Their fade out
     * (Subtask_FadeOut, nextToLoad 3) is still that world: as the fixed canvas it dropped to 240x160 for its last
     * frames before the game came back. */
    if (gUI.lastState == SUBTASK_AUXCUTSCENE && (gUI.field_0x3 == 0 || gUI.field_0x3 == 1 || gUI.field_0x3 == 5) &&
        (gUI.nextToLoad == 2 || gUI.nextToLoad == 3)) {
        return 0;
    }
    /* The world event shown after fusing two kinstones (src/subtask/subtaskWorldEvent.c: the place where something
     * changed, a camera on the map as in play; the PC port already re-clamps its camera for the wide view) is the
     * world drawn as in play too, like the aux cutscenes above, fade out included. */
    if (gUI.lastState == SUBTASK_WORLDEVENT && (gUI.nextToLoad == 2 || gUI.nextToLoad == 3)) {
        return 0;
    }
    /* Pause/inventory replace the world with a fixed GBA canvas. State 3 is
     * still fixed while its fade is active; fail closed until gameplay is
     * visibly restored. */
    return gUI.nextToLoad == 1 || gUI.nextToLoad == 2 ||
           (gUI.nextToLoad == 3 && gFadeControl.active != 0);
}

static int Port_WidescreenFullViewUiOverlayActive(void) {
    /* The location-name banner is not here: like a dialogue it is a band of BG0 that
     * Port_Widescreen_UpdateShadows centres with ws_msg_shift at any view width. Counting it
     * as an overlay dropped Full View to E2 for the whole banner (about four seconds) and the
     * camera then snapped to the 400-wide rest position when it ended. */
    /* The iris fade and a dark room's light are drawn across the whole view
     * (Port_Widescreen_Win0Lines). */
    if (Port_WidescreenIrisLive())
        return 0;
    return Port3DSFullViewPolicy_NativeWindowActive(gScreen.lcd.displayControl);
}

/* A room change whose data is being replaced. The fades are not part of it:
 * on the way out the old room is still drawn and fading (the player walks
 * through the door in GAMEMAIN_UPDATE), so Full View stays until the screen is
 * dark and the load begins; on the way in the new room is already loaded.
 * What mixes two map generations is the load itself (INITROOM, CHANGEROOM,
 * the reload flags) and the border scroll. */
static int Port_WidescreenRoomTransitionActive(void) {
    /* CHANGEROOM lasts the whole fade-in of an area change (GameMain_ChangeRoom returns while
     * the fade is on), but the room is loaded by its first ticks. Counting all of it kept Full
     * View off until the fade-in had ended, so the top hold swallowed the fade-in and the field
     * popped in at once. */
    enum { CHANGEROOM_LOAD_TICKS = 2 };
    if (gMain.substate == GAMEMAIN_INITROOM) return 1;
    /* A border scroll taken in Full View: the outgoing room's tiles come from
     * the scroll snapshot, so the two map generations never mix. */
    if (Port_WidescreenFullViewScrollActive()) return 0;
    if (gMain.substate == GAMEMAIN_CHANGEROOM && sWsChangeRoomTicks < CHANGEROOM_LOAD_TICKS) return 1;
    if (gMain.substate == GAMEMAIN_CHANGEAREA && gFadeControl.active == 0) return 1;
    return Port3DSFullViewPolicy_RoomTransitionActive(0, 0, gRoomControls.reload_flags, gRoomControls.scrollAction);
}

static Port3DSFullViewMode Port_WidescreenGameplayMode(void);
static int Port_WidescreenCameraRestXForMode(int target_x, Port3DSFullViewMode mode);
static int Port_WidescreenCameraRestYForMode(int target_y, Port3DSFullViewMode mode);

/* The game freezes the camera by clearing its target (a Minish portal's shrink and
 * growth, some cutscenes): the view is meant to stay where it is. Full View already
 * on keeps it while the frozen 400x240 window lies inside the room; dropping to
 * 266x160 there kept the 400 view's top-left corner and left Link out of sight.
 * Without a target there is nothing to centre a new 400 view on, so it is never
 * entered that way. */
static int Port_WidescreenFrozenCameraKeepsFullView(void) {
    /* Through a door the game can freeze the camera while Link walks in (out of the stairs, PL_SPAWN_STEP_IN): the
     * room is entered in Full View from the start, the camera snapped around Link
     * (Port_WidescreenSnapGameplayCamera), instead of waiting ~110 frames at 266x160 for the target. */
    if (Port_WidescreenGameplayMode() == PORT_3DS_FULL_VIEW_FALLBACK)
        return sWsArrivalFrozenCamera && gPlayerEntity.base.kind == PLAYER;
    if (Port_WidescreenGameplayMode() != PORT_3DS_FULL_VIEW_OUTDOOR_1X) return 0;
    return Port_WidescreenCameraRestXForMode(gRoomControls.scroll_x + 200, PORT_3DS_FULL_VIEW_OUTDOOR_1X) ==
               gRoomControls.scroll_x &&
           Port_WidescreenCameraRestYForMode(gRoomControls.scroll_y + 120, PORT_3DS_FULL_VIEW_OUTDOOR_1X) ==
               gRoomControls.scroll_y;
}

static Port3DSFullViewInputs Port_WidescreenFullViewInputs(void) {
    Port3DSFullViewInputs inputs;
    memset(&inputs, 0, sizeof(inputs));
#if defined(TMC_3DS) && defined(MODE1_GBA_HEIGHT) && (MODE1_GBA_WIDTH >= 400) && (MODE1_GBA_HEIGHT >= 240)
    inputs.isNew3DS = Port_Config_3DSFullViewComboEnabled();
    /* The combo helper already verifies these two persisted selections. Keep
     * them explicit here so the pure policy matrix remains authoritative. */
    inputs.aspectWide = inputs.isNew3DS;
    inputs.pixelPerfect = inputs.isNew3DS;
#endif
    inputs.gameTask = gMain.task == TASK_GAME;
    inputs.fixedCanvas = Port_WidescreenFixedCanvasSubtask();
    inputs.uiOverlay = Port_WidescreenFullViewUiOverlayActive();
    inputs.transitioning = Port_WidescreenRoomTransitionActive();
    /* A Minish portal's closing circle runs with every entity erased
     * (GameMain_MinishPortal), the room still drawn behind it. */
    inputs.playerValid = (gPlayerEntity.base.kind == PLAYER || gMain.substate == GAMEMAIN_MINISHPORTAL) &&
                         (gRoomControls.camera_target != NULL || Port_WidescreenFrozenCameraKeepsFullView());
    /* No scene is excluded: Mt. Crenel's Cave of Flames entrance (06/02) was, for its static BG3
     * landscape (32x32 tiles, parallax), and fell back to E2 mid-transition and stayed there.
     * A scenario shows what is there (owner, 2026-11-19): BG3 wraps across 400 as the GBA's own
     * map wraps, like the Mountain Top's (06/00), which never was excluded. */
    inputs.unsupportedScene = 0;
    if (gRoomControls.area <= AREA_98) {
        inputs.areaIsExterior = Port3DSFullViewPolicy_AreaIsExterior(
            gRoomControls.area, gRoomControls.room, gAreaMetadata[gRoomControls.area].flags);
    }
    inputs.roomWidth = (int)gRoomControls.width;
    inputs.roomHeight = (int)gRoomControls.height;
    if (sWsContentKey == Port_WidescreenRoomKey()) {
        inputs.contentWidth = (int)sWsContentPx;
        inputs.contentHeight = (int)sWsContentHeightPx;
    }
    return inputs;
}

static Port3DSFullViewMode Port_WidescreenDesired3DSViewMode(void) {
#ifdef TMC_3DS
    Port3DSFullViewInputs inputs = Port_WidescreenFullViewInputs();
    return Port3DSFullViewPolicy_Desired(&inputs);
#else
    return PORT_3DS_FULL_VIEW_FALLBACK;
#endif
}

/* ---- True widescreen: window-aspect-driven view width -------------------
 * MODE1_GBA_WIDTH is the framebuffer CAPACITY (hard cap); the presented
 * view width tracks the live window aspect so the frame fills the monitor
 * exactly: 16:9 -> 284 px, 21:9 -> 373 -> cap, 4:3/3:2 -> 240 (authentic).
 * Published once per frame from the present path; rounded to a multiple of
 * 8 so tile/HUD math stays exact. TMC_WS_VIEW_WIDTH=<px> overrides for
 * headless captures (dummy driver windows don't track a real monitor). */
static int sWsWindowW = 0;
static int sWsWindowH = 0;

void Port_Widescreen_SetWindowPixels(int w, int h) {
    sWsWindowW = w;
    sWsWindowH = h;
}

static int Port_WidescreenBaseTargetViewWidth(void) {
    static int env_w = -1;
    int w;
    if (env_w < 0) {
        const char* e = getenv("TMC_WS_VIEW_WIDTH");
        env_w = 0;
        if (e && *e) {
            int v = atoi(e);
            if (v >= 240 && v <= MODE1_GBA_WIDTH)
                env_w = v;
        }
    }
    if (env_w > 0) {
        return env_w;
    }
    if (sWsWindowW <= 0 || sWsWindowH <= 0) {
        return 240;
    }
    /* Exact aspect fit: the width that makes the 160-line frame fill the
     * window (no rounding — every consumer is pixel-based, and exact fit
     * beats up-to-7px side bars). */
    w = (int)(((long long)sWsWindowW * 160) / sWsWindowH);
    if (w < 240)
        w = 240;
    if (w > MODE1_GBA_WIDTH)
        w = MODE1_GBA_WIDTH;
    return w;
}

int Port_Widescreen_TargetViewWidth(void) {
    const Port3DSFullViewMode mode = Port_Widescreen_3DSViewMode();
    if (mode == PORT_3DS_FULL_VIEW_OUTDOOR_1X) {
        return MODE1_GBA_WIDTH < 400 ? MODE1_GBA_WIDTH : 400;
    }
    if (mode == PORT_3DS_FULL_VIEW_INTERIOR_2X) {
        return 200;
    }
    return Port_WidescreenBaseTargetViewWidth();
}

/* Widest view the current room can honestly feed: the window-aspect target
 * capped by the room's own width. Rooms narrower than the window target but
 * wider than 240 (0x100/0x110 rooms — most dungeons and larger interiors)
 * still get true widescreen at their full room width; the presenter
 * pillarboxes the small remainder instead of stretching. */
static s32 Port_WidescreenEffectiveTarget(void) {
    s32 target = Port_Widescreen_TargetViewWidth();
    s32 roomW = (s32)gRoomControls.width;
    if (roomW < target) {
        target = roomW;
    }
    return target;
}

static int Port_WidescreenEstablishedViewWidth(void) {
    int target = Port_WidescreenBaseTargetViewWidth();
    int roomWidth = (int)gRoomControls.width;
    if (!Port_Config_WidescreenEnabled() || target <= 240) {
        return 240;
    }
    if (roomWidth < target) {
        target = roomWidth;
    }
    if (target <= 240) {
        return 240;
    }
    if (sWsContentKey == Port_WidescreenRoomKey() && sWsContentPx > 0 &&
        sWsContentPx < target) {
        return 240;
    }
    return target;
}

int Port_Widescreen_FallbackNative(void) {
    s32 eff;
    if (!Port_Config_WidescreenEnabled()) {
        return 1;
    }
    if (Port_Widescreen_TargetViewWidth() <= 240) {
        return 1; /* window is 3:2/4:3 — native view already fills it */
    }
    eff = Port_WidescreenEffectiveTarget();
    if (eff <= 240) {
        return 1; /* one-screen room (<=240px): no extra world to reveal */
    }
    /* Room wider than its painted content: the map can't fill even the
     * room-capped view with the camera pinned left -> permanent void strip.
     * Render native 240 (GBA-identical framing, presented pillarboxed).
     * 0 = not scanned yet -> trust width. Key mismatch = cache belongs to
     * the PREVIOUS room -> trust width. */
    if (sWsContentKey == Port_WidescreenRoomKey() && sWsContentPx > 0 && sWsContentPx < eff) {
        return 1;
    }
    return 0;
}

Port3DSFullViewMode Port_Widescreen_3DSViewMode(void) {
    Port3DSFullViewMode publishedMode;
    /* Presentation consumes the geometry generation published alongside the
     * current IO/OAM/shadows. Live policy can change during the following
     * game tick; revalidating it here would combine new geometry with the
     * previous generation's already-produced frame. */
    u32 publishedKey;
    if (!Port_WidescreenRead3DSView(&publishedKey, &publishedMode)) return PORT_3DS_FULL_VIEW_FALLBACK;
    if (publishedKey == WS_CUTSCENE_VIEW_KEY || publishedKey == WS_TITLE_VIEW_KEY ||
        publishedKey == WS_FILESELECT_VIEW_KEY || publishedKey == WS_PAUSE_VIEW_KEY)
        return publishedMode; /* no map shadows there */
    if (!Port_Widescreen_ShadowsLive()) return PORT_3DS_FULL_VIEW_FALLBACK;
    return publishedMode;
}

/* After a save state is loaded: the view's latch (which room and which geometry the game
 * produced for) came back with the game's globals, but the shadows and the PPU's flags it
 * stands for belong to the live session and were not saved. Take it back to "a room just
 * loaded" so the geometry is staged again from the policy, as after a door. */
void Port_Widescreen_ResetAfterStateLoad(void) {
    sWsGameplayRoomKey = 0xffffffffu;
    sWsGameplayMode = PORT_3DS_FULL_VIEW_FALLBACK;
    sWsProducerPendingRoomKey = 0xffffffffu;
    sWsProducerPendingMode = PORT_3DS_FULL_VIEW_FALLBACK;
    sWsContentKey = 0xffffffffu;
    sWsContentPx = 0;
    sWsContentHeightPx = 0;
    sWsExperimentalCameraRoomKey = 0xffffffffu;
    sWsExperimentalCameraSnapArmed = 1;
    sWsStateLoaded = 1;
    Port_WidescreenPublish3DSView(0xffffffffu, PORT_3DS_FULL_VIEW_FALLBACK);
    virtuappu_mode1_ws_full_view = 0;
}

int Port_Widescreen_3DSHoldTop(void) {
#ifdef TMC_3DS
    if (sWsRoomLoadGraceTicks <= 0 || gMain.task != TASK_GAME) return 0;
    if (sWsRoomLoadTicks <= 0) {
        /* The load is over, but Full View takes a few more ticks to latch
         * (FULL_VIEW_ENTER_TICKS, then the shadows): releasing the hold at 16
         * showed those frames at 266x160 before the 400x240 view. Hold on
         * while the policy wants Full View and only the latch is missing. */
        const Port3DSFullViewFallbackReason reason = Port_Widescreen_3DSFallbackReason();
        if (reason != PORT_3DS_FULL_VIEW_REASON_LATCH_MISMATCH &&
            reason != PORT_3DS_FULL_VIEW_REASON_ROOM_GENERATION &&
            reason != PORT_3DS_FULL_VIEW_REASON_SHADOWS_UNAVAILABLE)
            return 0;
    }
    if (!Port_Config_3DSFullViewComboEnabled() || Port_WidescreenFixedCanvasSubtask()) return 0;
    /* Every room is a Full View candidate now (interiors included, P4.6), so a door into a house holds
     * the faded frame as well, instead of showing its first frames at 240/266 before the 400x240 latch. */
    if (gRoomControls.area > AREA_98) return 0;
    Port3DSFullViewMode published = PORT_3DS_FULL_VIEW_FALLBACK;
    (void)Port_WidescreenRead3DSView(NULL, &published);
    return published == PORT_3DS_FULL_VIEW_FALLBACK;
#else
    return 0;
#endif
}

Port3DSFullViewFallbackReason Port_Widescreen_3DSFallbackReason(void) {
#ifdef TMC_3DS
    Port3DSFullViewFallbackReason reason = PORT_3DS_FULL_VIEW_REASON_NONE;
    const Port3DSFullViewInputs inputs = Port_WidescreenFullViewInputs();
    const Port3DSFullViewMode desired = Port3DSFullViewPolicy_Decide(&inputs, &reason);
    u32 publishedKey;
    Port3DSFullViewMode publishedMode;
    if (desired == PORT_3DS_FULL_VIEW_FALLBACK) return reason;
    if (!Port_WidescreenRead3DSView(&publishedKey, &publishedMode) ||
        publishedKey != Port_WidescreenRoomKey()) {
        return PORT_3DS_FULL_VIEW_REASON_ROOM_GENERATION;
    }
    if (publishedMode != desired) return PORT_3DS_FULL_VIEW_REASON_LATCH_MISMATCH;
    if (!Port_Widescreen_ShadowsLive()) return PORT_3DS_FULL_VIEW_REASON_SHADOWS_UNAVAILABLE;
    return PORT_3DS_FULL_VIEW_REASON_NONE;
#else
    return PORT_3DS_FULL_VIEW_REASON_COMBO_DISABLED;
#endif
}

int Port_Widescreen_IsActive(void) {
    /* No message/controlMode gate here: snapping the viewport wide->240 for
     * every textbox (and back on close) was the single worst widescreen UX
     * issue. Dialogue now stays wide — the PPU centers the BG0 textbox via
     * the ws_msg_* knobs (published in Port_Widescreen_UpdateShadows), and
     * overlay screens that replace the map BGs (prologue storybook, pause
     * menu, ...) are caught by Port_Widescreen_ShadowsLive() instead: no
     * matching BG -> no shadow -> present native 240. */
    if (Port_Widescreen_3DSViewMode() != PORT_3DS_FULL_VIEW_FALLBACK) {
        return 1;
    }
    return (gMain.task == TASK_GAME && !Port_WidescreenFixedCanvasSubtask() &&
            Port_Config_WidescreenEnabled() && !Port_Widescreen_FallbackNative()) ? 1 : 0;
}

int Port_Widescreen_EffectiveViewWidth(void) {
    s32 eff;
    const Port3DSFullViewMode mode = Port_Widescreen_3DSViewMode();
    if (mode == PORT_3DS_FULL_VIEW_OUTDOOR_1X) {
        return 400;
    }
    if (mode == PORT_3DS_FULL_VIEW_INTERIOR_2X) {
        return 200;
    }
    if (!Port_Widescreen_IsActive()) {
        return 240;
    }
    eff = Port_WidescreenEffectiveTarget();
    return eff > 240 ? (int)eff : 240;
}

int Port_Widescreen_EffectiveViewHeight(void) {
    switch (Port_Widescreen_3DSViewMode()) {
        case PORT_3DS_FULL_VIEW_OUTDOOR_1X:
            return 240;
        case PORT_3DS_FULL_VIEW_INTERIOR_2X:
            return 120;
        default:
            return 160;
    }
}

/* A Full View border scroll carries the view into the next room: it is the
 * same 400x240 camera moving on, not a new room to latch. */
static int Port_WidescreenScrollCarriesFullView(void) {
    return Port_WidescreenFullViewScrollActive() && sWsGameplayMode == PORT_3DS_FULL_VIEW_OUTDOOR_1X;
}

static Port3DSFullViewMode Port_WidescreenGameplayMode(void) {
    if (sWsGameplayRoomKey == Port_WidescreenRoomKey()) {
        return sWsGameplayMode;
    }
    /* The scroll switches gRoomControls.room mid-tick (sub_0807BD14), before
     * UpdateShadows commits the carry at the next VBlank; the rest of that tick
     * (the HUD's button anchors) must already see the carried 400 view. */
    if (Port_WidescreenScrollCarriesFullView()) {
        return PORT_3DS_FULL_VIEW_OUTDOOR_1X;
    }
    /* Presentation may intentionally show the previous room's last committed
     * frame for one boundary. A new live room must never inherit that old
     * room's experimental camera/producer geometry. */
    return PORT_3DS_FULL_VIEW_FALLBACK;
}

int Port_Widescreen_GameplayViewWidth(void) {
    if (Port_Widescreen_PortalCutsceneWide() || Port_Widescreen_TitleWide()) return 400;
    switch (Port_WidescreenGameplayMode()) {
        case PORT_3DS_FULL_VIEW_OUTDOOR_1X:
            return 400;
        case PORT_3DS_FULL_VIEW_INTERIOR_2X:
            return 200;
        default:
            return Port_WidescreenEstablishedViewWidth();
    }
}

int Port_Widescreen_GameplayViewHeight(void) {
    if (Port_Widescreen_PortalCutsceneWide() || Port_Widescreen_TitleWide()) return 240;
    switch (Port_WidescreenGameplayMode()) {
        case PORT_3DS_FULL_VIEW_OUTDOOR_1X:
            return 240;
        case PORT_3DS_FULL_VIEW_INTERIOR_2X:
            return 120;
        default:
            return 160;
    }
}

/* Single source of truth for the camera's rest x (see port_widescreen.h).
 * Clamp order (lo wins over hi) matches the GBA branches it replaces. */
static int Port_WidescreenCameraRestXForMode(int target_x, Port3DSFullViewMode mode) {
    int viewW = mode == PORT_3DS_FULL_VIEW_OUTDOOR_1X ? 400 :
                mode == PORT_3DS_FULL_VIEW_INTERIOR_2X ? 200 :
                Port_WidescreenEstablishedViewWidth();
    int lo = (int)gRoomControls.origin_x;
    int boundsW = (int)gRoomControls.width;
    if (mode != PORT_3DS_FULL_VIEW_FALLBACK &&
        sWsContentKey == Port_WidescreenRoomKey() && sWsContentPx > 0 &&
        sWsContentPx < boundsW) {
        boundsW = (int)sWsContentPx;
    }
    int hi = lo + boundsW - viewW;
    int want = target_x - viewW / 2;
    /* Room (or its painted part) narrower than a Full View: centre it, the rest is black. */
    if (mode == PORT_3DS_FULL_VIEW_OUTDOOR_1X && boundsW < viewW) {
#ifdef TMC_3DS
        /* P4.9: where neighbours fill the margin, the camera may sit off centre to show them (and no black). */
        if (boundsW == (int)gRoomControls.width)
            return Port_NeighbourRooms_Rest(gRoomControls.room, 0, viewW, gRoomControls.scroll_x);
#endif
        return lo - (viewW - boundsW) / 2;
    }
    if (want > hi) {
        want = hi;
    }
    if (want < lo) {
        want = lo;
    }
    return want;
}

static int Port_WidescreenCameraRestYForMode(int target_y, Port3DSFullViewMode mode) {
    int viewH = mode == PORT_3DS_FULL_VIEW_OUTDOOR_1X ? 240 :
                mode == PORT_3DS_FULL_VIEW_INTERIOR_2X ? 120 :
                160;
    int lo = (int)gRoomControls.origin_y;
    int boundsH = (int)gRoomControls.height;
    if (mode != PORT_3DS_FULL_VIEW_FALLBACK &&
        sWsContentKey == Port_WidescreenRoomKey() && sWsContentHeightPx > 0 &&
        sWsContentHeightPx < boundsH) {
        boundsH = (int)sWsContentHeightPx;
    }
    int hi = lo + boundsH - viewH;
    int want = target_y - viewH / 2;
    if (mode == PORT_3DS_FULL_VIEW_OUTDOOR_1X && boundsH < viewH) {
#ifdef TMC_3DS
        if (boundsH == (int)gRoomControls.height)
            return Port_NeighbourRooms_Rest(gRoomControls.room, 1, viewH, gRoomControls.scroll_y);
#endif
        return lo - (viewH - boundsH) / 2;
    }
    if (want > hi) {
        want = hi;
    }
    if (want < lo) {
        want = lo;
    }
    return want;
}

int Port_Widescreen_CameraRestX(int target_x) {
    return Port_WidescreenCameraRestXForMode(target_x, Port_WidescreenGameplayMode());
}

int Port_Widescreen_CameraRestY(int target_y) {
    return Port_WidescreenCameraRestYForMode(target_y, Port_WidescreenGameplayMode());
}

/* The Minish Paths' parallax layers (BG1 leaves, BG3 shadows) follow the camera through their managers. A snap of
 * the camera into or out of Full View happens while a room fades in, when the managers do not run yet: the layers
 * kept the old camera's offsets for the whole fade and jumped into place when it ended. Their update is run again
 * here, with the new camera. */
void sub_0805754C(VerticalMinishPathBackgroundManager*);
void sub_08057F20(HorizontalMinishPathBackgroundManager*);
static void Port_WidescreenRefreshParallax(void) {
    Entity* manager = DeepFindEntityByID(MANAGER, VERTICAL_MINISH_PATH_BACKGROUND_MANAGER);
    if (manager != NULL) sub_0805754C((VerticalMinishPathBackgroundManager*)manager);
    manager = DeepFindEntityByID(MANAGER, HORIZONTAL_MINISH_PATH_BACKGROUND_MANAGER);
    if (manager != NULL) sub_08057F20((HorizontalMinishPathBackgroundManager*)manager);
}

static void Port_WidescreenSnapGameplayCamera(Port3DSFullViewMode mode) {
    const Port3DSFullViewMode previous = Port_WidescreenGameplayMode();
    sWsGameplayMode = mode;
    /* AuxCutscene 0 (the Minish Woods seen when the Deepwood Shrine's big door opens) places its camera by hand and
     * its camera target is still Link, in the shrine: the 400x240 view keeps the centre of the GBA's 240x160 picture
     * instead (80 and 40 px more on each side), and gives it back on the way out. Only while the woods are shown:
     * from Subtask_Die (nextToLoad 4) the shrine's own camera is back and snaps as any room's. */
    if (gMain.substate == GAMEMAIN_SUBTASK && gUI.lastState == SUBTASK_AUXCUTSCENE && gUI.field_0x3 == 0 &&
        gUI.nextToLoad < 4 && previous != mode) {
        const int sign = mode == PORT_3DS_FULL_VIEW_OUTDOOR_1X ? -1 : previous == PORT_3DS_FULL_VIEW_OUTDOOR_1X ? 1 : 0;
        gRoomControls.scroll_x = (s16)(gRoomControls.scroll_x + sign * 80);
        gRoomControls.scroll_y = (s16)(gRoomControls.scroll_y + sign * 40);
        gUpdateVisibleTiles = 1;
        Port_WidescreenRefreshParallax();
        return;
    }
    if (gRoomControls.camera_target == NULL) {
        /* A frozen camera leaving Full View (a portal's closing window, a script
         * moving it out of the room's 400x240 range): centre the narrower view on
         * Link, as the GBA camera was before it froze, not on the 400 view's
         * top-left corner. Effects aimed at Link's screen position then land in it. */
        if ((previous == PORT_3DS_FULL_VIEW_OUTDOOR_1X && mode == PORT_3DS_FULL_VIEW_FALLBACK) ||
            (previous == PORT_3DS_FULL_VIEW_FALLBACK && mode == PORT_3DS_FULL_VIEW_OUTDOOR_1X && sWsArrivalFrozenCamera)) {
            const int cx = gPlayerEntity.base.kind == PLAYER ? gPlayerEntity.base.x.HALF.HI : gRoomControls.scroll_x + 200;
            const int cy = gPlayerEntity.base.kind == PLAYER ? gPlayerEntity.base.y.HALF.HI : gRoomControls.scroll_y + 120;
            gRoomControls.scroll_x = (s16)Port_WidescreenCameraRestXForMode(cx, mode);
            gRoomControls.scroll_y = (s16)Port_WidescreenCameraRestYForMode(cy, mode);
            gUpdateVisibleTiles = 1;
            Port_WidescreenRefreshParallax();
        }
        return;
    }
    gRoomControls.scroll_x = (s16)Port_WidescreenCameraRestXForMode(
        gRoomControls.camera_target->x.HALF.HI, mode);
    gRoomControls.scroll_y = (s16)Port_WidescreenCameraRestYForMode(
        gRoomControls.camera_target->y.HALF.HI, mode);
    gUpdateVisibleTiles = 1;
    UpdateScreenShake();
    Port_WidescreenRefreshParallax();
}

static int Port_WidescreenMapShadowsCanPrepare(void);

int Port_Widescreen_PrepareGameplayCamera(void) {
    const u32 roomKey = Port_WidescreenRoomKey();
    if (sWsGameplayRoomKey != roomKey) {
        return 0;
    }

    const Port3DSFullViewInputs inputs = Port_WidescreenFullViewInputs();
    Port3DSFullViewMode desired = Port3DSFullViewPolicy_Desired(&inputs);
    if (desired != PORT_3DS_FULL_VIEW_FALLBACK &&
        !Port_WidescreenMapShadowsCanPrepare()) {
        desired = PORT_3DS_FULL_VIEW_FALLBACK;
        sWsProducerPendingRoomKey = 0xffffffffu;
        sWsProducerPendingMode = PORT_3DS_FULL_VIEW_FALLBACK;
    }

    /* Promotion is staged by UpdateShadows one VBlank earlier. Perform the
     * camera snap here, before Scroll1/map/HUD/OAM production, rather than in
     * VBlank after those consumers have already captured the old geometry. */
    if (sWsGameplayMode == PORT_3DS_FULL_VIEW_FALLBACK) {
        if (desired == PORT_3DS_FULL_VIEW_FALLBACK ||
            sWsProducerPendingRoomKey != roomKey || sWsProducerPendingMode != desired) {
            return 0;
        }
        Port_WidescreenSnapGameplayCamera(desired);
        sWsExperimentalCameraSnapArmed = 0;
        return 1;
    }

    if (desired != PORT_3DS_FULL_VIEW_FALLBACK) {
        return 0;
    }

    Port_WidescreenSnapGameplayCamera(PORT_3DS_FULL_VIEW_FALLBACK);
    sWsExperimentalCameraSnapArmed = 1;
    sWsProducerPendingRoomKey = 0xffffffffu;
    sWsProducerPendingMode = PORT_3DS_FULL_VIEW_FALLBACK;
    if (gFadeControl.active && (gFadeControl.type & FADE_IRIS) != 0 &&
        gRoomControls.camera_target != NULL) {
        gFadeControl.iris_x = gRoomControls.camera_target->x.HALF.HI - gRoomControls.scroll_x;
        gFadeControl.iris_y = gRoomControls.camera_target->y.HALF.HI - gRoomControls.scroll_y;
    }
    return 1;
}

/* True while at least one BG shadow is registered for the current frame —
 * i.e. the map BGs are actually what the PPU is rendering. Overlay screens
 * (prologue storybook, pause/menu subscreens) swap the BG control regs away
 * from the map settings, the shadow match fails, and this drops to 0 so the
 * present path falls back to a native 240 frame instead of showing a black
 * right third. */
int Port_Widescreen_ShadowsLive(void) {
    int i;
    for (i = 0; i < MODE1_GBA_BG_COUNT; i++) {
        if (virtuappu_mode1_ws_shadow[i] != NULL) {
            return 1;
        }
    }
    return 0;
}

int Port_Widescreen_HudRightAnchor(void) {
    if (!Port_Widescreen_IsActive()) {
        return 0;
    }
    if (gHUD.hideFlags != HUD_HIDE_NONE) {
        return 0;
    }
    return 1;
}

int Port_Widescreen_ProducerViewWidth(void) {
    if (Port_Widescreen_PortalCutsceneWide() || Port_Widescreen_TitleWide()) return 400;
    /* Producer geometry is the camera/gameplay mode already prepared for
     * this tick. Live policy may change after that preparation; UpdateShadows
     * intentionally commits the completed producer generation once before
     * the next tick adopts the new policy. */
    switch (Port_WidescreenGameplayMode()) {
        case PORT_3DS_FULL_VIEW_OUTDOOR_1X:
            return 400;
        case PORT_3DS_FULL_VIEW_INTERIOR_2X:
            return 200;
        default:
            return Port_WidescreenEstablishedViewWidth();
    }
}

int Port_Widescreen_ProducerViewHeight(void) {
    if (Port_Widescreen_PortalCutsceneWide() || Port_Widescreen_TitleWide()) return 240;
    switch (Port_WidescreenGameplayMode()) {
        case PORT_3DS_FULL_VIEW_OUTDOOR_1X:
            return 240;
        case PORT_3DS_FULL_VIEW_INTERIOR_2X:
            return 120;
        default:
            return 160;
    }
}

int Port_Widescreen_HudTargetViewWidth(void) {
    return Port_Widescreen_ProducerViewWidth();
}

int Port_Widescreen_HudTargetViewHeight(void) {
    return Port_Widescreen_ProducerViewHeight();
}

/* The tilemap entry for "nothing here" on a shadow layer. Entry 0 is tile 0, which is blank in most
 * tilesets but not all (an interior whose BG3 has a dither tile there drew it as garbage beside the
 * room), so use the first fully transparent tile of the layer's own character block. */
static u16 Port_WidescreenBlankEntry(int bg_index) {
    const u32 control = bg_index == 0 ? gScreen.bg0.control
                        : bg_index == 1 ? gScreen.bg1.control
                        : bg_index == 2 ? gScreen.bg2.control
                                        : gScreen.bg3.control;
    if ((control & 0x80u) != 0) return 0;
    const u8* const chars = gVram + ((control >> 2) & 3u) * 0x4000u;
    /* A tilemap entry reaches 1024 tiles, past its own 16 KB block, as long as it stays in BG memory. */
    const u32 firstTile = ((control >> 2) & 3u) * 0x4000u / 32u;
    for (u32 tile = 0; tile < 1024u && firstTile + tile < 2048u; ++tile) {
        const u32* words = (const u32*)(chars + tile * 32u);
        if ((words[0] | words[1] | words[2] | words[3] | words[4] | words[5] | words[6] | words[7]) == 0)
            return (u16)tile;
    }
    return 0;
}

static void Port_WidescreenShadow_Populate(int bg_index, u16* mapSpecial, u16* shadow,
                                           int fullViewOutdoor) {
    /* Populate the port-side shadow tilemap the PPU reads for the reveal
     * columns (display x >= MODE1_GBA_BG_CLIP_X). The engine's 32-tile VRAM
     * screenblock only spans ~256 px of world and wraps past that, so the
     * reveal columns can't come from VRAM; we mirror, from gMapData*Special,
     * exactly what the engine fill (ram_sub_080B197C_c) would have placed.
     *
     * Engine ground truth (ram_sub_080B197C_c, common case ydiff>=8):
     *   VRAM buffer cell (row sr, col c) = mapSpecial[2*row16 - 1 + sr][2*col16 + c]
     *   with col16 = xdiff>>4, row16 = ydiff>>4, mapSpecial stride 128 u16.
     * The PPU reads the shadow at [local_row][shadow_idx] where
     *   local_row  = ((line + BGVOFS) % 256)/8 % 32   (== the VRAM buffer row)
     *   shadow_idx = (tile_col - base) % 32           (tile_col == VRAM col at x)
     *
     * Therefore:
     *   (a) the shadow ROW index IS the VRAM buffer row sr, so fill shadow[sr]
     *       with map row (2*row16 - 1 + sr) — NOT (world_row & 31), which reads
     *       camera-shifted wrong rows.
     *   (b) the consumer base MUST equal the VRAM tile_col of the first reveal
     *       column (display CLIP_X) = CLIP_X/8 + (BGHOFS>=8 ? 1 : 0), so
     *       shadow_idx lands on reveal column index d. BGHOFS = (scroll-origin)
     *       & 0xf (UpdateScreenShake); its upper half carries one extra tile.
     * Getting either wrong shifts/wraps the reveal into stale cells — the
     * far-edge garbage. (No residency gate: the area tileset is resident in
     * char VRAM regardless of the 32-col tilemap window, so gating on that
     * window blacked legitimately-loaded reveal tiles. The PPU's 0x7C1F skip
     * still drops genuinely-unstreamed-palette pixels to clean black.) */
    s16 xdiff = (s16)(gRoomControls.scroll_x - gRoomControls.origin_x);
    s16 ydiff = (s16)(gRoomControls.scroll_y - gRoomControls.origin_y);
    s16 bg_x_offset = 0;
    s16 bg_y_offset = 0;
    int shadow_cols = fullViewOutdoor ? MODE1_WS_SHADOW_FULL_VIEW_COLS
                                       : MODE1_WS_SHADOW_WIDE_COLS;
    if (shadow_cols > MODE1_WS_SHADOW_COLS) shadow_cols = MODE1_WS_SHADOW_COLS;
    const s32 first_display_tile = fullViewOutdoor ? 0 : (MODE1_GBA_BG_CLIP_X / 8);
    s32 ws_base_world_col;
    switch (bg_index) {
        case 0: bg_x_offset = (s16)gScreen.bg0.xOffset; bg_y_offset = (s16)gScreen.bg0.yOffset; break;
        case 1: bg_x_offset = (s16)gScreen.bg1.xOffset; bg_y_offset = (s16)gScreen.bg1.yOffset; break;
        case 2: bg_x_offset = (s16)gScreen.bg2.xOffset; bg_y_offset = (s16)gScreen.bg2.yOffset; break;
        case 3: bg_x_offset = (s16)gScreen.bg3.xOffset; bg_y_offset = (s16)gScreen.bg3.yOffset; break;
    }
    ws_base_world_col = (xdiff >> 4) * 2 + (bg_x_offset >> 3) + first_display_tile;
    if (fullViewOutdoor) {
        const s32 shake_x = (s32)bg_x_offset - (xdiff & 0xf);
        ws_base_world_col = ((xdiff + shake_x) >> 3) + first_display_tile;
    }
    virtuappu_mode1_ws_shadow_base_tile[bg_index] = first_display_tile + ((bg_x_offset & 0xff) >> 3);

    enum { kMapStride = 128, kMapRows = 128 };
    const u16 blankEntry = Port_WidescreenBlankEntry(bg_index);
    /* Clamp to the ROOM rect, not just the 128-tile buffer: the buffers are
     * reused across rooms without clearing, so cells past the current room's
     * extent hold the previous room's tiles — sampling them leaked stale
     * graphics into the reveal near room edges. Outside the room = entry 0
     * (transparent; composite force-blacks it), same as GBA's void. */
    s32 room_tiles_w = (s32)gRoomControls.width / 8;
    s32 room_tiles_h = (s32)gRoomControls.height / 8;
    if (room_tiles_w > kMapStride)
        room_tiles_w = kMapStride;
    if (room_tiles_h > kMapRows)
        room_tiles_h = kMapRows;
    const int scrolling = Port_WidescreenScrollActive();
#ifdef TMC_3DS
    const int topLayer = mapSpecial == gMapDataTopSpecial;
    /* Rooms that swap building graphics by zone (port/port_zone_tiles.h, issue #26): each entry's
     * zone, so the renderer draws it from that zone's set. */
    uint8_t* const zoneRows = !scrolling && Port_ZoneTiles_Active() ? Port_ZoneTiles_ShadowVariants(bg_index) : NULL;
#endif
    for (int sr = 0; sr < MODE1_WS_SHADOW_ROWS; sr++) {
        int shadow_row = sr;
        s32 world_row = 2 * (ydiff >> 4) - 1 + sr;
        if (!scrolling && ydiff >= 0 && ydiff < 8 && sr == 0)
            world_row = 0;
        if (fullViewOutdoor) {
            /* The 240-line view can cross the rolling 32-row screenblock.
             * Publish each visible world row into the wrapped slot the PPU
             * will actually address after BGVOFS and screen shake. */
            const s32 shake_y = (s32)bg_y_offset - ((ydiff & 0xf) + 8);
            const int first_screen_row = ((bg_y_offset & 0xff) >> 3) & 31;
            shadow_row = (first_screen_row + sr) & 31;
            world_row = ((ydiff + shake_y) >> 3) + sr;
        }
        u16* row_dst = shadow + (size_t)shadow_row * (size_t)shadow_cols;
#ifdef TMC_3DS
        uint8_t* const zone_dst = zoneRows ? zoneRows + (size_t)shadow_row * (size_t)shadow_cols : NULL;
        if (zone_dst) memset(zone_dst, 0, (size_t)shadow_cols);
#endif
        if (!scrolling && (world_row < 0 || world_row >= room_tiles_h)) {
            for (int C = 0; C < shadow_cols; C++) {
#ifdef TMC_3DS
                /* P4.9: past the room's top or bottom edge, the neighbouring room's tile. */
                row_dst[C] = Port_NeighbourRooms_Tile(topLayer, ws_base_world_col + C, world_row, blankEntry);
#else
                row_dst[C] = blankEntry;
#endif
            }
            continue;
        }
        for (int C = 0; C < shadow_cols; C++) {
            s32 world_col = ws_base_world_col + C;
            u16 entry = scrolling ? Port_WidescreenScrollTile(mapSpecial, world_col, world_row)
                                  : ((world_col >= 0 && world_col < room_tiles_w)
                                         ? mapSpecial[world_row * kMapStride + world_col]
#ifdef TMC_3DS
                                         : Port_NeighbourRooms_Tile(topLayer, world_col, world_row, blankEntry));
#else
                                         : blankEntry);
#endif
            row_dst[C] = entry;
#ifdef TMC_3DS
            if (zone_dst && entry != 0) zone_dst[C] = Port_ZoneTiles_Variant(world_col, world_row);
#endif
        }
    }
#ifdef TMC_3DS
    Port_ZoneTiles_SetShadowLive(bg_index, zoneRows != NULL);
#endif
    virtuappu_mode1_ws_shadow_cols[bg_index] = (uint8_t)shadow_cols;
    virtuappu_mode1_ws_shadow_stride[bg_index] = (uint8_t)shadow_cols;
    virtuappu_mode1_ws_shadow[bg_index] = shadow;
}

/* Which PPU BG index renders a given map: the PPU selects a BG's tilemap by
 * its BGCNT screen_base, so the map whose bgSettings->control screen_base
 * matches gScreen.bgN.control is rendered as BG N. (In the field this is
 * bottom->BG2, top->BG1 — the reverse of the buffer numbering, which is why a
 * hardcoded shadow[1]=bottom/shadow[2]=top was swapped and showed garbage.) */
static int Port_WidescreenPpuBgForControl(u32 control) {
    u32 sb = (control >> 8) & 0x1fu;
    if (((gScreen.bg0.control >> 8) & 0x1fu) == sb)
        return 0;
    if (((gScreen.bg1.control >> 8) & 0x1fu) == sb)
        return 1;
    if (((gScreen.bg2.control >> 8) & 0x1fu) == sb)
        return 2;
    if (((gScreen.bg3.control >> 8) & 0x1fu) == sb)
        return 3;
    return -1;
}

static int Port_WidescreenMapShadowsCanPrepare(void) {
    int required = 0;
    if (gMapBottom.bgSettings != NULL) {
        ++required;
        if (Port_WidescreenPpuBgForControl(gMapBottom.bgSettings->control) < 0) {
            return 0;
        }
    }
    if (gMapTop.bgSettings != NULL) {
        ++required;
        if (Port_WidescreenPpuBgForControl(gMapTop.bgSettings->control) < 0) {
            return 0;
        }
    }
    return required > 0;
}

/* Minish paths own two parallax tilemaps instead of gMapTop. Their reveal
 * must use each manager's actual source/page and scroll, not the ground map. */
static u16 sWsParallax[2][MODE1_WS_SHADOW_ROWS * MODE1_WS_SHADOW_COLS];
static void Port_WidescreenParallaxShadows(int fullView) {
    /* Full View draws the whole 400 columns from the shadow (the first one is the view's left
     * edge); the 240/266 path only the columns past the native 240. */
    const int shadowCols = fullView ? MODE1_WS_SHADOW_FULL_VIEW_COLS : MODE1_WS_SHADOW_WIDE_COLS;
    const int firstTile = fullView ? 0 : 30;
    if (gMapTop.bgSettings != NULL || (gScreen.bg3.control & ~0x43) != 0x1d08 ||
        (gScreen.bg1.control & ~0x43) != 0x1e08)
        return;
    for (int layer = 0; layer < 2; ++layer) {
        const int bg = layer ? 1 : 3;
        const BgSettings* settings = layer ? &gScreen.bg1 : (const BgSettings*)&gScreen.bg3;
        const u16* src = settings->subTileMap;
        int cols = 32, start = 0;
        if (src == gBG3Buffer + layer * 0x400) {
            const int scroll =
                Port_HorizontalMinishPathScroll(gRoomControls.scroll_x - gRoomControls.origin_x, gRoomControls.width,
                                                layer ? 2 : 3, Port_Widescreen_GameplayViewWidth());
            start = (scroll >> 4) * 2;
            cols = 128;
            src = (const u16*)(gUnk_02006F00 + layer * 0x2000);
        } else {
            const uintptr_t off = (uintptr_t)src - (uintptr_t)gMapDataTopSpecial;
            if (off > 0x7800 || (off & 1))
                continue;
        }
        const int base = firstTile + ((settings->xOffset & 0xff) >> 3);
        /* A room narrower than the Full View is centred in it with black on both sides (where
         * the owner's border pictures go): the room's own picture, the 240 columns the game
         * shows at its native scroll, is placed at view columns shift..shift+roomCols-1 and
         * the parallax layers draw nothing outside it. The game's scroll is left as it is. */
        /* The vertical corridors point the layer at a window of 32 rows of a taller map and
         * scroll it by yOffset & 0x3f. The GBA's 160 lines never reach past that window; the
         * view's 240 lines do, and the hardware would wrap to the window's first rows. Those
         * rows belong to the map rows that follow the window, so the slots above the first
         * displayed one are read from there (the displayed rows are fewer than 32, so no slot
         * serves both ends). */
        int wrapRows = 0, srcLimit = 0;
        if (cols == 32) {
            wrapRows = (settings->yOffset & 0xff) >> 3;
            srcLimit = (layer ? 0x2000 : 0x1000) - (int)(((uintptr_t)src - (uintptr_t)gMapDataTopSpecial) >> 1);
        }
        /* Centred unless neighbour rooms drew the camera aside (P4.9: the Crenel paths, area 0x12): the
         * layers then go where the ground is, at the camera's distance from the room's left edge. */
        int shift = 0, firstCol = 0, endCol = shadowCols;
        if (fullView && (int)gRoomControls.width < 400) {
            const int margin = (400 - (int)gRoomControls.width) / 8;
            shift = ((int)gRoomControls.origin_x - gRoomControls.scroll_x + 4) >> 3;
            if (shift < 0) shift = 0;
            if (shift > margin) shift = margin;
            firstCol = shift;
            endCol = shift + (int)gRoomControls.width / 8;
        }
        for (int row = 0; row < 32; ++row) {
            for (int col = 0; col < shadowCols; ++col) {
                if (col < firstCol || col >= endCol) {
                    sWsParallax[layer][row * shadowCols + col] = 0;
                    continue;
                }
                int x = start + base + col - shift;
                if (cols == 32)
                    x &= 31;
                int srcIdx = row * cols + x;
                if (row < wrapRows && srcIdx + 32 * cols < srcLimit)
                    srcIdx += 32 * cols;
                sWsParallax[layer][row * shadowCols + col] = x < cols ? src[srcIdx] : 0;
            }
        }
        virtuappu_mode1_ws_shadow[bg] = sWsParallax[layer];
        virtuappu_mode1_ws_shadow_base_tile[bg] = base;
        virtuappu_mode1_ws_shadow_cols[bg] = shadowCols;
        virtuappu_mode1_ws_shadow_stride[bg] = shadowCols;
    }
}

/* The light rays of the Minish Woods (LightRayManager, gfx group 0x25) are a
 * screen-fixed BG3 picture that fills only the right half of its 256 px map and
 * ends at the right edge of the GBA screen: the light comes in from there. In
 * Full View the layer would otherwise repeat from the view's left edge, cutting
 * the rays at x=240 and starting them mid-view. Anchor the one copy of the map
 * to the right edge of the view instead (as the HUD is) and leave the columns
 * to its left empty (tile 0 of the rays' charblock is blank). */
static u16 sWsLightRays[MODE1_WS_SHADOW_ROWS * MODE1_WS_SHADOW_FULL_VIEW_COLS];
static void Port_WidescreenLightRayShadow(int width) {
    const LightRayManager* rays = (const LightRayManager*)DeepFindEntityByID(MANAGER, LIGHT_RAY_MANAGER);
    const u16* map = gScreen.bg3.subTileMap;
    if (rays == NULL || rays->gfxGroup != 0x25 || map == NULL || (gScreen.lcd.displayControl & DISPCNT_BG3_ON) == 0 ||
        (gScreen.bg3.control & 0xC080) != 0 || width <= 240 || ((width - 240) & 7) != 0)
        return;
    /* Shadow column c covers view x [c*8 - fine, c*8 + 8 - fine): with the map's
     * native screen moved right by width-240, that is map column c + H/8 - (width-240)/8. */
    const int first = ((gScreen.bg3.xOffset & 0x1ff) >> 3) - (width - 240) / 8;
    /* The 240 px tall view reaches past the map's 32 rows and would wrap to its
     * top; the map is a 32-row window into gBG3Buffer (64 rows), so the rows
     * below come from the buffer. The view spans at most 31 rows, so each one
     * keeps its own slot at (row & 31), the slot the renderers read. */
    const int firstRow = (gScreen.bg3.yOffset & 0x1ff) >> 3;
    const u16 blankEntry = Port_WidescreenBlankEntry(3);
    const ptrdiff_t inBuffer = map - gBG3Buffer;
    for (int k = 0; k < MODE1_WS_SHADOW_ROWS; ++k) {
        const int row = firstRow + k;
        const u16* src;
        if (inBuffer >= 0 && inBuffer < (ptrdiff_t)ARRAY_COUNT(gBG3Buffer))
            src = inBuffer + row * 32 < (ptrdiff_t)ARRAY_COUNT(gBG3Buffer) ? map + row * 32 : NULL;
        else
            src = map + (row & 31) * 32;
        u16* dst = &sWsLightRays[(row & 31) * MODE1_WS_SHADOW_FULL_VIEW_COLS];
        for (int col = 0; col < MODE1_WS_SHADOW_FULL_VIEW_COLS; ++col) {
            const int mapCol = first + col;
            dst[col] = src != NULL && mapCol >= 0 && mapCol < 32 ? src[mapCol] : blankEntry;
        }
    }
    virtuappu_mode1_ws_shadow[3] = sWsLightRays;
    virtuappu_mode1_ws_shadow_base_tile[3] = 0;
    virtuappu_mode1_ws_shadow_cols[3] = MODE1_WS_SHADOW_FULL_VIEW_COLS;
    virtuappu_mode1_ws_shadow_stride[3] = MODE1_WS_SHADOW_FULL_VIEW_COLS;
}

/* Mount Crenel's storm (src/manager/weatherChangeManager.c): the rain is BG1 over a 32x32 map (screen base 29,
 * gfx groups 0x2B..0x2E by strength) that only scrolls sideways within one 32-pixel period, in place of the top map
 * layer (gMapTop.bgSettings NULL). Its pattern repeats every 32 columns and rows, so Full View repeats it over all of
 * the view, as the fog and the light rays reach the edges; the map alone covered 256 columns, and the rows past it
 * showed whatever followed it in VRAM. */
static u16 sWsRain[MODE1_WS_SHADOW_ROWS * MODE1_WS_SHADOW_FULL_VIEW_COLS];
static void Port_WidescreenRainShadow(int width) {
    const WeatherChangeManager* weather =
        (const WeatherChangeManager*)DeepFindEntityByID(MANAGER, WEATHER_CHANGE_MANAGER);
    if (weather == NULL || weather->unk_22 > 3 || gMapTop.bgSettings != NULL ||
        (gScreen.lcd.displayControl & DISPCNT_BG1_ON) == 0 || (gScreen.bg1.control & 0xC080) != 0 || width <= 240 ||
        ((width - 240) & 7) != 0)
        return;
    const u16* map = (const u16*)gba_TryMemPtr(0x06000000u + ((gScreen.bg1.control >> 8) & 0x1fu) * 0x800u);
    if (map == NULL)
        return;
    /* Column c of the shadow covers view x [c*8 - fine, c*8 + 8 - fine), as Port_WidescreenLightRayShadow's. */
    const int first = ((gScreen.bg1.xOffset & 0x1ff) >> 3) - (width - 240) / 8;
    const int firstRow = (gScreen.bg1.yOffset & 0x1ff) >> 3;
    for (int k = 0; k < MODE1_WS_SHADOW_ROWS; ++k) {
        const int row = (firstRow + k) & 31;
        u16* dst = &sWsRain[row * MODE1_WS_SHADOW_FULL_VIEW_COLS];
        for (int col = 0; col < MODE1_WS_SHADOW_FULL_VIEW_COLS; ++col)
            dst[col] = map[row * 32 + ((first + col) & 31)];
    }
    virtuappu_mode1_ws_shadow[1] = sWsRain;
    virtuappu_mode1_ws_shadow_base_tile[1] = 0;
    virtuappu_mode1_ws_shadow_cols[1] = MODE1_WS_SHADOW_FULL_VIEW_COLS;
    virtuappu_mode1_ws_shadow_stride[1] = MODE1_WS_SHADOW_FULL_VIEW_COLS;
}

/* Called per-VBlank from src/interrupts.c::UpdateDisplayControls. */
/* Rooms narrower than the view whose black sides take the colour of the room's rim (owner, 2026-10-30; the backdrop,
 * which nothing but the sides shows): the BG palette entry (palette * 16 + index) that has it, or -1. */
int Port_Widescreen_BackdropFill(void) {
    static const struct { u8 area, room; s8 palette, index; } kRooms[] = {
        { 0x26, 0x0C, 7, 15 }, /* the cave with the ladder: its rim's light orange */
        /* Crenel's horizontal Minish path (800x192, shorter than the view): above and below it its wall's purple
         * stones, the bottom rows' colour, a step darker than the top rows' (BG pal 6 colour 3); the rocks (BG1) and
         * their blended shadows (BG3) still go over it. */
        { 0x12, 0x02, 6, 2 },
        { 0x12, 0x03, 6, 2 }, /* the next room: the same maps in its own palettes */
    };
    for (unsigned i = 0; i < sizeof(kRooms) / sizeof(kRooms[0]); ++i)
        if (kRooms[i].area == gRoomControls.area && kRooms[i].room == gRoomControls.room)
            return kRooms[i].palette * 16 + kRooms[i].index;
    return -1;
}

void Port_Widescreen_UpdateShadows(void) {
    if (gMain.task != TASK_GAME || (gRoomControls.scrollAction != 2 && gRoomControls.scrollAction != 0))
        sWsScroll.valid = 0;
    int enterRoomBannerActive = Port_WidescreenBannerState_IsActive(
        &sWsEnterRoomBanner, gMain.task == TASK_GAME, gRoomControls.area, gRoomControls.room);
    const u32 roomKey = Port_WidescreenRoomKey();
    Port3DSFullViewMode desiredMode;
    Port3DSFullViewMode producerMode;
    Port3DSFullViewMode previousMode = PORT_3DS_FULL_VIEW_FALLBACK;
    Port3DSFullViewMode latchedMode;
    u32 previousRoomKey = 0xffffffffu;
    int normalWideActive;
    int fullViewOutdoor;
    int requiredMapShadows = 0;
    int preparedMapShadows = 0;
    const int transitionActive = Port_WidescreenRoomTransitionActive();

    (void)Port_WidescreenRead3DSView(&previousRoomKey, &previousMode);
    if (previousRoomKey != roomKey) previousMode = PORT_3DS_FULL_VIEW_FALLBACK;
    if (sWsExperimentalCameraRoomKey != roomKey) {
        sWsExperimentalCameraRoomKey = roomKey;
        sWsExperimentalCameraSnapArmed = 1;
    }
    if (gMain.substate != GAMEMAIN_CHANGEROOM) sWsChangeRoomTicks = 0;
    else if (sWsChangeRoomTicks < 255) ++sWsChangeRoomTicks;
    if (sWsRoomLoadTicks > 0) --sWsRoomLoadTicks;
    if (sWsRoomLoadGraceTicks > 0) --sWsRoomLoadGraceTicks;
    if (gRoomControls.camera_target != NULL) sWsArrivalFrozenCamera = 0;
    if (gFadeControl.active != 0) sWsFadeRecent = 90;
    else if (sWsFadeRecent > 0) --sWsFadeRecent;
    /* Out of the game (a soft reset to the intro and title, the file select) the gameplay
     * camera is not prepared, so nothing else drops the producer: it stayed in Full View with
     * the last room's key, and the intro was drawn at 400x240 over that room's map shadows. */
    if (gMain.task != TASK_GAME) sWsGameplayMode = PORT_3DS_FULL_VIEW_FALLBACK;
    if (sWsGameplayRoomKey != roomKey) {
        const int carry = Port_WidescreenScrollCarriesFullView();
        sWsRoomLoadTicks = sWsFadeRecent > 0 ? 16 : 0;
        sWsRoomLoadGraceTicks = sWsFadeRecent > 0 ? 16 + 24 : 0;
        sWsArrivalFrozenCamera = sWsFadeRecent > 0 || sWsStateLoaded;
        sWsStateLoaded = 0;
        sWsGameplayRoomKey = roomKey;
        sWsGameplayMode = carry ? PORT_3DS_FULL_VIEW_OUTDOOR_1X : PORT_3DS_FULL_VIEW_FALLBACK;
        sWsProducerPendingRoomKey = 0xffffffffu;
        sWsProducerPendingMode = PORT_3DS_FULL_VIEW_FALLBACK;
    }
    if (transitionActive) {
        sWsExperimentalCameraSnapArmed = 1;
    }

    for (int i = 0; i < MODE1_GBA_BG_COUNT; i++) {
        virtuappu_mode1_ws_shadow[i] = NULL;
        virtuappu_mode1_ws_shadow_cols[i] = MODE1_WS_SHADOW_WIDE_COLS;
        virtuappu_mode1_ws_shadow_stride[i] = MODE1_WS_SHADOW_WIDE_COLS;
#ifdef TMC_3DS
        Port_ZoneTiles_SetShadowLive(i, false);
#endif
    }
#ifdef TMC_3DS
    if (Port_Config_3DSFullViewComboEnabled()) {
        Port_ZoneTiles_Update();
        Port_NeighbourRooms_Update();
    }
#endif
    virtuappu_mode1_ws_hud_right_anchor = 0;
    virtuappu_mode1_ws_hud_right_native_x = MODE1_WS_HUD_RIGHT_NATIVE_X;
    virtuappu_mode1_ws_msg_shift = 0;
    virtuappu_mode1_ws_full_view = 0;
    Port_WidescreenPublish3DSView(roomKey, PORT_3DS_FULL_VIEW_FALLBACK);
#ifdef TMC_3DS
    if (Port_Widescreen_PortalCutsceneWide()) {
        virtuappu_mode1_ws_full_view = 1;
        Port_WidescreenPublish3DSView(WS_CUTSCENE_VIEW_KEY, PORT_3DS_FULL_VIEW_OUTDOOR_1X);
        return;
    }
    if (Port_Widescreen_TitleWide()) {
        virtuappu_mode1_ws_full_view = 1;
        Port_WidescreenPublish3DSView(WS_TITLE_VIEW_KEY, PORT_3DS_FULL_VIEW_OUTDOOR_1X);
        return;
    }
    if (Port_Widescreen_FileSelectWide()) {
        virtuappu_mode1_ws_full_view = 1;
        Port_WidescreenPublish3DSView(WS_FILESELECT_VIEW_KEY, PORT_3DS_FULL_VIEW_OUTDOOR_1X);
        return;
    }
    if (Port_Widescreen_PauseWide()) {
        virtuappu_mode1_ws_full_view = 1;
        Port_WidescreenPublish3DSView(WS_PAUSE_VIEW_KEY, PORT_3DS_FULL_VIEW_OUTDOOR_1X);
        return;
    }
#endif

    /* Refresh painted bounds before choosing geometry. Both dimensions are
     * ratcheted per room so streaming can only make the experimental mode
     * available; it cannot oscillate full/wide while the player stands still. */
    if (gMain.task == TASK_GAME && !Port_WidescreenFixedCanvasSubtask() &&
        Port_Config_WidescreenEnabled()) {
        if (roomKey != sWsContentKey) {
            sWsContentKey = roomKey;
            sWsContentPx = 0;
            sWsContentHeightPx = 0;
        }
        s32 px = Port_WidescreenScanContentPx();
        if (px > sWsContentPx) {
            sWsContentPx = px;
            if (getenv("TMC_WS_TRACE") != NULL) {
                fprintf(stderr, "[ws] area=0x%02x room=0x%02x width=%d content_px=%d -> %s\n",
                        (unsigned)gRoomControls.area, (unsigned)gRoomControls.room, (int)gRoomControls.width,
                        (int)sWsContentPx, Port_Widescreen_FallbackNative() ? "native-240" : "wide");
            }
        }
#ifdef TMC_3DS
        if (Port_Config_3DSFullViewComboEnabled()) {
            s32 contentHeight = Port_WidescreenScanContentHeightPx();
            if (contentHeight > sWsContentHeightPx) {
                sWsContentHeightPx = contentHeight;
            }
        }
#endif
    }

    desiredMode = Port_WidescreenDesired3DSViewMode();
    if (desiredMode != PORT_3DS_FULL_VIEW_FALLBACK &&
        !Port_WidescreenMapShadowsCanPrepare()) {
        sWsExperimentalCameraSnapArmed = 1;
        sWsProducerPendingRoomKey = 0xffffffffu;
        sWsProducerPendingMode = PORT_3DS_FULL_VIEW_FALLBACK;
        desiredMode = PORT_3DS_FULL_VIEW_FALLBACK;
    }

#ifdef TMC_3DS
    {
        /* Entering Full View needs the policy to hold for a while. Leaving an
         * interior runs through a fade, a scroll, the room banner and the
         * content scan, and the raw policy flipped open for two to four ticks
         * between those phases: 266 -> 400 -> 266 -> 400 with a camera snap
         * each time (a flashing screen). Dropping out stays immediate. */
        enum { FULL_VIEW_ENTER_TICKS = 4 };
        static unsigned stableTicks;
        if (desiredMode == PORT_3DS_FULL_VIEW_FALLBACK) {
            stableTicks = 0;
        } else if (stableTicks < FULL_VIEW_ENTER_TICKS) {
            ++stableTicks;
            const Port3DSFullViewMode currentlyProducing =
                sWsGameplayRoomKey == roomKey ? sWsGameplayMode : PORT_3DS_FULL_VIEW_FALLBACK;
            if (currentlyProducing == PORT_3DS_FULL_VIEW_FALLBACK) desiredMode = PORT_3DS_FULL_VIEW_FALLBACK;
        }
    }
#endif
    /* Commit the geometry this tick actually used to produce BG/OAM. Policy
     * only schedules the following tick. This is what prevents a late fade,
     * message or settings tap from reinterpreting a completed 200/400 frame
     * as E2 (and vice versa). */
    producerMode = sWsGameplayRoomKey == roomKey
                       ? sWsGameplayMode
                       : PORT_3DS_FULL_VIEW_FALLBACK;
    if (desiredMode != producerMode) {
        sWsExperimentalCameraSnapArmed = 1;
        if (desiredMode != PORT_3DS_FULL_VIEW_FALLBACK) {
            sWsProducerPendingRoomKey = roomKey;
            sWsProducerPendingMode = desiredMode;
        } else {
            sWsProducerPendingRoomKey = 0xffffffffu;
            sWsProducerPendingMode = PORT_3DS_FULL_VIEW_FALLBACK;
        }
    }
    normalWideActive = gMain.task == TASK_GAME && !Port_WidescreenFixedCanvasSubtask() &&
                       Port_Config_WidescreenEnabled() && !Port_Widescreen_FallbackNative();
    if (producerMode == PORT_3DS_FULL_VIEW_FALLBACK && !normalWideActive) {
        return;
    }

    fullViewOutdoor = producerMode == PORT_3DS_FULL_VIEW_OUTDOOR_1X;
    if (gMapBottom.bgSettings != NULL) {
        ++requiredMapShadows;
        int bg = Port_WidescreenPpuBgForControl(gMapBottom.bgSettings->control);
        if (bg >= 0) {
            Port_WidescreenShadow_Populate(bg, gMapDataBottomSpecial, sWsShadowBG1, fullViewOutdoor);
            ++preparedMapShadows;
        }
    }
    if (gMapTop.bgSettings != NULL) {
        ++requiredMapShadows;
        int bg = Port_WidescreenPpuBgForControl(gMapTop.bgSettings->control);
        if (bg >= 0) {
            Port_WidescreenShadow_Populate(bg, gMapDataTopSpecial, sWsShadowBG2, fullViewOutdoor);
            ++preparedMapShadows;
        }
    }

    Port_WidescreenParallaxShadows(fullViewOutdoor);
#ifdef TMC_3DS
    /* The vertical Minish paths in Full View (rooms of 240x800 whose BG1 is set up by
     * src/manager/verticalMinishPathBackgroundManager.c; which ones and their strips: kShafts in scene_pictures_3ds.c) go
     * on beside the room, each layer drawn as its BG at the room's place on the view: the ground moving with the room,
     * BG1 (ledges, leaves) and BG3 (their shadows) with the scroll their manager gives them (1.25 and 1.125 times the
     * room's). */
    {
        extern void ScenePictures_SetShaftView(int scene, int groundBg, int x, int groundY, int ledgesY, int shadowsY);
        extern int ScenePictures_ShaftScene(int area, int room, const u8* leavesMap);
        /* BG1's map (its part of gMapDataTopSpecial) only while the manager's BG1 is set up: a scene told by that map
         * needs it, the Crenel shaft (found by its room; its BG1 is set up otherwise) does not. */
        const bool leavesBg = gRoomControls.width == 240 &&
                              gScreen.bg1.control ==
                                  (BGCNT_SCREENBASE(30) | BGCNT_PRIORITY(1) | BGCNT_CHARBASE(2) | BGCNT_MOSAIC);
        const int scene = fullViewOutdoor && gMapBottom.bgSettings != NULL
                              ? ScenePictures_ShaftScene(gRoomControls.area, gRoomControls.room,
                                                         leavesBg ? (const u8*)gMapDataTopSpecial + 0x2000 : NULL)
                              : -1;
        const int bg = scene >= 0 ? Port_WidescreenPpuBgForControl(gMapBottom.bgSettings->control) : -1;
        const s32 dy = gRoomControls.scroll_y - gRoomControls.origin_y;
        ScenePictures_SetShaftView(scene, bg, (int)gRoomControls.origin_x - gRoomControls.scroll_x - 80, (int)-dy,
                                   (int)-(dy + (dy >> 2)), (int)-(dy + (dy >> 3)));
    }
    /* Crenel's horizontal Minish path in Full View (area 0x12 room 02, 800x192, horizontalMinishPathBackgroundManager.c):
     * bands above and below the room (scene_pictures_3ds.h, SCENE_ROW_*). The ground's picture column 0 sits on the
     * room's column 0 and its bands just above and below the room; the rocks (BG1) and their shadows (BG3) are their
     * whole 1024-column layer, whose column X the view shows at X minus the layer's scroll (the manager's, at 1.25 and
     * 1.125 times the room's), and row Y at Y minus the BG's vertical offset (their bands start at the layer's rows -32
     * and 208). */
    {
        extern void ScenePictures_SetRowView(int scene, int groundBg, const int x[3], const int y[3][2]);
        int x[3] = { 0, 0, 0 }, y[3][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 } };
        int groundBg = -1;
        /* Room 03 is the same in other palettes (SCENE_ROW_CRENEL_03). */
        const int scene = gRoomControls.room == 0x02 ? 0 : gRoomControls.room == 0x03 ? 1 : -1;
        if (fullViewOutdoor && gRoomControls.area == 0x12 && scene >= 0 && gMapBottom.bgSettings != NULL &&
            (gScreen.bg1.control & ~0x43) == 0x1e08 && (gScreen.bg3.control & ~0x43) == 0x1d08) {
            groundBg = Port_WidescreenPpuBgForControl(gMapBottom.bgSettings->control);
            const int sx = gRoomControls.scroll_x - gRoomControls.origin_x;
            const int top = (int)gRoomControls.origin_y - gRoomControls.scroll_y;
            const int viewWidth = Port_Widescreen_GameplayViewWidth();
            x[0] = -sx;
            y[0][0] = top - 48;
            y[0][1] = top + (int)gRoomControls.height;
            x[1] = -Port_HorizontalMinishPathScroll(sx, gRoomControls.width, 2, viewWidth);
            y[1][0] = -32 - (gScreen.bg1.yOffset & 0xff);
            y[1][1] = 208 - (gScreen.bg1.yOffset & 0xff);
            x[2] = -Port_HorizontalMinishPathScroll(sx, gRoomControls.width, 3, viewWidth);
            y[2][0] = -32 - (gScreen.bg3.yOffset & 0xff);
            y[2][1] = 208 - (gScreen.bg3.yOffset & 0xff);
        }
        ScenePictures_SetRowView(scene, groundBg, x, y);
    }
#endif

    /* Publish only shadows built for this exact room/producer generation. */
#ifdef TMC_3DS
    const int experimentalShadowsReady = requiredMapShadows > 0 &&
                                         preparedMapShadows == requiredMapShadows &&
                                         Port_Widescreen_ShadowsLive();
    latchedMode = Port3DSFullViewPolicy_Latch(
        previousMode, producerMode, 1, experimentalShadowsReady);
#else
    latchedMode = PORT_3DS_FULL_VIEW_FALLBACK;
#endif
    if (producerMode != PORT_3DS_FULL_VIEW_FALLBACK && latchedMode != producerMode) {
        Port_WidescreenSnapGameplayCamera(PORT_3DS_FULL_VIEW_FALLBACK);
        sWsExperimentalCameraSnapArmed = 1;
        sWsProducerPendingRoomKey = 0xffffffffu;
        sWsProducerPendingMode = PORT_3DS_FULL_VIEW_FALLBACK;
    }
    if (latchedMode == PORT_3DS_FULL_VIEW_OUTDOOR_1X) {
        virtuappu_mode1_ws_full_view = 1;
        Port_WidescreenLightRayShadow(400); /* the published view is not 400 wide yet this tick */
        Port_WidescreenRainShadow(400);
    }
    if (!Port_Widescreen_ShadowsLive()) {
        if (producerMode != PORT_3DS_FULL_VIEW_FALLBACK) {
            Port_WidescreenSnapGameplayCamera(PORT_3DS_FULL_VIEW_FALLBACK);
            sWsExperimentalCameraSnapArmed = 1;
            sWsProducerPendingRoomKey = 0xffffffffu;
            sWsProducerPendingMode = PORT_3DS_FULL_VIEW_FALLBACK;
        }
        return;
    }
    Port_WidescreenPublish3DSView(roomKey, latchedMode);
    if (latchedMode == producerMode && latchedMode != PORT_3DS_FULL_VIEW_FALLBACK) {
        sWsProducerPendingRoomKey = 0xffffffffu;
        sWsProducerPendingMode = PORT_3DS_FULL_VIEW_FALLBACK;
    }

    /* Publish the live textbox rect so the PPU can center it (BG0 composes
     * the box for a 240-px canvas). The engine frame (DispMessageFrame /
     * DeleteWindow, src/message.c) spans (W+2) x (H+2) BG0 tiles STARTING at
     * tile (textWindowPosX, textWindowPosY) — border tiles are drawn inward
     * from that corner, not around it. Publishing a rect short of the real
     * frame left the right border outside the shifted copy (overdrawn by the
     * interior) and the bottom border row outside the y-band (torn by the
     * HUD right-anchor remap). Clamp to the native canvas. */
    {
        extern int Port_Message_WindowRect(int*, int*, int*, int*);
        int x0, y0, x1, y1;
        if ((gMessage.state & MESSAGE_ACTIVE) != 0 && Port_Message_WindowRect(&x0, &y0, &x1, &y1)) {
            if (x0 < 0)
                x0 = 0;
            if (x1 > 240)
                x1 = 240;
            if (y0 < 0)
                y0 = 0;
            if (y1 > 160)
                y1 = 160;
            if (x1 > x0 && y1 > y0) {
                virtuappu_mode1_ws_msg_x0 = x0;
                virtuappu_mode1_ws_msg_x1 = x1;
                virtuappu_mode1_ws_msg_y0 = y0;
                virtuappu_mode1_ws_msg_y1 = y1;
                virtuappu_mode1_ws_msg_shift = (Port_Widescreen_EffectiveViewWidth() - 240) / 2;
            }
        } else if (enterRoomBannerActive) {
            /* The location-name banner bypasses gMessage and owns BG0 rows 5-6
             * across the full native canvas. Move that whole band together;
             * this also suspends the HUD right-anchor remap on those rows, which
             * otherwise cuts a viewWidth-240 gap through the banner text. */
            virtuappu_mode1_ws_msg_x0 = PORT_WS_ENTER_ROOM_BANNER_X0;
            virtuappu_mode1_ws_msg_x1 = PORT_WS_ENTER_ROOM_BANNER_X1;
            virtuappu_mode1_ws_msg_y0 = PORT_WS_ENTER_ROOM_BANNER_Y0;
            virtuappu_mode1_ws_msg_y1 = PORT_WS_ENTER_ROOM_BANNER_Y1;
            virtuappu_mode1_ws_msg_shift = (Port_Widescreen_EffectiveViewWidth() - 240) / 2;
        }
    }
    virtuappu_mode1_ws_hud_right_anchor = Port_Widescreen_HudRightAnchor();
    virtuappu_mode1_ws_hud_right_native_x = MODE1_WS_HUD_RIGHT_NATIVE_X;
}
#else
void Port_Widescreen_BeginScroll(unsigned room, unsigned direction) {
    (void)room;
    (void)direction;
}
int Port_Widescreen_AdvanceScroll(void) {
    return 0;
}
int Port_Widescreen_PortalCutsceneWide(void) {
    return 0;
}
int Port_Widescreen_CutsceneViewLive(void) {
    return 0;
}
int Port_Widescreen_TitleForestShown(void) {
    return 0;
}
int Port_Widescreen_StretchEdges(void) {
    return 0;
}
int Port_Widescreen_FileSelectCaveShown(void) {
    return 0;
}
int Port_Widescreen_PauseBackgroundShown(void) {
    return 0;
}
int Port_Widescreen_PauseWide(void) {
    return 0;
}
int Port_Widescreen_PauseViewLive(void) {
    return 0;
}
int Port_Widescreen_TitleWide(void) {
    return 0;
}
int Port_Widescreen_FileSelectWide(void) {
    return 0;
}
int Port_Widescreen_FileSelectViewLive(void) {
    return 0;
}
int Port_Widescreen_TitleViewLive(void) {
    return 0;
}
int Port_Stereo_Scenes(void) {
    return 0;
}
int Port_Widescreen_ContentRects(PortBlurRect* rects, int max) {
    (void)rects;
    (void)max;
    return 0;
}
int Port_Widescreen_CutsceneStrips(int* pictureX, int* pictureY, int* portal) {
    (void)pictureX;
    (void)pictureY;
    (void)portal;
    return 0;
}
void Port_Widescreen_UpdateShadows(void) { /* no-op at native 240 */
}
void Port_Widescreen_SetEnterRoomBannerActive(int active) {
    (void)active;
}
int Port_Widescreen_FallbackNative(void) {
    return 1;
}
int Port_Widescreen_IsActive(void) {
    return 0;
}
int Port_Widescreen_EffectiveViewWidth(void) {
    return 240;
}
int Port_Widescreen_EffectiveViewHeight(void) {
    return 160;
}
int Port_Widescreen_GameplayViewWidth(void) {
    return 240;
}
int Port_Widescreen_GameplayViewHeight(void) {
    return 160;
}
int Port_Widescreen_ProducerViewWidth(void) {
    return 240;
}
int Port_Widescreen_ProducerViewHeight(void) {
    return 160;
}
Port3DSFullViewMode Port_Widescreen_3DSViewMode(void) {
    return PORT_3DS_FULL_VIEW_FALLBACK;
}
Port3DSFullViewFallbackReason Port_Widescreen_3DSFallbackReason(void) {
    return PORT_3DS_FULL_VIEW_REASON_COMBO_DISABLED;
}
int Port_Widescreen_3DSHoldTop(void) {
    return 0;
}
int Port_Widescreen_HudRightAnchor(void) {
    return 0;
}
int Port_Widescreen_HudTargetViewWidth(void) {
    return 240;
}
int Port_Widescreen_HudTargetViewHeight(void) {
    return 160;
}
int Port_Widescreen_ShadowsLive(void) {
    return 0;
}
void Port_Widescreen_SetWindowPixels(int w, int h) {
    (void)w;
    (void)h;
}
int Port_Widescreen_TargetViewWidth(void) {
    return 240;
}
int Port_Widescreen_CameraRestX(int target_x) {
    /* GBA-exact: center at 120, clamp view inside the room. */
    int lo = (int)gRoomControls.origin_x;
    int hi = lo + (int)gRoomControls.width - 240;
    int want = target_x - 120;
    if (want > hi) {
        want = hi;
    }
    if (want < lo) {
        want = lo;
    }
    return want;
}
int Port_Widescreen_CameraRestY(int target_y) {
    int lo = (int)gRoomControls.origin_y;
    int hi = lo + (int)gRoomControls.height - 160;
    int want = target_y - 80;
    if (want > hi) {
        want = hi;
    }
    if (want < lo) {
        want = lo;
    }
    return want;
}
int Port_Widescreen_PrepareGameplayCamera(void) {
    return 0;
}
#endif

/* Touch-overlay helper: 1 when the R button currently has a CONTEXT
 * action (speak/read/check/open/lift/grab/throw/drop/grow/shrink) —
 * i.e. pressing R right now does something specific. ROLL and NONE
 * don't count: roll is available during any walk, so highlighting it
 * would keep the button lit almost permanently and destroy the signal.
 * rActionPlayerState overrides the interact-object frame when set
 * (same precedence the HUD renderer uses). Read by the Android touch
 * overlay to glow the R face button. */
int Port_TouchControls_RActionAvailable(void) {
    unsigned r = gHUD.rActionPlayerState != 0 ? gHUD.rActionPlayerState : gHUD.rActionInteractObject;
    switch (r) {
        case R_ACTION_CANCEL:
        case R_ACTION_DROP:
        case R_ACTION_THROW:
        case R_ACTION_READ:
        case R_ACTION_CHECK:
        case R_ACTION_OPEN:
        case R_ACTION_SPEAK:
        case R_ACTION_GRAB:
        case R_ACTION_LIFT:
        case R_ACTION_GROW:
        case R_ACTION_SHRINK:
            return 1;
        default:
            return 0;
    }
}

/*
 * UpdateSpriteForCollisionLayer — sets OBJ priority bits based on entity's collision layer.
 * (from Thumb asm at 0x08016A04)
 *
 * Table embedded in ROM:
 *   Layer 0: spriteRendering b3=0x80 (priority 2), spriteOrientation flipY=0x80 (priority 2)
 *   Layer 1: same as layer 0
 *   Layer 2: spriteRendering b3=0x40 (priority 1), spriteOrientation flipY=0x40 (priority 1)
 *   Layer 3: same as layer 2
 *
 * This ensures entities on the bottom layer render behind the top BG layer (tree canopy, roofs)
 * and entities on the top layer render in front of the top BG layer.
 */
void UpdateSpriteForCollisionLayer(Entity* entity) {
    if (entity == NULL) {
        return;
    }

    /* On this engine, b3/flipY are 2-bit fields (0..3), not raw bit masks. */
    if (entity->collisionLayer == 2 || entity->collisionLayer == 3) {
        entity->spriteRendering.b3 = 1;
        entity->spriteOrientation.flipY = 1;
    } else {
        entity->spriteRendering.b3 = 2;
        entity->spriteOrientation.flipY = 2;
    }
}

// Area / room data — now provided by src/data/areaMetadata.c
// const AreaHeader gAreaMetadata[256]; // removed: real data in areaMetadata.c
/**
 * GravityUpdate — port of ARM thumb routine at 0x08003FC4.
 * Applies gravity to an entity's z-axis each frame.
 */
u32 GravityUpdate(Entity* entity, u32 gravity) {
    s32 z = (s32)entity->z.WORD - (s32)entity->zVelocity;
    if (z < 0) {
        entity->z.WORD = (u32)z;
        entity->zVelocity = (s32)entity->zVelocity - (s32)gravity;
        return (u32)z;
    } else {
        entity->z.WORD = 0;
        entity->zVelocity = 0;
        return 0;
    }
}

/**
 * BounceUpdate — port of Thumb routine at 0x080044EC.
 * Like GravityUpdate but with bouncing: when entity hits ground,
 * calculates a reduced bounce velocity.
 *
 * Returns: 2 = airborne, 1 = just bounced, 0 = done bouncing
 */
u32 BounceUpdate(Entity* entity, u32 acceleration) {
    s32 z = (s32)entity->z.WORD - (s32)entity->zVelocity;
    if (z < 0) {
        /* Still airborne */
        entity->z.WORD = (u32)z;
        entity->zVelocity = (s32)entity->zVelocity - (s32)acceleration;
        return 2;
    }
    /* Hit ground — calculate bounce */
    entity->z.WORD = 1; /* z=1 signals "just landed" (player can't do certain actions at z!=0) */
    s32 vel = (s32)entity->zVelocity - (s32)acceleration;
    vel = -vel;
    vel >>= 1;
    u32 uvel = (u32)vel;
    uvel = uvel + (uvel >> 2); /* vel * 1.25 — damped bounce */
    u32 result;
    if ((uvel >> 12) >= 0xC) {
        result = 1; /* Still has enough energy to bounce */
    } else {
        result = 0; /* Done bouncing */
        uvel = 0;
    }
    entity->zVelocity = uvel;
    return result;
}

/**
 * GetFacingDirection — port of Thumb routine at 0x080045C4.
 * Calculates direction (0-31 in 5-bit system) from entity A to entity B.
 * Falls through to CalculateDirectionTo in ASM.
 */
u32 GetFacingDirection(Entity* from, Entity* to) {
    return CalculateDirectionTo((s16)from->x.HALF.HI, (s16)from->y.HALF.HI, (s16)to->x.HALF.HI, (s16)to->y.HALF.HI);
}

/**
 * sub_080045B4 — port of Thumb routine at 0x080045B4.
 * Calculates direction from entity position to absolute (targetX, targetY).
 * In ASM: shuffles args then tail-calls ram_CalcCollisionDirection.
 */
u32 sub_080045B4(Entity* entity, u32 targetX, u32 targetY) {
    return CalculateDirectionTo((s16)entity->x.HALF.HI, (s16)entity->y.HALF.HI, (s16)targetX, (s16)targetY);
}

/**
 * EntityInRectRadius — port of Thumb routine at 0x080041A0.
 * Checks if two entities are within rectangular proximity AND share collision layers.
 *
 * Returns 1 if both axis-distance checks pass and entities share layer bits 0-1.
 * A radius of 0 on an axis skips that axis check (always passes).
 */
u32 EntityInRectRadius(Entity* a, Entity* b, u32 xRadius, u32 yRadius) {
    /* Collision layer check: both must share bits 0-1 */
    u8 sharedLayers = a->collisionLayer & b->collisionLayer;
    if ((sharedLayers & 3) == 0)
        return 0;

    /* X axis check */
    if (xRadius != 0) {
        s32 deltaX = (s16)a->x.HALF.HI - (s16)b->x.HALF.HI;
        u32 offsetX = (u32)(deltaX + (s32)xRadius);
        if (offsetX > xRadius * 2)
            return 0;
    }

    /* Y axis check */
    if (yRadius != 0) {
        s32 deltaY = (s16)a->y.HALF.HI - (s16)b->y.HALF.HI;
        u32 offsetY = (u32)(deltaY + (s32)yRadius);
        if (offsetY > yRadius * 2)
            return 0;
    }

    return 1;
}

/**
 * CheckPlayerInRegion — port of Thumb routine at 0x0800293E.
 * Checks whether the player entity is within a room-relative rectangle.
 *
 * The rectangle is centered at (x, y) with half-extents (halfWidth, halfHeight),
 * all relative to the room origin (gRoomControls.origin_x/y).
 */
u32 CheckPlayerInRegion(u32 x, u32 y, u32 halfWidth, u32 halfHeight) {
    s32 playerRelX = (s32)gPlayerEntity.base.x.HALF.HI - (s32)gRoomControls.origin_x;
    s32 playerRelY = (s32)gPlayerEntity.base.y.HALF.HI - (s32)gRoomControls.origin_y;

    /* Unsigned range check: (x - (playerRelX - halfWidth)) must be < 2*halfWidth */
    u32 offsetX = (u32)((s32)x - (playerRelX - (s32)halfWidth));
    if (offsetX >= halfWidth * 2)
        return 0;

    u32 offsetY = (u32)((s32)y - (playerRelY - (s32)halfHeight));
    if (offsetY >= halfHeight * 2)
        return 0;

    return 1;
}

/* ================================================================
 * Tile lookup functions — port of ARM routines in intr.s
 *
 * On GBA these use indirection tables (gActTilePtrs, gMapDataPtrs,
 * gCollisionDataPtrs) that point into gMapBottom / gMapTop fields.
 * On PC we can access the struct fields directly.
 *
 * Layer mapping:
 *   0, 1, 3 → gMapBottom
 *   2       → gMapTop
 *
 * Coordinate conversion (ARM shifts):
 *   lsl #22, lsr #26  ≡  (x & 0x3FF) >> 4  →  tile index 0..63
 * ================================================================ */

static inline MapLayer* GetMapLayerForLayer(u32 layer) {
    return (layer == 2) ? &gMapTop : &gMapBottom;
}

static inline u32 NormalizeTilePos(u32 tilePos) {
    return tilePos & 0x0FFF; /* 64x64 tilemap */
}

static inline u32 TilePosFromRoomTile(u32 roomTileX, u32 roomTileY) {
    return (roomTileX & 0x3F) | ((roomTileY & 0x3F) << 6);
}

/* World pixel → room-relative tile position */
static inline u32 WorldToTilePos(u32 worldX, u32 worldY) {
    u32 roomX = worldX - gRoomControls.origin_x;
    u32 roomY = worldY - gRoomControls.origin_y;
    u32 tileX = ((roomX << 22) >> 26); /* (roomX & 0x3FF) >> 4 */
    u32 tileY = ((roomY << 22) >> 26);
    return tileX + (tileY << 6); /* tileX + tileY * 64 */
}

/* Room pixel → tile position */
static inline u32 RoomToTilePos(u32 roomX, u32 roomY) {
    u32 tileX = ((roomX << 22) >> 26);
    u32 tileY = ((roomY << 22) >> 26);
    return tileX + (tileY << 6);
}

/* ---------- ActTile family ---------- */

/**
 * GetActTileAtTilePos — get act tile at a raw tile position.
 * (port of arm_GetActTileAtTilePos at 0x080B1AE0)
 */
u32 GetActTileAtTilePos(u16 tilePos, u8 layer) {
    MapLayer* ml = GetMapLayerForLayer(layer);
    if (ml == NULL)
        return 0;
    return ml->actTiles[NormalizeTilePos(tilePos)];
}

/**
 * GetActTileAtRoomTile — get act tile at room tile coordinates.
 * (port of arm_GetActTileAtRoomTile)
 */
u32 GetActTileAtRoomTile(u32 roomTileX, u32 roomTileY, u32 layer) {
    u32 tilePos = TilePosFromRoomTile(roomTileX, roomTileY);
    MapLayer* ml = GetMapLayerForLayer(layer);
    if (ml == NULL)
        return 0;
    return ml->actTiles[tilePos];
}

/**
 * GetActTileAtRoomCoords — get act tile at room pixel coordinates.
 * (port of arm_GetActTileAtRoomCoords)
 */
u32 GetActTileAtRoomCoords(u32 roomX, u32 roomY, u32 layer) {
    u32 tilePos = RoomToTilePos(roomX, roomY);
    MapLayer* ml = GetMapLayerForLayer(layer);
    if (ml == NULL)
        return 0;
    return ml->actTiles[NormalizeTilePos(tilePos)];
}

/**
 * GetActTileAtWorldCoords — get act tile at world pixel coordinates.
 * (port of arm_GetActTileAtWorldCoords)
 */
u32 GetActTileAtWorldCoords(u32 worldX, u32 worldY, u32 layer) {
    u32 tilePos = WorldToTilePos(worldX, worldY);
    MapLayer* ml = GetMapLayerForLayer(layer);
    if (ml == NULL)
        return 0;
    return ml->actTiles[NormalizeTilePos(tilePos)];
}

/**
 * GetActTileAtEntity — get act tile under an entity.
 * (port of arm_GetActTileAtEntity)
 */
u32 GetActTileAtEntity(Entity* entity) {
    u32 tilePos = WorldToTilePos(entity->x.HALF.HI, entity->y.HALF.HI);
    MapLayer* ml = GetMapLayerForLayer(entity->collisionLayer);
    if (ml == NULL)
        return 0;
    return ml->actTiles[NormalizeTilePos(tilePos)];
}

/**
 * GetActTileRelativeToEntity — get act tile at entity position + offset.
 * (port of arm_GetActTileRelativeToEntity)
 */
u32 GetActTileRelativeToEntity(Entity* entity, s32 xOffset, s32 yOffset) {
    u32 worldX = (u16)entity->x.HALF.HI + xOffset;
    u32 worldY = (u16)entity->y.HALF.HI + yOffset;
    u32 tilePos = WorldToTilePos(worldX, worldY);
    MapLayer* ml = GetMapLayerForLayer(entity->collisionLayer);
    if (ml == NULL)
        return 0;
    return ml->actTiles[NormalizeTilePos(tilePos)];
}

/**
 * GetActTileForTileType — convert a tileType to its act tile value.
 * (port of arm_GetActTileForTileType at 0x080B1B54)
 *
 * tileType < 0x4000 → gMapTileTypeToActTile[tileType]
 * tileType >= 0x4000 → gMapSpecialTileToActTile[tileType - 0x4000]
 */
extern const u8 gMapTileTypeToActTile[];
extern const u8 gMapSpecialTileToActTile[];
extern const u16 gUnk_080B7A3E[];
u32 GetActTileForTileType(u32 tileType) {
    if (tileType < 0x4000)
        return GetMapTileTypeToActTile(tileType);
    else
        return gMapSpecialTileToActTile[tileType - 0x4000];
}

/* ---------- CollisionData family ---------- */

/**
 * GetCollisionDataAtTilePos — get collision byte at a raw tile position.
 * (port of arm_GetCollisionDataAtTilePos)
 */
u32 GetCollisionDataAtTilePos(u32 tilePos, u32 layer) {
    MapLayer* ml = GetMapLayerForLayer(layer);
    if (ml == NULL)
        return 0;
    return ml->collisionData[NormalizeTilePos(tilePos)];
}

u32 GetCollisionDataAtRoomTile(u32 roomTileX, u32 roomTileY, u32 layer) {
    return GetCollisionDataAtTilePos(TilePosFromRoomTile(roomTileX, roomTileY), layer);
}

u32 GetCollisionDataAtRoomCoords(u32 roomX, u32 roomY, u32 layer) {
    u32 tilePos = RoomToTilePos(roomX, roomY);
    MapLayer* ml = GetMapLayerForLayer(layer);
    if (ml == NULL)
        return 0;
    return ml->collisionData[NormalizeTilePos(tilePos)];
}

u32 GetCollisionDataAtWorldCoords(u32 worldX, u32 worldY, u32 layer) {
    u32 tilePos = WorldToTilePos(worldX, worldY);
    MapLayer* ml = GetMapLayerForLayer(layer);
    if (ml == NULL)
        return 0;
    return ml->collisionData[NormalizeTilePos(tilePos)];
}

u32 GetCollisionDataAtEntity(Entity* entity) {
    u32 tilePos = WorldToTilePos(entity->x.HALF.HI, entity->y.HALF.HI);
    MapLayer* ml = GetMapLayerForLayer(entity->collisionLayer);
    if (ml == NULL)
        return 0;
    return ml->collisionData[NormalizeTilePos(tilePos)];
}

u32 GetCollisionDataRelativeTo(Entity* entity, s32 xOffset, s32 yOffset) {
    u32 worldX = (u16)entity->x.HALF.HI + xOffset;
    u32 worldY = (u16)entity->y.HALF.HI + yOffset;
    u32 tilePos = WorldToTilePos(worldX, worldY);
    MapLayer* ml = GetMapLayerForLayer(entity->collisionLayer);
    if (ml == NULL)
        return 0;
    return ml->collisionData[NormalizeTilePos(tilePos)];
}

/* ---------- TileType family ---------- */

/**
 * GetTileTypeAtTilePos — get tile type at a raw tile position.
 * (port of arm_GetTileTypeAtTilePos at 0x080B1A60)
 *
 * Reads mapData[tilePos] → tileIndex.
 * If tileIndex >= 0x4000 → return tileIndex (special tile reference).
 * Otherwise → return tileTypes[tileIndex].
 */
u32 GetTileTypeAtTilePos(u32 tilePos, u32 layer) {
    MapLayer* ml = GetMapLayerForLayer(layer);
    if (ml == NULL)
        return 0;
    u16 tileIndex = ml->mapData[NormalizeTilePos(tilePos)];
    if (tileIndex >= 0x4000)
        return tileIndex;
    return ml->tileTypes[tileIndex];
}

u32 GetTileTypeAtRoomTile(u32 roomTileX, u32 roomTileY, u32 layer) {
    return GetTileTypeAtTilePos(TilePosFromRoomTile(roomTileX, roomTileY), layer);
}

u32 GetTileTypeAtRoomCoords(u32 roomX, u32 roomY, u32 layer) {
    u32 tilePos = RoomToTilePos(roomX, roomY);
    return GetTileTypeAtTilePos(tilePos, layer);
}

u32 GetTileTypeAtWorldCoords(s32 worldX, s32 worldY, u32 layer) {
    u32 tilePos = WorldToTilePos((u32)worldX, (u32)worldY);
    return GetTileTypeAtTilePos(tilePos, layer);
}

u32 GetTileTypeAtEntity(Entity* entity) {
    u32 tilePos = WorldToTilePos(entity->x.HALF.HI, entity->y.HALF.HI);
    return GetTileTypeAtTilePos(tilePos, entity->collisionLayer);
}

u32 GetTileTypeRelativeToEntity(Entity* entity, s32 xOffset, s32 yOffset) {
    u32 worldX = (u16)entity->x.HALF.HI + xOffset;
    u32 worldY = (u16)entity->y.HALF.HI + yOffset;
    u32 tilePos = WorldToTilePos(worldX, worldY);
    return GetTileTypeAtTilePos(tilePos, entity->collisionLayer);
}

/* ---------- sub_080B1B84 / sub_080B1BA4 — tile data lookup ---------- */

/**
 * sub_080B1B84 — look up tile property data (u16) from gUnk_08000360 table.
 * (port of arm_sub_080B1B84 at 0x080B1B84)
 *
 * Calls GetTileTypeAtTilePos, then indexes into gUnk_08000360 or gUnk_080B7A3E
 * (based on whether tileType < 0x4000 or not) as a u16 array.
 */
u32 sub_080B1B84(u32 tilePos, u32 layer) {
    u32 tileType = GetTileTypeAtTilePos(tilePos, layer);
    const u16* table;
    if (tileType < 0x4000) {
        return Port_GetTileTypeProperty(tileType);
    } else {
        table = gUnk_080B7A3E;
    }
    return table[tileType & 0x3FFF];
}

/**
 * sub_080B1BA4 — like sub_080B1B84 but AND result with a mask (r2).
 * (port of arm_sub_080B1BA4 at 0x080B1BA4)
 *
 * #PC_PORT issue #139: when the lantern queries a tile for the "ignitable"
 * flag (mask 0x40), it passes the player's collisionLayer. In some rooms
 * (Grimblade dojo room 5 area 0x25) the lightable TILE_TYPE_117 tiles only
 * live on LAYER_TOP while the player walks on LAYER_BOTTOM, so the check
 * silently fails. If the queried layer returns 0 for the masked flag, fall
 * back to the OTHER layer — keeps existing behaviour when the property is
 * already present on the queried layer.
 */
u32 sub_080B1BA4(u32 tilePos, u32 layer, u32 mask) {
    u32 tileType = GetTileTypeAtTilePos(tilePos, layer);
    u32 r;
    if (tileType < 0x4000) {
        r = Port_GetTileTypeProperty(tileType) & mask;
    } else {
        r = gUnk_080B7A3E[tileType & 0x3FFF] & mask;
    }
#ifdef PC_PORT
    if (r == 0 && Port_ShouldFallbackTilePropertyLayer(mask)) {
        u32 other = (layer == 2) ? 1 : 2;
        u32 tt2 = GetTileTypeAtTilePos(tilePos, other);
        r = (tt2 < 0x4000) ? (Port_GetTileTypeProperty(tt2) & mask)
                           : (gUnk_080B7A3E[tt2 & 0x3FFF] & mask);
    }
#endif
    return r;
}

/* ---------- SetCollisionData, SetActTileAtTilePos, GetTileIndex, SetTile, CloneTile ---------- */

extern const u8 gMapSpecialTileToActTile[];
extern const u8 gMapSpecialTileToCollisionData[];
extern const u8 gMapTileTypeToCollisionData[];
extern void UnregisterInteractTile(u32, u32);
extern void RegisterInteractTile(u32, u32, u32);

/**
 * SetCollisionData — set collision data at a tile position.
 * (port of Thumb veneer at 0x08000148)
 *
 * gCollisionDataPtrs layout (by layer):
 *   0 → gMapBottom.collisionData
 *   1 → gMapBottom.collisionData
 *   2 → gMapTop.collisionData
 *   3 → gMapBottom.collisionData
 */
void SetCollisionData(u32 collisionData, u32 tilePos, u32 layer) {
    MapLayer* ml = GetMapLayerForLayer(layer);
    if (ml == NULL)
        return;
    ml->collisionData[NormalizeTilePos(tilePos)] = collisionData;
}

/**
 * SetActTileAtTilePos — set act tile at a tile position.
 * (port of Thumb veneer at 0x080001D0)
 *
 * gActTilePtrs layout (by layer):
 *   0 → gMapBottom.actTiles
 *   1 → gMapBottom.actTiles
 *   2 → gMapTop.actTiles
 *   3 → gMapBottom.actTiles
 */
void SetActTileAtTilePos(u32 actTile, u32 tilePos, u32 layer) {
    MapLayer* ml = GetMapLayerForLayer(layer);
    if (ml == NULL)
        return;
    ml->actTiles[NormalizeTilePos(tilePos)] = actTile;
}

/**
 * GetTileIndex — get the tile index (mapData value) at a tile position.
 * (port of Thumb veneer at 0x080001DA)
 *
 * Returns mapData[tilePos] for the specified layer.
 */
u32 GetTileIndex(u32 tilePos, u32 layer) {
    MapLayer* ml = GetMapLayerForLayer(layer);
    if (ml == NULL)
        return 0;
    return ml->mapData[NormalizeTilePos(tilePos)];
}

/**
 * SetTile — set a tile at a position and update collision/actTile data.
 * (port of Thumb veneer at 0x0800015C)
 *
 * If tileIndex >= 0x4000 (special tile):
 *   - Uses gMapSpecialTileToActTile / gMapSpecialTileToCollisionData
 *   - Calls UnregisterInteractTile + RegisterInteractTile for tile entity management
 * Else (normal tile):
 *   - Looks up tileType from tileTypes[tileIndex]
 *   - Uses gMapTileTypeToActTile / gMapTileTypeToCollisionData
 *   - Calls UnregisterInteractTile
 */
void SetTile(u32 tileIndex, u32 tilePos, u32 layer) {
    MapLayer* ml = GetMapLayerForLayer(layer);
    u32 pos = NormalizeTilePos(tilePos);
    u16 oldTile;

    if (ml == NULL)
        return;

    oldTile = ml->mapData[pos];
    ml->mapData[pos] = (u16)tileIndex;

    if (tileIndex >= 0x4000) {
        /* Special tile */
        u32 specialIdx = tileIndex - 0x4000;
        SetActTileAtTilePos(gMapSpecialTileToActTile[specialIdx], pos, layer);
        SetCollisionData(gMapSpecialTileToCollisionData[specialIdx], pos, layer);
        UnregisterInteractTile(pos, layer);
        RegisterInteractTile(oldTile, pos, layer);
    } else {
        /* Normal tile — look up tile type from tileTypes table */
        u16 tileType = ml->tileTypes[tileIndex];
        SetActTileAtTilePos(GetMapTileTypeToActTile(tileType), pos, layer);
        SetCollisionData(gMapTileTypeToCollisionData[tileType], pos, layer);
        UnregisterInteractTile(pos, layer);
    }
}

/**
 * CloneTile — look up the tile index from a tile type and call SetTile.
 * (port of Thumb veneer at 0x08000152)
 *
 * Looks up tileIndices[tileType] for the given layer to get the actual tile index,
 * then falls through to SetTile(tileIndex, tilePos, layer).
 */
void CloneTile(u32 tileType, u32 tilePos, u32 layer) {
    MapLayer* ml = GetMapLayerForLayer(layer);
    u16 tileIndex = ml->tileIndices[tileType];
    SetTile(tileIndex, tilePos, layer);
}

/* ---------- ResolveCollisionLayer, CheckOnLayerTransition, UpdateCollisionLayer ---------- */

/**
 * Transition tile table entry for layer transitions.
 * (from ARM asm at 0x08016A90 / gTransitionTiles)
 */
typedef struct {
    u16 actTile;
    u8 preservedLayer;
    u8 destinationLayer;
} TransitionTileEntry;

static const TransitionTileEntry sTransitionTiles[] = {
    { 0x0A, 2, 1 }, { 0x09, 2, 1 }, { 0x0C, 1, 2 }, { 0x0B, 1, 2 },
    { 0x52, 3, 3 }, { 0x27, 3, 3 }, { 0x26, 3, 3 }, { 0x0000, 0, 0 }, /* terminator */
};

/**
 * Table of act tiles to check for ResolveCollisionLayer.
 * (from ARM asm above gTransitionTiles)
 */
static const TransitionTileEntry sResolveCollisionLayerTiles[] = {
    { 0x2A, 3, 3 }, { 0x2D, 3, 3 }, { 0x2B, 3, 3 }, { 0x2C, 3, 3 },   { 0x4C, 3, 3 },
    { 0x4E, 3, 3 }, { 0x4D, 3, 3 }, { 0x4F, 3, 3 }, { 0x0000, 0, 0 }, /* terminator */
};

/**
 * ResolveCollisionLayer — determine which collision layer an entity should be on
 * based on the tile under it.
 * (port of ARM asm at 0x08016A30)
 */
u32 ResolveCollisionLayer(Entity* entity) {
    if (entity->collisionLayer == 0) {
        u32 tileType = GetTileTypeAtWorldCoords(entity->x.HALF.HI, entity->y.HALF.HI, 2);
        u8 newLayer = 1;
        if (tileType != 0) {
            u32 actTile = GetActTileForTileType(tileType);
            newLayer = 2;
            /* Check against the resolve table */
            const TransitionTileEntry* p = sResolveCollisionLayerTiles;
            while (p->actTile != 0) {
                if (actTile == p->actTile) {
                    newLayer = p->destinationLayer;
                    break;
                }
                p++;
            }
        }
        entity->collisionLayer = newLayer;
    }
    UpdateSpriteForCollisionLayer(entity);
    return 0;
}

/**
 * CheckOnLayerTransition — check if entity is standing on a layer-transition tile.
 * (port of ARM asm at 0x08016A68)
 *
 * Returns the act tile at the entity's position (preserved in r0 in ARM code).
 */
u32 CheckOnLayerTransition(Entity* entity) {
    u32 actTile = GetActTileAtEntity(entity);
    const TransitionTileEntry* p = sTransitionTiles;
    while (p->actTile != 0) {
        if (p->actTile == actTile) {
            entity->collisionLayer =
                Port_ApplyCollisionLayerTransition(entity->collisionLayer, p->preservedLayer, p->destinationLayer);
            return actTile;
        }
        p++;
    }
    return actTile;
}

/**
 * UpdateCollisionLayer — check layer transition and update sprite priority.
 * (port of ARM asm at 0x08016AB4)
 */
u32 UpdateCollisionLayer(Entity* entity) {
    u32 actTile = CheckOnLayerTransition(entity);
    UpdateSpriteForCollisionLayer(entity);
    return actTile;
}

/**
 * GetTileHazardType — check if the entity is standing on a hazardous tile.
 * (port of ARM asm at 0x080043E8)
 *
 * Hazard list:
 *   0x0D → 1 (hole)
 *   0x10 → 2 (lava)
 *   0x11 → 2 (lava)
 *   0x5A → 3 (swamp)
 *   0x13 → 4 (water/drown)
 *
 * Returns 0 if entity has no action, is in air (z < 0), or not on a hazard tile.
 */
u32 GetTileHazardType(Entity* entity) {
    static const struct {
        u16 actTile;
        u16 hazardType;
    } sHazardList[] = {
        { 0x0D, 1 }, { 0x10, 2 }, { 0x11, 2 }, { 0x5A, 3 }, { 0x13, 4 }, { 0x00, 0 }, /* terminator */
    };

    if (entity->action == 0)
        return 0;

    u32 actTile = UpdateCollisionLayer(entity);

    /* Check z position — if entity is in the air, no hazard */
    if ((s16)entity->z.HALF.HI < 0)
        return 0;

    if (actTile == 0)
        return 0;

    for (int i = 0; sHazardList[i].actTile != 0; i++) {
        if (actTile == sHazardList[i].actTile)
            return sHazardList[i].hazardType;
    }
    return 0;
}

RoomHeader* gAreaRoomHeaders[256];
void* gAreaRoomMaps[256];
void* gAreaTable[256];
void* gAreaTileSets[256];
void* gAreaTiles[256];

// Function pointer tables
/* gSubtasks -- defined in port_stubs.c as proper function pointer table */
// ButtonUIElement_Actions — defined in ui.c with proper function pointers
// EzloNagUIElement_Actions — defined in ui.c with proper function pointers
// gUIElementDefinitions — defined in ui.c with proper UIElementDefinition type

/* Native function-pointer tables — the original `void*[16]` zero-stubs were
 * an unfinished placeholder. On GBA the table is 5 packed 4-byte function
 * pointers in ROM; the C dispatcher (`Subtask_FastTravel_Functions[idx]()`)
 * strides 8 bytes per index on x86-64 and a NULL stub array means every
 * dispatch hits NULL → SIGSEGV with RIP=0. The fix mirrors gleerok 9d5f55a5
 * — a real native array of decompiled C functions. */
extern void Subtask_FastTravel_0(void);
extern void Subtask_FastTravel_1(void);
extern void Subtask_FastTravel_2(void);
extern void Subtask_FastTravel_3(void);
extern void Subtask_FastTravel_4(void);
void (*const Subtask_FastTravel_Functions[])(void) = {
    Subtask_FastTravel_0, Subtask_FastTravel_1, Subtask_FastTravel_2, Subtask_FastTravel_3, Subtask_FastTravel_4,
};
/* Subtask_MapHint_Functions — the matching usage in src/subtask/subtaskMapHint.c
 * defines a *static local* array of the same name with proper native
 * function pointers, so this global is dead code. Kept only because
 * stubs_autogen.c still mentions the symbol; once stubs_autogen is
 * regenerated the entire `void* []` line below can go away. */
void* Subtask_MapHint_Functions[16];

// Exit lists / transitions — now provided by src/data/transitions.c
// gExitLists and gExitList_RoyalValley_ForestMaze removed

// Various game data
u32 gFixedTypeGfxData[528];
_Static_assert(ARRAY_COUNT(gFixedTypeGfxData) == PORT_FIXED_TYPE_GFX_CAPACITY_ENTRIES,
               "fixed-gfx staging capacity must match the runtime bounds");
// gCaveBorderMapData — now provided by src/data/caveBorderMapData.c
// gOverworldLocations — now provided by src/data/areaMetadata.c
u16* gMoreSpritePtrs[16];
u8 gExtraFrameOffsets[4352];
u8 gShakeOffsets[256];
u16 gDungeonNames[64];
/* gFigurines — now provided by port/port_figurines.c (Figurine[137] table,
 * resolved from ROM after Port_LoadRom). The 512-byte stub here used to
 * SEGV the figurine viewer (#57): every fig->pal / fig->gfx was NULL. */
void* gLilypadRails[32];
// gMapActTileToSurfaceType — now provided by src/data/mapActTileToSurfaceType.c
/* gPalette_549 is the start of a 26-palette contiguous block in the GBA
 * `gfxAndPalettes` blob. Mt Crenel's weather-change manager reads
 * `gPalette_549 + 0xD0` (i.e., 13 palettes past the start) as `palette2`
 * for cross-fade — which only works because the GBA linker placed
 * gPalette_549..gPalette_574 sequentially. The PC port previously stubbed
 * this as a single 32-byte buffer, so the +0xD0 read walked off into garbage
 * and the Mt Crenel mountaintop terrain rendered with random colors (#34).
 * Allocate the full 416-color (832-byte) block; port_rom.c populates it
 * from gGlobalGfxAndPalettes after ROM load. */
u16 gPalette_549[0x1A0];
u32* gTranslations[16];
// gWallMasterScreenTransitions — now provided by src/data/screenTransitions.c
u16* gZeldaFollowerText[8];
Frame* gSpriteAnimations_322[128];
u32 gSpriteAnimations_GhostBrothers[64];
DiggingCaveEntranceTransition gDiggingCaveEntranceTransition;
u8 RupeeKeyDigits[16];

// Player macros — now provided by src/data/data_080046A4.c

// Entity data (ROM data — all zero-init stubs).
// On Linux the LTO type gate (pc_lto_type_check) compares these against the
// game's `extern EntityData name` declarations, so back a correctly-typed
// EntityData symbol with oversized storage via an ELF alias. Mach-O (macOS)
// has no `alias` attribute — and runs no gate — so define plain oversized
// storage there (the pre-gate form; the linker matches purely by name).
/* The Linux alias gives debuggers a typed EntityData view of the storage.
 * Under ASan the alias's 16-byte type poisons a redzone INSIDE the real
 * storage, so every sentinel-walk past the first entry (room.c
 * LoadRoomEntityList) and the Port_InitDataStubs memcpy report a bogus
 * global-buffer-overflow. Use the plain byte array there. */
#if defined(__SANITIZE_ADDRESS__)
#define TMC_ASAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TMC_ASAN_BUILD 1
#endif
#endif
#if defined(__linux__) && !defined(TMC_ASAN_BUILD)
#define ENTITY_DATA_STUB(name, bytes)                     \
    u8 name##_storage[bytes] __attribute__((aligned(4))); \
    extern EntityData name __attribute__((alias(#name "_storage")))
#else
#define ENTITY_DATA_STUB(name, bytes) u8 name[bytes] __attribute__((aligned(4)))
#endif
/* Real ROM lists are 80 and 96 bytes (4 and 5 entities + 0xFF terminator);
 * 64 bytes cut the terminator off both. */
ENTITY_DATA_STUB(Entities_HouseInteriors1_Mayor_080D6210, 80);
ENTITY_DATA_STUB(Entities_MinishPaths_MayorsCabin_gUnk_080D6138, 96);
ENTITY_DATA_STUB(UpperInn_Din, 64);
ENTITY_DATA_STUB(UpperInn_Farore, 64);
ENTITY_DATA_STUB(UpperInn_Nayru, 64);
ENTITY_DATA_STUB(UpperInn_NoDin, 64);
ENTITY_DATA_STUB(UpperInn_NoFarore, 64);
ENTITY_DATA_STUB(UpperInn_NoNayru, 64);
ENTITY_DATA_STUB(UpperInn_Oracles, 64);
ENTITY_DATA_STUB(gUnk_additional_8_DeepwoodShrine_StairsToB1, 64);
ENTITY_DATA_STUB(gUnk_additional_8_HouseInteriors1_Library1F, 64);
ENTITY_DATA_STUB(gUnk_additional_8_HouseInteriors3_BorlovEntrance, 64);
ENTITY_DATA_STUB(gUnk_additional_8_HyruleCastle_3, 64);
/* gUnk_additional_8_MelarisMine_Main needs at least 96 bytes for the
 * post-state-change entity list (2 mountain minishes + Melari himself
 * + 2 cutscene-sword objects + terminator). The smaller default size
 * truncated past Melari, leaving the room with garbage entities the
 * runtime parsed as kind=GROUND_ITEM (visible as the "double heart
 * containers" in #42). */
ENTITY_DATA_STUB(gUnk_additional_8_MelarisMine_Main, 128);
ENTITY_DATA_STUB(gUnk_additional_8_PalaceOfWinds_GyorgTornado, 64);
/* ASan caught this at startup: Port_InitDataStubs (data_stubs_autogen.c:653)
 * copies 0x50 (80) bytes here from ROM, but the storage was sized 64 — the
 * memcpy overflowed 16 bytes into gUnk_additional_9_HouseInteriors2_Percy. */
ENTITY_DATA_STUB(gUnk_additional_9_HouseInteriors1_Library1F, 128);
ENTITY_DATA_STUB(gUnk_additional_9_HouseInteriors2_Percy, 64);
ENTITY_DATA_STUB(gUnk_additional_9_HouseInteriors3_BorlovEntrance, 64);
ENTITY_DATA_STUB(gUnk_additional_9_MelarisMine_Main, 64);
ENTITY_DATA_STUB(gUnk_additional_9_PalaceOfWinds_GyorgTornado, 64);
ENTITY_DATA_STUB(gUnk_additional_a_CaveOfFlamesBoss_Main, 64);
ENTITY_DATA_STUB(gUnk_additional_a_DeepwoodShrineBoss_Main, 64);
ENTITY_DATA_STUB(gUnk_additional_a_HouseInteriors2_Percy, 64);
ENTITY_DATA_STUB(gUnk_additional_a_HouseInteriors3_BorlovEntrance, 64);
ENTITY_DATA_STUB(gUnk_additional_a_TempleOfDroplets_BigOcto, 64);
ENTITY_DATA_STUB(gUnk_additional_c_HouseInteriors2_Romio, 64);
#undef ENTITY_DATA_STUB
u32 Enemies_LakeHylia_Main;
u32 Area_HyruleTown[16];

// Script data (ROM data — zero-init stubs so linker resolves them)
u16 script_08012C48;
u16 script_08015B14;
u16 script_BedAtSimons;
u16 script_BedInLinksRoom;
#define SCRIPT_STUB(name) Script name
SCRIPT_STUB(script_BombMinish);
SCRIPT_STUB(script_BombMinishKinstone);
u16 script_BusinessScrubIntro[16];
u16 script_CutsceneMiscObjectSwordInChest;
u16 script_CutsceneMiscObjectTheLittleHat;
u16 script_EzloTalkOcarina[16];
SCRIPT_STUB(script_ForestMinish1);
SCRIPT_STUB(script_ForestMinish2);
SCRIPT_STUB(script_ForestMinish3);
SCRIPT_STUB(script_ForestMinish4);
SCRIPT_STUB(script_ForestMinish5);
SCRIPT_STUB(script_ForestMinish6);
SCRIPT_STUB(script_ForestMinish7);
SCRIPT_STUB(script_ForestMinish8);
SCRIPT_STUB(script_ForestMinish9);
SCRIPT_STUB(script_ForestMinish10);
SCRIPT_STUB(script_ForestMinish11);
SCRIPT_STUB(script_ForestMinish12);
SCRIPT_STUB(script_ForestMinish13);
SCRIPT_STUB(script_ForestMinish14);
SCRIPT_STUB(script_ForestMinish15);
SCRIPT_STUB(script_ForestMinish16);
SCRIPT_STUB(script_ForestMinish17);
SCRIPT_STUB(script_ForestMinish18);
SCRIPT_STUB(script_ForestMinish19);
SCRIPT_STUB(script_ForestMinish20);
SCRIPT_STUB(script_ForestMinish21);
#undef SCRIPT_STUB
u16 script_MazaalBossObjectMazaal[16];
u16 script_MazaalMacroDefeated[16];
u8 script_MinishVillageObjectLeftStoneOpening[4];
u8 script_MinishVillageObjectRightStoneOpening[4];
u16 script_PlayerAtDarkNut1[16], script_PlayerAtDarkNut2[16], script_PlayerAtDarkNut3[16];
u16 script_PlayerAtMadderpillar[16];
u16 script_PlayerGetElement[16];
u32 script_PlayerIntro;
void* script_PlayerSleepingInn[8];
u32 script_PlayerWakeAfterRest;
u32 script_PlayerWakingUpAtSimons;
u32 script_PlayerWakingUpInHyruleCastle;
u8 script_Rem[4];
u16 script_Stockwell;
u16 script_StockwellBuy[16];
u16 script_StockwellDogFood[16];
u8 script_TalonGotKey;
u16 script_WindTribespeople6;
u16 script_ZeldaMagic;

// Unk_08133368 — already defined in data_stubs_autogen.c

/* ================================================================
 * Enemy update system — ported from ARM Thumb assembly (enemy.s,
 * code_08001A7C.s, code_080043E8.s)
 * ================================================================ */

extern void DeleteThisEntity(void);
extern u32 EntityDisabled(Entity*);
extern void DrawEntity(Entity*);
extern void Knockback1(Entity*);
extern void Knockback2(Entity*);
extern void EnemyDetachFX(Entity*);
extern void UpdateAnimationVariableFrames(Entity*, u32);
extern void CreatePitFallFX(Entity*);
extern void CreateDrownFX(Entity*);
extern void CreateLavaDrownFX(Entity*);
extern void CreateSwampDrownFX(Entity*);
extern u32 Random(void);
extern void (*gEnemyFunctions[])(Entity*);

/*
 * sub_080028E0 — Decrement iframes towards zero.
 * Port of 0x080028E0 from code_08001A7C.s.
 */
void sub_080028E0(Entity* entity) {
    if (entity->iframes > 0)
        entity->iframes--;
    else if (entity->iframes < 0)
        entity->iframes++;
}

/*
 * GetRandomByWeight — pick a random index from a probability table.
 * Port of 0x080028F4 from code_08001A7C.s.
 */
u32 GetRandomByWeight(const u8* weights) {
    u32 r = Random() & 0xFF;
    u32 i = 0;
    for (;;) {
        r -= weights[i];
        i++;
        if ((s32)r < 0)
            break;
    }
    return i - 1;
}

/*
 * CheckRectOnScreen — test if a rectangle (centred at x,y with
 * half-extents halfW, halfH) overlaps the visible screen.
 * Port of 0x0800290E from code_08001A7C.s.
 */
u32 CheckRectOnScreen(s32 x, s32 y, u32 halfW, u32 halfH) {
    s32 sx = gRoomControls.scroll_x - gRoomControls.origin_x;
    u32 dx = (u32)(x - sx + halfW);
    /* Runtime-gated widescreen: when the WIP option is off or this room
     * falls back to native, cull against 240 so engine behaviour matches the
     * 240x160 frame we present. */
    if (dx >= halfW * 2 + (u32)Port_Widescreen_GameplayViewWidth())
        return 0;
    s32 sy = gRoomControls.scroll_y - gRoomControls.origin_y;
    u32 dy = (u32)(y - sy + halfH);
    if (dy >= halfH * 2 + (u32)Port_Widescreen_GameplayViewHeight())
        return 0;
    return 1;
}
