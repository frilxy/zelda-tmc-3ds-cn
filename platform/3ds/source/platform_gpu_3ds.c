#include "platform_3ds.h"
#include "platform_gpu_3ds.h"
#include "top_view_3ds.h"
#include "port_ppu_gpu_3ds.h"
#include "ppu_gpu_3ds_budget.h"
#include "port_second_screen_3ds.h"
#include "port_widescreen.h"
#include "port_pause_3ds.h"
#include "fade_gpu_3ds.h"
#include "scene_pictures_3ds.h"
#include "stereo_3ds.h"
#include "cpu/mode1.h"

#include <3ds.h>
#include <citro2d.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* The static quad is drawn with the PPU's own shader and attribute info, so it
 * must use the PPU's vertex type -- not a look-alike. A separate struct here
 * silently became wrong the moment the vertex was packed to 16 bytes (UV is
 * int16 scaled by PPU_GPU3DS_UV_SCALE, and the stride shrank), which would have
 * mis-sampled the whole top screen. Aliasing the real type keeps them in step
 * by construction. */
typedef PpuGpu3DSVertex PresentVertex;
static PresentVertex* sPresentQuad;
static bool sPresentQuadValid;
static float sPresentQuadW, sPresentQuadH;
static unsigned sPresentQuadWidth;

/* Declared locally: port_runtime_config.h pulls in port_types.h, whose
 * u32/s32 collide with libctru's. */
bool Port_Config_GpuStaticQuad(void);
bool Port_Config_CompactUpload(void);
bool Port_Config_BottomRgb565(void);

static C3D_RenderTarget* sTopTarget;
/* Stereoscopic 3D (stereo_3ds.h): the right eye's target, made the first time it is needed and
 * linked to the screen only while 3D is on; sEyeTarget is the one the top is being drawn on,
 * sRightEye the second pass of a frame (no upload, no counting). */
static C3D_RenderTarget* sTopRightTarget;
static bool sStereoLinked;
static C3D_RenderTarget* sEyeTarget;
static bool sRightEye;
static u32 sTopOutputFlags;
static C3D_RenderTarget* sBottomTarget;
static C3D_Tex sTopTexture;
static C3D_Tex sBottomTexture;
static C3D_Tex sSharpBilinearTexture;
static C3D_RenderTarget* sSharpBilinearTarget;
static Tex3DS_SubTexture sTopSubtexture;
static Tex3DS_SubTexture sSharpBilinearSubtexture;
static Tex3DS_SubTexture sBottomSubtexture;
static uint32_t* sTopUpload;
static uint32_t* sBottomUploads[2];
static void* sC2dFlushBase;
static size_t sC2dFlushSize;
static C3D_Tex sUpdateTexture;
static uint32_t* sUpdatePixels;
static bool sUpdateReady;
/* The updater can replace the LCD output without modifying the held game targets. */
static C3D_RenderTarget* sPauseUpdateTargets[2];
static bool sPauseUpdateLinked;
static bool sFrameActive;

/* Inside a frame C3D_SyncDisplayTransfer splits the command list with flags 0, and GSP then runs the PPU's
 * draws without cleaning them from the CPU cache: a draw word still dirty in the ARM11's L1 is read as an
 * older frame's, and a sprite vanishes for a frame (the Zz bubble, one tile). Splitting here first, with
 * GX_CMDLIST_FLUSH, sends the draws so far cleaned; the transfer's own split then has nothing left (offset 0),
 * so the GX queue gets no extra entry. */
static void SplitFlushed(void) {
    if (sFrameActive) C3D_FrameSplit(GX_CMDLIST_FLUSH);
}
static bool sReady;
static bool sOld3DSProfile;
static bool sBottomTargetValid;
static PlatformGpu3DSUploadLayout sUploadLayout;
static PlatformGpu3DSStats sStats;
static unsigned sTopPresentWidth = 240;
static unsigned sTopPresentHeight = 160;
static unsigned sTopValidSourceWidth = 240;
static unsigned sTopValidSourceHeight = 160;
static Port3DSFullViewMode sTopPresentMode = PORT_3DS_FULL_VIEW_FALLBACK;
static int sTopCropX;
static int sTopCropY;

enum {
    TOP_TEXTURE_WIDTH = 512,
    TOP_TEXTURE_HEIGHT = 256,
    SHARP_BILINEAR_TEXTURE_WIDTH = 1024,
    SHARP_BILINEAR_TEXTURE_HEIGHT = 512,
};

_Static_assert(SHARP_BILINEAR_TEXTURE_WIDTH >= 266 * 2 + 1,
               "Bilinear target must hold Wide plus its guard column");
_Static_assert(SHARP_BILINEAR_TEXTURE_HEIGHT >= 160 * 2 + 1,
               "Bilinear target must hold the frame plus its guard row");

extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;
extern bool Port_Config_GetShowFps(void);
extern bool Port_Config_GetColorCorrection(void);


extern void Port_FadedBlackRGB(uint8_t* r, uint8_t* g, uint8_t* b); /* src/fade.c */

/* What the bars beside a narrow view are filled with: black, or the fade's
 * colour while the game fades (a door's white fade takes the whole screen). */
static u32 TopClearColor(void) {
    uint8_t r, g, b;
    /* The bars take the fade the picture beside them shows: the GPU's blend toward grey when the frame
     * used it (black becomes level * weight / 255), else the shown frame's palette fade. Port_FadedBlackRGB
     * is the game's next step, a frame ahead of the picture, which made the bars fade on their own. */
    const unsigned weight = PortPpuGpu3DS_FadeWeight();
    if (weight != 0) {
        r = g = b = (uint8_t)((PortPpuGpu3DS_FadeLevel() * weight + 127u) / 255u);
    } else if (!FadeGpu3DS_ShownBlack(&r, &g, &b)) {
        Port_FadedBlackRGB(&r, &g, &b);
    }
    /* The picture's target keeps 5 bits a channel by dropping the low three; the screen's 5-bit
     * conversion of an 8-bit clear colour rounds the other way, so a fade's plateau (a grey of 200) was
     * one step darker in the bars than in the picture. Hand it the smallest 8-bit value of the same step. */
#define BAR_STEP(v) ((uint8_t)((((unsigned)(v) >> 3u) * 255u + 30u) / 31u))
    return C2D_Color32(BAR_STEP(r), BAR_STEP(g), BAR_STEP(b), 255);
#undef BAR_STEP
}
extern bool Port_Config_GetWideAi(void);
extern int Port_Config_Get3DSAspectRatio(void);
extern int Port_Config_Get3DSDisplayStyle(void);
extern bool Port_Config_3DSFullViewComboEnabled(void);
extern bool Port_Config_GpuFrameSync(void);
extern double Port_PPU_3DS_CurrentFps(void);

static const uint8_t* StatusGlyph(char c) {
    static const uint8_t digits[10][7] = {
        { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 },
        { 14, 17, 1, 2, 4, 8, 31 },     { 30, 1, 1, 14, 1, 1, 30 },
        { 2, 6, 10, 18, 31, 2, 2 },     { 31, 16, 16, 30, 1, 1, 30 },
        { 14, 16, 16, 30, 17, 17, 14 }, { 31, 1, 2, 4, 8, 8, 8 },
        { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 1, 14 },
    };
    static const uint8_t letters[12][7] = {
        { 14, 17, 17, 31, 17, 17, 17 }, /* A */
        { 30, 17, 17, 17, 17, 17, 30 }, /* D */
        { 31, 16, 16, 30, 16, 16, 31 }, /* E */
        { 31, 16, 16, 30, 16, 16, 16 }, /* F */
        { 17, 27, 21, 21, 17, 17, 17 }, /* M */
        { 30, 17, 17, 30, 16, 16, 16 }, /* P */
        { 15, 16, 16, 14, 1, 1, 30 },   /* S */
        { 17, 17, 17, 17, 17, 17, 14 }, /* U */
        { 17, 17, 17, 17, 17, 10, 4 },  /* V */
        { 14, 17, 16, 16, 16, 17, 14 }, /* C */
        { 14, 17, 16, 23, 17, 17, 15 }, /* G */
        { 16, 16, 16, 16, 16, 16, 31 }, /* L */
    };
    static const uint8_t letterIds[26] = {
        /* A    B    C  D  E  F   G */
        0,   255,   9, 1, 2, 3, 10, 255, 255, 255, 255, 11, 4,
        255, 255, 5, 255, 255, 6, 255, 7, 8, 255, 255, 255, 255,
    };
    static const uint8_t dot[7] = { 0, 0, 0, 0, 0, 12, 12 };
    if (c == '.') return dot;
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'A' && c <= 'Z') {
        uint8_t id = letterIds[c - 'A'];
        if (id != 255) return letters[id];
    }
    return NULL;
}


static void DrawStatusTextColor(float x, float y, float scale, const char* text, uint32_t color) {
    for (; *text; ++text, x += 6.0f * scale) {
        const uint8_t* glyph = StatusGlyph(*text);
        if (!glyph) continue;
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5;) {
                if ((glyph[row] & (1u << (4 - col))) == 0) {
                    ++col;
                    continue;
                }
                int end = col + 1;
                while (end < 5 && (glyph[row] & (1u << (4 - end))) != 0) ++end;
                C2D_DrawRectSolid(x + col * scale, y + row * scale, 0.8f,
                                  (end - col) * scale, scale, color);
                col = end;
            }
        }
    }
}

static void DrawStatusText(float x, float y, float scale, const char* text) {
    DrawStatusTextColor(x, y, scale, text, C2D_Color32(255, 255, 255, 255));
}

static void DrawFpsOverlay(void);
static void DrawScenePictures(int frameX, int frameY, int frameW, int frameH, bool abgrAfter);

static u32 TextureTransfer(void) {
    return GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
           GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
           GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
           GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
}

/* Report Task 3 wants the bottom screen in RGB565. Tried and it does not work
 * here, for a reason worth recording so it is not attempted again.
 *
 * GX converts formats in hardware, so in principle the texture could be RGB565
 * while the painter keeps writing 32-bit -- halving the texture's VRAM and the
 * transfer's write bandwidth, which is GSP work on core 1 where the audio
 * worker lives, at no CPU cost. But the painter writes **ABGR**, and
 * ConfigureAbgrTextureEnv un-swizzles it at sample time by reading *alpha as
 * red*. RGB565 has no alpha, so the conversion discards the channel carrying
 * red and the screen comes out red. Verified on an emulator.
 *
 * Making this work needs the painter to emit true RGBA8 -- a format migration
 * across 9000 lines and 85 signatures -- and even then only the transfer would
 * shrink: at 512 KB in 17.2 ms the painter is compute-bound at ~30 MB/s, so its
 * pixel loops would not speed up. Left switchable and off. */
static bool sBottomIsRgb565;

static u32 BottomTextureTransfer(void) {
    if (!sBottomIsRgb565) return TextureTransfer();
    return GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) |
           GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
           GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
           GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
}

static void ConfigureAbgrTextureEnv(void) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env, C2D_Color32(255, 0, 0, 255));

    env = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_B, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MULTIPLY_ADD);
    C3D_TexEnvColor(env, C2D_Color32(0, 255, 0, 255));

    env = C3D_GetTexEnv(2);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MULTIPLY_ADD);
    C3D_TexEnvColor(env, C2D_Color32(0, 0, 255, 255));
}

/* The first Bilinear pass must preserve the upload texture's ABGR channel
 * order. The existing three-stage conversion is then applied exactly once,
 * when the intermediate texture is drawn to the physical top target. */
static void ConfigureIdentityTextureEnv(void) {
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    C3D_TexEnvInit(C3D_GetTexEnv(2));
}

/* sTopTexture is uploaded as the port's ABGR carrier format: its texture
 * alpha component contains the red colour channel, not transparency.  The
 * Bilinear pre-pass must therefore overwrite the intermediate target instead
 * of applying Citro2D's normal source-alpha blend.  Otherwise black pixels
 * (red == 0) become transparent and leave the previous frame behind, which
 * erases text-box fills, outlines and shadows and creates motion trails.
 *
 * Preserve that carrier alpha in the render target with ONE/ZERO; the final
 * pass then performs the established ABGR conversion and uses normal alpha
 * blending on the physical top target. */
static void ConfigureOpaqueOverwriteBlend(void) {
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

static void ConfigureStandardAlphaBlend(void) {
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
}

bool PlatformGpu3DS_Init(bool old3dsProfile) {
    memset(&sStats, 0, sizeof(sStats));
    sOld3DSProfile = old3dsProfile;
    /* MUST match the expression in Port_PPU_Init exactly: that one gives the
     * pitch to the painter, this one sizes the allocation and the transfer. */
    sUploadLayout = PlatformGpu3DS_GetUploadLayout(old3dsProfile && Port_Config_CompactUpload());
    /* Allocated at the full size whatever the layout, so COMPACT UP can be
     * switched during play (PlatformGpu3DS_SetCompactUpload): the compact
     * layout only uses less of the same buffers. */
    const PlatformGpu3DSUploadLayout fullLayout = PlatformGpu3DS_GetUploadLayout(false);
    const size_t topBytes =
        (size_t)fullLayout.topPitch * fullLayout.topRows * sizeof(uint32_t);
    const size_t bottomBytes =
        (size_t)fullLayout.bottomPitch * fullLayout.bottomRows * sizeof(uint32_t);
    sBottomTargetValid = false;
    sSharpBilinearTarget = NULL;
    sC2dFlushBase = NULL;
    sC2dFlushSize = 0;
    sTopUpload = (uint32_t*)linearMemAlign(topBytes, 0x80);
    /* Four vertices for the optional static-quad presenter (report Task 2). */
    sPresentQuad = (PresentVertex*)linearMemAlign(4 * sizeof(PresentVertex), 0x80);
    sPresentQuadValid = false;
    sBottomUploads[0] = (uint32_t*)linearMemAlign(bottomBytes, 0x80);
    sBottomUploads[1] = (uint32_t*)linearMemAlign(bottomBytes, 0x80);
    if (!sTopUpload || !sBottomUploads[0] || !sBottomUploads[1]) goto fail_linear;
    memset(sTopUpload, 0, topBytes);
    memset(sBottomUploads[0], 0, bottomBytes);
    memset(sBottomUploads[1], 0, bottomBytes);
    GSPGPU_FlushDataCache(sTopUpload, topBytes);
    GSPGPU_FlushDataCache(sBottomUploads[0], bottomBytes);
    GSPGPU_FlushDataCache(sBottomUploads[1], bottomBytes);
    /* The GPU renderer runs on both models and needs the larger command buffer. */
    if (!C3D_Init(PPU_GPU3DS_COMMAND_BUFFER_BYTES))
        goto fail_linear;
    /* Every rectangle of the numbers box is one object: past the limit citro2d silently drops the
     * rest of the frame's draws, and the bottom screen went black on the frames with wide digits. */
    if (!C2D_Init(512)) {
        C3D_Fini();
        goto fail_linear;
    }
    C2D_Prepare();
    C3D_BufInfo* c2dBuffers = C3D_GetBufInfo();
    if (c2dBuffers && c2dBuffers->bufCount > 0) {
        const u32 heapPhysical = osConvertVirtToPhys((void*)__ctru_linear_heap);
        const u32 vertexPhysical = c2dBuffers->base_paddr + c2dBuffers->buffers[0].offset;
        const uintptr_t heapStart = (uintptr_t)__ctru_linear_heap;
        const uintptr_t heapEnd = heapStart + __ctru_linear_heap_size;
        const uintptr_t vertexAddress = heapStart + (u32)(vertexPhysical - heapPhysical);
        const uintptr_t flushStart = vertexAddress & ~(uintptr_t)0x7Fu;
        uintptr_t flushEnd = flushStart + 64u * 1024u;
        if (flushEnd > heapEnd) flushEnd = heapEnd;
        if (flushStart >= heapStart && flushStart < flushEnd) {
            sC2dFlushBase = (void*)flushStart;
            sC2dFlushSize = flushEnd - flushStart;
        }
    }
    sStats.linearHeapBytes = __ctru_linear_heap_size;
    sStats.c2dFlushBytes = (uint32_t)sC2dFlushSize;
    sStats.c2dFlushAddress = (uintptr_t)sC2dFlushBase;
    sStats.topUploadPitch = sUploadLayout.topPitch;
    sStats.topUploadBytes = (uint32_t)topBytes;
    sStats.bottomUploadPitch = sUploadLayout.bottomPitch;
    sStats.bottomUploadBytes = (uint32_t)bottomBytes;
    sStats.topUploadAddress = (uintptr_t)sTopUpload;
    sStats.bottomUploadAddress[0] = (uintptr_t)sBottomUploads[0];
    sStats.bottomUploadAddress[1] = (uintptr_t)sBottomUploads[1];
    if (!C3D_TexInitVRAM(&sTopTexture, TOP_TEXTURE_WIDTH, TOP_TEXTURE_HEIGHT, GPU_RGBA8)) goto fail;
    sBottomIsRgb565 = Port_Config_BottomRgb565();
    if (!C3D_TexInitVRAM(&sBottomTexture, 512, 256,
                         sBottomIsRgb565 ? GPU_RGB565 : GPU_RGBA8))
        goto fail_top_texture;
    C3D_TexSetFilter(&sTopTexture, GPU_NEAREST, GPU_NEAREST);
    /* The complete 320x240 compositor (map, HUD and menus) shares this
     * texture, so linear filtering here makes bilinear presentation the
     * default consistently instead of special-casing individual panels. */
    C3D_TexSetFilter(&sBottomTexture, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sTopTexture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetWrap(&sBottomTexture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    /* 266x160 Wide is the largest fallback frame, so this container holds its
     * exact 532x320 nearest-neighbour 2x image plus guard texels. This target
     * has no depth buffer. Allocation failure is non-fatal: Bilinear then
     * falls back to a direct linear-filtered presentation. */
    if (C3D_TexInitVRAM(&sSharpBilinearTexture, SHARP_BILINEAR_TEXTURE_WIDTH,
                        SHARP_BILINEAR_TEXTURE_HEIGHT, GPU_RGBA8)) {
        C3D_TexSetFilter(&sSharpBilinearTexture, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&sSharpBilinearTexture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        sSharpBilinearTarget = C3D_RenderTargetCreateFromTex(
            &sSharpBilinearTexture, GPU_TEXFACE_2D, 0, -1);
        if (!sSharpBilinearTarget) {
            C3D_TexDelete(&sSharpBilinearTexture);
        } else {
            sStats.sharpBilinearAvailable = true;
            sStats.sharpBilinearTargetBytes =
                SHARP_BILINEAR_TEXTURE_WIDTH * SHARP_BILINEAR_TEXTURE_HEIGHT * sizeof(uint32_t);
        }
    }

    sTopTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
    sBottomTarget = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH16);
    if (!sTopTarget || !sBottomTarget) goto fail_targets;
    const u32 output = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                       GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                       GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                       GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_RenderTargetSetOutput(sTopTarget, GFX_TOP, GFX_LEFT, output);
    sTopOutputFlags = output;
    sEyeTarget = sTopTarget;
    C3D_RenderTargetSetOutput(sBottomTarget, GFX_BOTTOM, GFX_LEFT, output);
    sReady = true;
    return true;

fail_targets:
    if (sBottomTarget) C3D_RenderTargetDelete(sBottomTarget);
    if (sTopTarget) C3D_RenderTargetDelete(sTopTarget);
    if (sSharpBilinearTarget) {
        C3D_RenderTargetDelete(sSharpBilinearTarget);
        sSharpBilinearTarget = NULL;
    }
    if (sStats.sharpBilinearAvailable) {
        C3D_TexDelete(&sSharpBilinearTexture);
        sStats.sharpBilinearAvailable = false;
    }
    C3D_TexDelete(&sBottomTexture);
fail_top_texture:
    C3D_TexDelete(&sTopTexture);
fail:
    C2D_Fini();
    C3D_Fini();
fail_linear:
    if (sBottomUploads[1]) linearFree(sBottomUploads[1]);
    if (sBottomUploads[0]) linearFree(sBottomUploads[0]);
    if (sTopUpload) linearFree(sTopUpload);
    sBottomUploads[0] = NULL;
    sBottomUploads[1] = NULL;
    if (sPresentQuad) { linearFree(sPresentQuad); sPresentQuad = NULL; }
    sPresentQuadValid = false;
    sTopUpload = NULL;
    return false;
}

uint32_t* PlatformGpu3DS_TopBuffer(void) { return sTopUpload; }
uint32_t* PlatformGpu3DS_BottomBuffer(unsigned index) {
    return index < 2 ? sBottomUploads[index] : NULL;
}

/* Plain screens at 1x in Full View (Port_Widescreen_StretchEdges: the Nintendo/Capcom logos, the
 * blank frames between the title and file select): the GBA's 240x160 screen sits in the middle of
 * the 400x240 one; its edge columns and rows are stretched to the screen's edges, so its plain
 * background (and the game's fades, which are in the frame) fills the whole screen as one picture.
 * Drawn with the frame's own texture and texture env, right after it, opaque. */
static void DrawFrameEdges(const C2D_Image* frame, const C2D_DrawParams* at) {
    static int stretch; /* the answer for the frame before, for Port_Widescreen_StretchEdges' -1 */
    const int now = Port_Widescreen_StretchEdges();
    if (now >= 0) stretch = now;
    if (at->pos.w != 240.0f || at->pos.h != 160.0f || !stretch) return;
    const Tex3DS_SubTexture* sub = frame->subtex;
    const float texelW = 1.0f / frame->tex->width, texelH = 1.0f / frame->tex->height;
    const float x0 = at->pos.x, y0 = at->pos.y, x1 = x0 + at->pos.w, y1 = y0 + at->pos.h;
    /* columns: left edge, the frame, right edge; rows: top edge, the frame, bottom edge */
    const float cols[3][2] = { { sub->left, sub->left + texelW }, { sub->left, sub->right },
                               { sub->right - texelW, sub->right } };
    const float rows[3][2] = { { sub->top, sub->top - texelH }, { sub->top, sub->bottom },
                               { sub->bottom + texelH, sub->bottom } };
    const float xs[4] = { 0.0f, x0, x1, 400.0f }, ys[4] = { 0.0f, y0, y1, 240.0f };
    /* Opaque: a blank frame (no layer on) leaves its texels' alpha at 0, and the backdrop it shows
     * has to cover the bars all the same. */
    C2D_Flush();
    ConfigureOpaqueOverwriteBlend();
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if ((r == 1 && c == 1) || xs[c + 1] <= xs[c] || ys[r + 1] <= ys[r]) continue;
            const Tex3DS_SubTexture strip = {
                .width = 1, .height = 1,
                .left = cols[c][0], .right = cols[c][1], .top = rows[r][0], .bottom = rows[r][1],
            };
            const C2D_Image image = { .tex = frame->tex, .subtex = &strip };
            const C2D_DrawParams params = {
                .pos = { .x = xs[c], .y = ys[r], .w = xs[c + 1] - xs[c], .h = ys[r + 1] - ys[r] },
                .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
            };
            C2D_DrawImage(image, &params, NULL);
        }
    }
    C2D_Flush();
    ConfigureStandardAlphaBlend();
}

/* ---- the blurred border (FRAME = BLUR) ----
 *
 * An ambilight: the colours of the game's picture, blurred, fill the border it leaves on the 400x240
 * screen, never fading to black (a white screen gets a white border). Near the picture the border is
 * its own pixels (barely blurred); the farther from it, the more diluted, up to a soft average of the
 * whole picture at the screen's edge. Five small VRAM targets (a target switch costs the Old 3DS, so
 * there are as few passes as this takes):
 *   0  the picture shrunk to 64x64 (blended into its previous contents: see below);
 *   1  the whole screen as a 32x16 image: that picture over its rectangle with its edge texels carried
 *      outwards (the clamped wrap), so the corners take the colour of the corner;
 *   2  that shrunk to 16x8, and 3 to 8x8: the diluted picture;
 *   4  the border itself, 64x32: the sum of stages 1 to 3, each scaled by a weight that depends on the
 *      distance from the picture (TopBlurMasks: near the picture stage 1, further out stage 2, then 3). Each level is blurrier than the
 *      one over it, and the 6 px texels with the linear stretch to the screen blend the zones' edges.
 * The small passes copy the source as it is (the port's ABGR carrier or true RGBA), so the final draw
 * uses the env the source needs. They are redone every BLUR_REFRESH frames, once per frame whatever the
 * eye. Pass 0 is blended into the previous result (BLUR_SMOOTH_ALPHA of 255 new), so the glow follows a
 * scroll or a change of scene gently instead of jumping with every frame. Render targets must be at
 * least 8 texels each way (a 4-high one aborts in citro3d). */
enum { BLUR_STAGES = 5, BLUR_BORDER = BLUR_STAGES - 1, BLUR_REFRESH = 2, BLUR_SMOOTH_ALPHA = 40, BLUR_STEREO_INSET = 8, BLUR_NEAR_FALLOFF = 24,
       BLUR_MID_FALLOFF = 72, BLUR_EDGE_INSET = 6 };
typedef struct BlurStage {
    C3D_Tex texture;
    C3D_RenderTarget* target;
    unsigned width, height;
} BlurStage;
/* One set of stages per rectangle the border is made from (the room that shows and the drawn neighbouring rooms:
 * the border grows from each, the nearest one winning). Allocated when first needed. */
enum { BLUR_RECTS = 3 };
typedef struct BlurSet {
    BlurStage stage[BLUR_STAGES];
    C3D_Tex mask[3]; /* the weights of the levels (stages 1 to 3), one small texture each, as big as their stage; all
                        four channels hold the weight, so scaling a stage by one scales its colour and its alpha alike */
    int rect[4];     /* the rectangle it was built for */
    bool ready, valid;
} BlurSet;
static const unsigned kBlurStageSize[BLUR_STAGES][2] = { { 64, 64 }, { 32, 16 }, { 16, 8 }, { 8, 8 }, { 64, 32 } };
static BlurSet sBlurSets[BLUR_RECTS];
/* With more than one set: their borders, each scaled by its weight (the share of the screen it is the nearest
 * rectangle of), added into one. */
static BlurStage sBlurFinal = { .width = 64, .height = 32 };
static C3D_Tex sBlurWeight[BLUR_RECTS];
static bool sBlurTried, sBlurOk, sBlurValid, sBlurFinalReady;
static unsigned sBlurTick;
static int sBlurCount;        /* rectangles of the last build */
static int sBlurWeightsFor[BLUR_RECTS][4]; /* what the weights were made for */

static void TopBlurSetFree(BlurSet* set) {
    for (int i = 0; i < BLUR_STAGES; ++i) {
        if (!set->stage[i].target) continue;
        C3D_RenderTargetDelete(set->stage[i].target);
        C3D_TexDelete(&set->stage[i].texture);
        set->stage[i].target = NULL;
    }
    for (int i = 0; i < 3; ++i) {
        if (!set->mask[i].data) continue;
        C3D_TexDelete(&set->mask[i]);
        set->mask[i].data = NULL;
    }
    set->ready = set->valid = false;
}

static void TopBlurFree(void) {
    for (int j = 0; j < BLUR_RECTS; ++j) {
        TopBlurSetFree(&sBlurSets[j]);
        if (sBlurWeight[j].data) {
            C3D_TexDelete(&sBlurWeight[j]);
            sBlurWeight[j].data = NULL;
        }
    }
    if (sBlurFinal.target) {
        C3D_RenderTargetDelete(sBlurFinal.target);
        C3D_TexDelete(&sBlurFinal.texture);
        sBlurFinal.target = NULL;
    }
    sBlurFinalReady = false;
    sBlurOk = sBlurValid = false;
}

static bool TopBlurStageInit(BlurStage* st) {
    if (!C3D_TexInitVRAM(&st->texture, st->width, st->height, GPU_RGBA8)) return false;
    C3D_TexSetFilter(&st->texture, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&st->texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    st->target = C3D_RenderTargetCreateFromTex(&st->texture, GPU_TEXFACE_2D, 0, -1);
    if (!st->target) {
        C3D_TexDelete(&st->texture);
        return false;
    }
    return true;
}

static bool TopBlurSetEnsure(int j) {
    BlurSet* set = &sBlurSets[j];
    if (set->ready) return true;
    for (int i = 0; i < BLUR_STAGES; ++i) {
        set->stage[i].width = kBlurStageSize[i][0];
        set->stage[i].height = kBlurStageSize[i][1];
        if (!TopBlurStageInit(&set->stage[i])) {
            TopBlurSetFree(set);
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (!C3D_TexInit(&set->mask[i], set->stage[i + 1].width, set->stage[i + 1].height, GPU_RGBA8)) {
            TopBlurSetFree(set);
            return false;
        }
        C3D_TexSetFilter(&set->mask[i], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&set->mask[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    set->ready = true;
    return true;
}

static bool TopBlurInit(void) {
    if (sBlurTried) return sBlurOk;
    sBlurTried = true;
    if (!TopBlurSetEnsure(0)) return false;
    sBlurOk = true;
    return true;
}

/* The shared pieces of a border made from more than one rectangle, when first needed. */
static bool TopBlurFinalEnsure(void) {
    if (sBlurFinalReady) return true;
    if (!TopBlurStageInit(&sBlurFinal)) return false;
    for (int j = 0; j < BLUR_RECTS; ++j) {
        if (!C3D_TexInit(&sBlurWeight[j], 64, 32, GPU_RGBA8)) return false;
        C3D_TexSetFilter(&sBlurWeight[j], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sBlurWeight[j], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    }
    sBlurFinalReady = true;
    return true;
}

static void TopBlurShutdown(void) {
    TopBlurFree();
}

/* What the blurred border is made from this frame: the parts of the view the game's picture holds, as
 * rectangles of the screen (x, y, w x h) and as the texels of the frame they are (srcX, srcY). Every screen
 * whose picture leaves the 400x240 black around it gets them: a picture drawn smaller than the screen
 * (FRAME = BLUR in ORIGINAL; the 240x160 screens of WIDE + BLUR / WIDE + AI, which leave the scenes that
 * have a generated picture, and the portal's strips, to those) and, in WIDE, a room smaller than the
 * view, which the game centres with black on both sides (the room, and the neighbouring rooms drawn
 * beside it, each a rectangle of its own). */
typedef struct BlurRect {
    int x, y, w, h;
    int srcX, srcY;
} BlurRect;
typedef struct BlurJob {
    BlurRect rects[BLUR_RECTS];
    int count;
    bool over; /* drawn over the frame, where it is black, not under it: the room's own black is in the frame */
} BlurJob;

static bool TopBlurWanted(const TopView3DSPlan* plan, BlurJob* job) {
    const Port3DSFullViewPresentation* presentation = &plan->source;
    /* Only FRAME = BLUR (VIEW ORIGINAL) since 2026-10-20: WIDE + BLUR, and WIDE + AI's blur where it has no picture,
     * cost the Old 3DS too much while scrolling and were dropped. The rectangles of the view (job->over, more than
     * one: the room and its neighbours) served them and are kept for a cheaper way back. */
    (void)plan;
    (void)job;
    return false;
    job->count = 1;
    job->over = false;
    job->rects[0] = (BlurRect){ plan->drawX, plan->drawY, plan->drawWidth, plan->drawHeight, presentation->sourceX,
                                presentation->sourceY };
    return true;
}

static void TopBlurInvalidate(void) {
    sBlurValid = false;
    for (int j = 0; j < BLUR_RECTS; ++j) sBlurSets[j].valid = false;
}

/* One pass: the (u0, v0)-(u1, v1) part of `src` (texture coordinates, which may lie outside 0..1 for
 * the clamped wrap to carry the edge) into the whole of `dst`. */
static void TopBlurPass(C3D_Tex* src, float u0, float v0, float u1, float v1, const BlurStage* dst) {
    const Tex3DS_SubTexture sub = {
        .width = (u16)dst->width, .height = (u16)dst->height, .left = u0, .right = u1, .top = v0, .bottom = v1,
    };
    const C2D_Image image = { .tex = src, .subtex = &sub };
    const C2D_DrawParams params = {
        .pos = { 0.0f, 0.0f, (float)dst->width, (float)dst->height },
        .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
    };
    C3D_TexSetFilter(src, GPU_LINEAR, GPU_LINEAR);
    C2D_SceneBegin(dst->target);
    C2D_DrawImage(image, &params, NULL);
}

/* The weights of the border's three levels at the centre of each texel of their stage: how far a point of the
 * screen is from the picture's rectangle (the straight distance, so the zones have rounded corners) gives the
 * near level a weight falling smoothly from 1 at the picture to 0 at BLUR_NEAR_FALLOFF, the mid one likewise to
 * BLUR_MID_FALLOFF, and the weights are made to add up to 1 everywhere: near, mid among what the near one leaves,
 * the diluted picture the rest. */
static float TopBlurFalloff(float distance, float reach) {
    float t = 1.0f - distance / reach;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

static void TopBlurMasks(BlurSet* set, int x, int y, int w, int h) {
    for (int level = 0; level < 3; ++level) {
        const unsigned mw = set->stage[level + 1].width, mh = set->stage[level + 1].height;
        uint32_t texels[32 * 16];
        for (unsigned row = 0; row < mh; ++row) { /* memory row 0 is the bottom of the screen */
            for (unsigned col = 0; col < mw; ++col) {
                const float sx = ((float)col + 0.5f) * 400.0f / (float)mw;
                const float sy = (1.0f - ((float)row + 0.5f) / (float)mh) * 240.0f;
                const float dx = sx < (float)x ? (float)x - sx : sx > (float)(x + w) ? sx - (float)(x + w) : 0.0f;
                const float dy = sy < (float)y ? (float)y - sy : sy > (float)(y + h) ? sy - (float)(y + h) : 0.0f;
                const float distance = sqrtf(dx * dx + dy * dy);
                const float near = TopBlurFalloff(distance, (float)BLUR_NEAR_FALLOFF);
                const float mid = TopBlurFalloff(distance, (float)BLUR_MID_FALLOFF);
                const float weight = level == 0 ? near : level == 1 ? (1.0f - near) * mid : (1.0f - near) * (1.0f - mid);
                const unsigned v = (unsigned)(weight * 255.0f + 0.5f);
                /* 8x8 tiles, row-major; inside a tile the bits of x and y interleaved (x first). */
                const unsigned tile = (row / 8u) * (mw / 8u) + col / 8u;
                const unsigned cx = col & 7u, cy = row & 7u;
                const unsigned morton = (cx & 1u) | ((cy & 1u) << 1) | ((cx & 2u) << 1) | ((cy & 2u) << 2) |
                                        ((cx & 4u) << 2) | ((cy & 4u) << 3);
                texels[tile * 64u + morton] = v | (v << 8) | (v << 16) | (v << 24);
            }
        }
        C3D_TexLoadImage(&set->mask[level], texels, GPU_TEXFACE_2D, 0);
    }
}

/* The share of each rectangle in the final border (64x32 texels): at the centre of a texel the rectangles weigh
 * 1 / (1 + (distance / 10)^2), made to add up to 1, so the nearest one decides the colour and where two are
 * about as near they blend. */
static void TopBlurWeights(const BlurRect* rects, int count) {
    for (int k = 0; k < count; ++k) {
        uint32_t texels[64 * 32];
        for (unsigned row = 0; row < 32; ++row) {
            for (unsigned col = 0; col < 64; ++col) {
                const float sx = ((float)col + 0.5f) * 400.0f / 64.0f;
                const float sy = (1.0f - ((float)row + 0.5f) / 32.0f) * 240.0f;
                float sum = 0.0f, mine = 0.0f;
                for (int j = 0; j < count; ++j) {
                    const float dx = sx < (float)rects[j].x ? (float)rects[j].x - sx
                                     : sx > (float)(rects[j].x + rects[j].w) ? sx - (float)(rects[j].x + rects[j].w) : 0.0f;
                    const float dy = sy < (float)rects[j].y ? (float)rects[j].y - sy
                                     : sy > (float)(rects[j].y + rects[j].h) ? sy - (float)(rects[j].y + rects[j].h) : 0.0f;
                    const float d = sqrtf(dx * dx + dy * dy) / 10.0f;
                    const float weight = 1.0f / (1.0f + d * d);
                    sum += weight;
                    if (j == k) mine = weight;
                }
                const unsigned v = (unsigned)(mine / sum * 255.0f + 0.5f);
                const unsigned tile = (row / 8u) * 8u + col / 8u;
                const unsigned cx = col & 7u, cy = row & 7u;
                const unsigned morton = (cx & 1u) | ((cy & 1u) << 1) | ((cx & 2u) << 1) | ((cy & 2u) << 2) |
                                        ((cx & 4u) << 2) | ((cy & 4u) << 3);
                texels[tile * 64u + morton] = v | (v << 8) | (v << 16) | (v << 24);
            }
        }
        C3D_TexLoadImage(&sBlurWeight[k], texels, GPU_TEXFACE_2D, 0);
        memcpy(sBlurWeightsFor[k], &rects[k], sizeof(sBlurWeightsFor[k]));
    }
}

/* Shrinks the picture (srcX, srcY, srcW x srcH of `source`), which is drawn at (x, y, w x h) on the
 * screen, into a set's stages. */
static void TopBlurBuildSet(BlurSet* set, C3D_Tex* source, int srcX, int srcY, int srcW, int srcH, int x, int y, int w,
                            int h) {
    const int rect[4] = { x, y, w, h };
    const bool smooth = set->valid && memcmp(rect, set->rect, sizeof(rect)) == 0;
    ConfigureIdentityTextureEnv();
    ConfigureOpaqueOverwriteBlend();
    if (smooth) { /* the picture goes into its previous contents, a little of it each time */
        C3D_BlendingColor((u32)BLUR_SMOOTH_ALPHA << 24);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA,
                       GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA);
    }
    /* The picture to 64x64; half a texel in so the linear filter never reads outside it. */
    const float tw = (float)source->width, th = (float)source->height;
    /* Not the picture's outermost rows and columns: the border carries the edge outwards, and a few bright
     * pixels along the edge of an otherwise dark picture (the line where a room ends) would become streaks
     * across the whole border. */
    if (srcW > 8 * BLUR_EDGE_INSET && srcH > 8 * BLUR_EDGE_INSET) {
        srcX += BLUR_EDGE_INSET;
        srcY += BLUR_EDGE_INSET;
        srcW -= 2 * BLUR_EDGE_INSET;
        srcH -= 2 * BLUR_EDGE_INSET;
    }
    TopBlurPass(source, ((float)srcX + 0.5f) / tw, 1.0f - ((float)srcY + 0.5f) / th,
                ((float)(srcX + srcW) - 0.5f) / tw, 1.0f - ((float)(srcY + srcH) - 0.5f) / th, &set->stage[0]);
    C2D_Flush(); /* drawn under that blend, the rest under the overwrite one */
    ConfigureOpaqueOverwriteBlend();
    /* That over the picture's rectangle of the screen, its edge texels carried out to the screen's (texture
     * coordinates past 0..1), then shrunk again. */
    TopBlurPass(&set->stage[0].texture, -(float)x / (float)w, 1.0f + (float)y / (float)h, (float)(400 - x) / (float)w,
                1.0f - (float)(240 - y) / (float)h, &set->stage[1]);
    TopBlurPass(&set->stage[1].texture, 0.0f, 1.0f, 1.0f, 0.0f, &set->stage[2]);
    TopBlurPass(&set->stage[2].texture, 0.0f, 1.0f, 1.0f, 0.0f, &set->stage[3]);
    if (!smooth) TopBlurMasks(set, x, y, w, h);
    /* Each level scaled by its weight (the destination times the mask, colour and alpha)... */
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_SRC_COLOR, GPU_ZERO, GPU_SRC_ALPHA);
    for (int level = 1; level <= 3; ++level)
        TopBlurPass(&set->mask[level - 1], 0.0f, 1.0f, 1.0f, 0.0f, &set->stage[level]);
    C2D_Flush();
    /* ...and the border is their sum. */
    ConfigureOpaqueOverwriteBlend();
    TopBlurPass(&set->stage[3].texture, 0.0f, 1.0f, 1.0f, 0.0f, &set->stage[BLUR_BORDER]);
    C2D_Flush();
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    TopBlurPass(&set->stage[2].texture, 0.0f, 1.0f, 1.0f, 0.0f, &set->stage[BLUR_BORDER]);
    TopBlurPass(&set->stage[1].texture, 0.0f, 1.0f, 1.0f, 0.0f, &set->stage[BLUR_BORDER]);
    C2D_Flush();
    ConfigureOpaqueOverwriteBlend();
    memcpy(set->rect, rect, sizeof(rect));
    set->valid = true;
}

/* Builds the border from the job's rectangles of `source`, unless this frame is skipped or belongs to the
 * right eye. Call between the eye's clear and its first draw; the caller goes back to the eye's scene. */
static void TopBlurBuild(C3D_Tex* source, const BlurJob* job) {
    if (sRightEye || !TopBlurInit() || job->count <= 0) return;
    if (sBlurValid && (++sBlurTick % BLUR_REFRESH) != 0) return;
    int count = 0;
    for (int j = 0; j < job->count && j < BLUR_RECTS; ++j) {
        const BlurRect* r = &job->rects[j];
        if (r->w <= 0 || r->h <= 0 || !TopBlurSetEnsure(j)) continue;
        TopBlurBuildSet(&sBlurSets[j], source, r->srcX, r->srcY, r->w, r->h, r->x, r->y, r->w, r->h);
        ++count;
    }
    if (count > 1 && !TopBlurFinalEnsure()) count = 1;
    if (count > 1) {
        bool changed = count != sBlurCount;
        for (int k = 0; k < count && !changed; ++k) changed = memcmp(sBlurWeightsFor[k], &job->rects[k], sizeof(sBlurWeightsFor[k])) != 0;
        if (changed) TopBlurWeights(job->rects, count);
        /* Each border scaled by its share, then added into the final one. */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_SRC_COLOR, GPU_ZERO, GPU_SRC_ALPHA);
        for (int k = 0; k < count; ++k)
            TopBlurPass(&sBlurWeight[k], 0.0f, 1.0f, 1.0f, 0.0f, &sBlurSets[k].stage[BLUR_BORDER]);
        C2D_Flush();
        ConfigureOpaqueOverwriteBlend();
        TopBlurPass(&sBlurSets[0].stage[BLUR_BORDER].texture, 0.0f, 1.0f, 1.0f, 0.0f, &sBlurFinal);
        C2D_Flush();
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        for (int k = 1; k < count; ++k)
            TopBlurPass(&sBlurSets[k].stage[BLUR_BORDER].texture, 0.0f, 1.0f, 1.0f, 0.0f, &sBlurFinal);
        C2D_Flush();
        ConfigureOpaqueOverwriteBlend();
    }
    sBlurCount = count;
    sBlurValid = count > 0;
}

/* The border drawn where the job's rectangles are not: the screen cut into bands at their top and bottom
 * edges, and in each band what the rectangles crossing it leave of the width (each part of the last stage
 * that lies under it when it is stretched over the whole screen). Needs the eye's scene and the standard blend;
 * leaves the env it set (`carrier`: the source was the ABGR carrier). */
static void TopBlurDraw(const BlurJob* job, bool carrier) {
    if (!sBlurValid || !sBlurOk || sBlurCount <= 0) return;
    C3D_Tex* texture = sBlurCount > 1 ? &sBlurFinal.texture : &sBlurSets[0].stage[BLUR_BORDER].texture;
    if (carrier) ConfigureAbgrTextureEnv();
    else ConfigureIdentityTextureEnv();
    /* A layer in front moves in opposite ways in the two eyes and can uncover a few columns of the frame's
     * black just inside the room's edge: over the frame the glow goes that far in too. */
    const int inset = job->over && Stereo3DS_HasDepth() ? BLUR_STEREO_INSET : 0;
    /* Over the frame: added where it is black (the light is multiplied by what the frame lacks of white),
     * so what the game drew in the border (the HUD) stays as it is. */
    if (job->over)
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE_MINUS_DST_COLOR, GPU_ONE, GPU_ZERO, GPU_ONE);
    int breaks[2 * BLUR_RECTS + 2], breakCount = 0;
    breaks[breakCount++] = 0;
    breaks[breakCount++] = 240;
    for (int j = 0; j < job->count; ++j) {
        const int top = job->rects[j].y, bottom = job->rects[j].y + job->rects[j].h;
        if (top > 0 && top < 240) breaks[breakCount++] = top;
        if (bottom > 0 && bottom < 240) breaks[breakCount++] = bottom;
    }
    for (int i = 1; i < breakCount; ++i) { /* sorted */
        const int v = breaks[i];
        int k = i - 1;
        while (k >= 0 && breaks[k] > v) { breaks[k + 1] = breaks[k]; --k; }
        breaks[k + 1] = v;
    }
    for (int band = 0; band + 1 < breakCount; ++band) {
        const int ya = breaks[band], yb = breaks[band + 1];
        if (yb <= ya) continue;
        /* What the rectangles crossing the band cover of the width, merged and sorted. */
        int from[BLUR_RECTS], to[BLUR_RECTS], n = 0;
        for (int j = 0; j < job->count; ++j) {
            const BlurRect* r = &job->rects[j];
            if (r->y > ya || r->y + r->h < yb) continue;
            int lo = r->x, hi = r->x + r->w, k = n++;
            while (k > 0 && from[k - 1] > lo) { from[k] = from[k - 1]; to[k] = to[k - 1]; --k; }
            from[k] = lo;
            to[k] = hi;
        }
        int merged = 0;
        for (int k = 0; k < n; ++k) {
            if (merged > 0 && from[k] <= to[merged - 1]) {
                if (to[k] > to[merged - 1]) to[merged - 1] = to[k];
            } else {
                from[merged] = from[k];
                to[merged++] = to[k];
            }
        }
        int cursor = 0;
        for (int k = 0; k <= merged; ++k) {
            const int gapEnd = k < merged ? from[k] + (from[k] > 0 ? inset : 0) : 400;
            if (gapEnd > cursor) {
                const int rx = cursor, rw = gapEnd - cursor;
                const Tex3DS_SubTexture sub = {
                    .width = (u16)rw, .height = (u16)(yb - ya),
                    .left = (float)rx / 400.0f,
                    .right = (float)(rx + rw) / 400.0f,
                    .top = 1.0f - (float)ya / 240.0f,
                    .bottom = 1.0f - (float)yb / 240.0f,
                };
                const C2D_Image image = { .tex = texture, .subtex = &sub };
                C2D_DrawImageAt(image, (float)rx, (float)ya, 0.0f, NULL, 1.0f, 1.0f);
            }
            if (k < merged) cursor = to[k] - (to[k] < 400 ? inset : 0);
        }
    }
    C2D_Flush();
    if (job->over) ConfigureStandardAlphaBlend();
}

static void DrawTopImage(const uint32_t* pixels, unsigned width, unsigned height,
                         unsigned validSourceWidth, unsigned validSourceHeight,
                         Port3DSFullViewMode requestedMode, int cropX, int cropY) {
    const int style = Port_Config_Get3DSDisplayStyle();
    TopView3DSPlan plan;
    /* `requestedMode` is latched with the IO/OAM generation. A settings
     * change can occur after that generation was produced; do not reinterpret
     * its final experimental frame through the new live combo state. */
    TopView3DS_BuildPlan(sOld3DSProfile,
                         requestedMode != PORT_3DS_FULL_VIEW_FALLBACK,
                         Port_Config_Get3DSAspectRatio(), style, requestedMode,
                         (int)width, (int)height, (int)validSourceWidth,
                         (int)validSourceHeight, cropX, cropY, &plan);
    const Port3DSFullViewPresentation* presentation = &plan.source;
    sTopPresentWidth = (unsigned)presentation->renderWidth;
    sTopPresentHeight = (unsigned)presentation->renderHeight;
    sTopValidSourceWidth = validSourceWidth;
    sTopValidSourceHeight = validSourceHeight;
    sTopPresentMode = plan.mode;
    sTopCropX = presentation->sourceX;
    sTopCropY = presentation->sourceY;

    if (!sRightEye) { /* the right eye is drawn from the same upload */
        const size_t topFlushBytes =
            (size_t)sUploadLayout.topPitch * sTopPresentHeight * sizeof(uint32_t);
        Platform3DS_CleanDataCache(pixels, topFlushBytes);
        /* Old 3DS only: the CPU renderer publishes 160 rows. Describe that exact
         * source rectangle so the display engine does not read another 96 unused
         * RGBA rows. New 3DS retains the established transfer dimensions. */
        const unsigned sourceHeight = sOld3DSProfile ? sUploadLayout.topRows : TOP_TEXTURE_HEIGHT;
        SplitFlushed();
        C3D_SyncDisplayTransfer((u32*)pixels, GX_BUFFER_DIM(sUploadLayout.topPitch, sourceHeight),
                                (u32*)sTopTexture.data, GX_BUFFER_DIM(TOP_TEXTURE_WIDTH, TOP_TEXTURE_HEIGHT),
                                TextureTransfer());
    }
    sTopSubtexture = (Tex3DS_SubTexture){
        .width = (u16)presentation->sourceWidth,
        .height = (u16)presentation->sourceHeight,
        .left = (float)presentation->sourceX / TOP_TEXTURE_WIDTH,
        .top = 1.0f - (float)presentation->sourceY / TOP_TEXTURE_HEIGHT,
        .right = (float)(presentation->sourceX + presentation->sourceWidth) / TOP_TEXTURE_WIDTH,
        .bottom = 1.0f - (float)(presentation->sourceY + presentation->sourceHeight) / TOP_TEXTURE_HEIGHT,
    };
    const C2D_Image image = { .tex = &sTopTexture, .subtex = &sTopSubtexture };
    const C2D_DrawParams params = {
        .pos = { .x = (float)plan.drawX, .y = (float)plan.drawY,
                 .w = (float)plan.drawWidth, .h = (float)plan.drawHeight },
        .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
    };
    C2D_TargetClear(sEyeTarget, TopClearColor());
    BlurJob blur;
    const bool blurBorder = TopBlurWanted(&plan, &blur);
    if (blurBorder)
        TopBlurBuild(&sTopTexture, &blur);
    else
        TopBlurInvalidate();
    const unsigned intermediateScale = (unsigned)plan.sharpBilinearScale;
    const bool validSharpBilinearScale = intermediateScale == 2u;
    const unsigned intermediateWidth = validSharpBilinearScale
                                           ? (unsigned)presentation->sourceWidth * intermediateScale
                                           : 0u;
    const unsigned intermediateHeight = validSharpBilinearScale
                                            ? (unsigned)presentation->sourceHeight * intermediateScale
                                            : 0u;
    const bool useSharpBilinear = plan.useSharpBilinear && validSharpBilinearScale &&
                                  sSharpBilinearTarget &&
                                  intermediateWidth < SHARP_BILINEAR_TEXTURE_WIDTH &&
                                  intermediateHeight < SHARP_BILINEAR_TEXTURE_HEIGHT;
    if (useSharpBilinear) {
        const C2D_DrawParams integerParams = {
            .pos = { .x = 0.0f, .y = 0.0f,
                     .w = (float)intermediateWidth, .h = (float)intermediateHeight },
            .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
        };
        C3D_TexSetFilter(&sTopTexture, GPU_NEAREST, GPU_NEAREST);
        C2D_SceneBegin(sSharpBilinearTarget);
        ConfigureIdentityTextureEnv();
        ConfigureOpaqueOverwriteBlend();
        C2D_DrawImage(image, &integerParams, NULL);

        /* Linear filtering can sample one texel beyond a subtexture edge.
         * The valid image starts on the texture's clamped top/left edges;
         * duplicate its final source column, row and corner into a one-texel
         * right/bottom guard instead of allowing stale atlas data to bleed. */
        const Tex3DS_SubTexture rightEdgeSubtexture = {
            .width = 1,
            .height = (u16)presentation->sourceHeight,
            .left = (float)(presentation->sourceX + presentation->sourceWidth - 1) /
                    TOP_TEXTURE_WIDTH,
            .top = 1.0f - (float)presentation->sourceY / TOP_TEXTURE_HEIGHT,
            .right = (float)(presentation->sourceX + presentation->sourceWidth) /
                     TOP_TEXTURE_WIDTH,
            .bottom = 1.0f -
                      (float)(presentation->sourceY + presentation->sourceHeight) /
                          TOP_TEXTURE_HEIGHT,
        };
        const Tex3DS_SubTexture bottomEdgeSubtexture = {
            .width = (u16)presentation->sourceWidth,
            .height = 1,
            .left = (float)presentation->sourceX / TOP_TEXTURE_WIDTH,
            .top = 1.0f -
                   (float)(presentation->sourceY + presentation->sourceHeight - 1) /
                       TOP_TEXTURE_HEIGHT,
            .right = (float)(presentation->sourceX + presentation->sourceWidth) /
                     TOP_TEXTURE_WIDTH,
            .bottom = 1.0f -
                      (float)(presentation->sourceY + presentation->sourceHeight) /
                          TOP_TEXTURE_HEIGHT,
        };
        const Tex3DS_SubTexture cornerSubtexture = {
            .width = 1,
            .height = 1,
            .left = rightEdgeSubtexture.left,
            .top = bottomEdgeSubtexture.top,
            .right = rightEdgeSubtexture.right,
            .bottom = bottomEdgeSubtexture.bottom,
        };
        const C2D_Image rightEdgeImage = { .tex = &sTopTexture, .subtex = &rightEdgeSubtexture };
        const C2D_Image bottomEdgeImage = { .tex = &sTopTexture, .subtex = &bottomEdgeSubtexture };
        const C2D_Image cornerImage = { .tex = &sTopTexture, .subtex = &cornerSubtexture };
        const C2D_DrawParams rightEdgeParams = {
            .pos = { .x = (float)intermediateWidth, .y = 0.0f,
                     .w = 1.0f, .h = (float)intermediateHeight },
            .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
        };
        const C2D_DrawParams bottomEdgeParams = {
            .pos = { .x = 0.0f, .y = (float)intermediateHeight,
                     .w = (float)intermediateWidth, .h = 1.0f },
            .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
        };
        const C2D_DrawParams cornerParams = {
            .pos = { .x = (float)intermediateWidth, .y = (float)intermediateHeight,
                     .w = 1.0f, .h = 1.0f },
            .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
        };
        C2D_DrawImage(rightEdgeImage, &rightEdgeParams, NULL);
        C2D_DrawImage(bottomEdgeImage, &bottomEdgeParams, NULL);
        C2D_DrawImage(cornerImage, &cornerParams, NULL);

        /* Beginning the physical scene flushes the complete nearest pass
         * before this texture is sampled. UVs cover only the valid 2x image,
         * never the unused power-of-two container. */
        sSharpBilinearSubtexture = (Tex3DS_SubTexture){
            .width = (u16)intermediateWidth,
            .height = (u16)intermediateHeight,
            .left = 0.0f,
            .top = 1.0f,
            .right = (float)intermediateWidth / SHARP_BILINEAR_TEXTURE_WIDTH,
            .bottom = 1.0f - (float)intermediateHeight / SHARP_BILINEAR_TEXTURE_HEIGHT,
        };
        const C2D_Image intermediateImage = {
            .tex = &sSharpBilinearTexture, .subtex = &sSharpBilinearSubtexture
        };
        /* SceneBegin flushes the complete overwrite batch before changing
         * blend state for the physical-target batch. */
        C2D_SceneBegin(sEyeTarget);
        ConfigureStandardAlphaBlend();
        if (blurBorder && !blur.over) TopBlurDraw(&blur, true);
        C3D_TexSetFilter(&sSharpBilinearTexture, GPU_LINEAR, GPU_LINEAR);
        C2D_DrawImage(intermediateImage, &params, NULL);
        DrawFrameEdges(&intermediateImage, &params);
        ConfigureAbgrTextureEnv();
        if (!sRightEye) ++sStats.sharpBilinearFrames;
    } else {
        const GPU_TEXTURE_FILTER_PARAM filter =
            (plan.linearFilter || plan.useSharpBilinear) ? GPU_LINEAR : GPU_NEAREST;
        C3D_TexSetFilter(&sTopTexture, filter, filter);
        C2D_SceneBegin(sEyeTarget);
        ConfigureStandardAlphaBlend();
        if (blurBorder && !blur.over) TopBlurDraw(&blur, true);
        C2D_DrawImage(image, &params, NULL);
        DrawFrameEdges(&image, &params);
        ConfigureAbgrTextureEnv();
        if (plan.useSharpBilinear) if (!sRightEye) ++sStats.sharpBilinearFallbacks;
    }
    if (blurBorder && blur.over) {
        C2D_Flush();
        TopBlurDraw(&blur, true);
    }
    DrawScenePictures(plan.drawX, plan.drawY, plan.drawWidth, plan.drawHeight, true);
    if (Port_Config_GetShowFps()) DrawFpsOverlay();
}

/* Pictures that extend a scene the GBA drew at 240x160 (docs/wide-scene-extension.md), drawn only
 * outside the game's own picture: generated extensions shipped in the romfs
 * (platform/3ds/assets/wide-scenes/), or the player's own in the data folder, which wins. RGB565
 * as tools/scene-extend/to_console.py writes them, put through the game's own colour conversion
 * (colour correction included) and drawn with the frame's ABGR env. A texture lives only while its
 * scene is on screen, and is made again when colour correction changes. */
typedef struct ScenePicture {
    const char* path;      /* the player's own, in the data folder: it wins */
    const char* romfsPath; /* the one the game ships */
    unsigned width, height, texWidth, texHeight;
    C3D_Tex texture;
    int state;      /* 0 not tried, 1 loaded, -1 no file */
    bool corrected; /* the colour correction the texture was made with */
    bool procedural; /* made by code from the game's own art, no generative AI: WIDE is enough */
    const uint16_t* (*make)(void); /* made by the game instead of read from a file: GBA colours, 0 where nothing */
} ScenePicture;

static void LoadScenePicture(ScenePicture* picture) {
    picture->state = -1;
    if (picture->path == NULL && picture->make == NULL) return;
    const size_t count = (size_t)picture->width * picture->height;
    const uint16_t* made = picture->make ? picture->make() : NULL;
    uint16_t* pixels = malloc(count * sizeof(uint16_t));
    size_t got = 0;
    if (pixels != NULL && made != NULL) {
        /* As a file has it (RGB565), for the loop below. */
        for (size_t i = 0; i < count; ++i) {
            const uint16_t c = made[i];
            pixels[i] = (uint16_t)(((c & 31u) << 11) | (((c >> 5) & 31u) << 6) | ((c >> 10) & 31u));
        }
        got = count;
    } else if (pixels != NULL && picture->path != NULL &&
               ScenePictures_ReadFile(picture->path, picture->romfsPath, pixels, count * sizeof(uint16_t))) {
        got = count;
    }
    if (got != count || !C3D_TexInit(&picture->texture, picture->texWidth, picture->texHeight, GPU_RGBA8)) {
        free(pixels);
        return;
    }
    /* Write in Morton order so there is no DMA source to keep alive after this frame. */
    uint32_t* texels = picture->texture.data;
    memset(texels, 0, picture->texture.size);
    for (unsigned r = 0; r < picture->height; ++r) {
        for (unsigned x = 0; x < picture->width; ++x) {
            /* Back to the GBA's 15-bit colour (the picture keeps the game's palette). */
            const uint16_t c = pixels[r * picture->width + x];
            const uint16_t gba = (uint16_t)(((c >> 11) & 31u) | ((((c >> 5) & 63u) >> 1) << 5) | ((c & 31u) << 10));
            const size_t tile = (size_t)(r >> 3) * (picture->texWidth >> 3) + (x >> 3);
            const size_t offset = tile * 64u + PpuGpu3DS_MortonIndex(x & 7u, r & 7u);
            texels[offset] = virtuappu_mode1_rgb555_to_abgr8888(gba);
        }
    }
    free(pixels);
    C3D_TexFlush(&picture->texture);
    C3D_TexSetFilter(&picture->texture, GPU_NEAREST, GPU_NEAREST);
    picture->corrected = Port_Config_GetColorCorrection();
    picture->state = 1;
}

/* Whether the scene's picture can be drawn this frame; frees it when the scene is not shown. */
static bool ScenePictureReady(ScenePicture* picture, bool shown) {
    if (!picture->procedural && !Port_Config_GetWideAi()) shown = false;
    if (picture->state == 1 && (!shown || picture->corrected != Port_Config_GetColorCorrection())) {
        C3D_TexDelete(&picture->texture);
        picture->state = 0;
    }
    if (!shown) {
        picture->state = 0;
        return false;
    }
    if (picture->state == 0) LoadScenePicture(picture);
    return picture->state == 1;
}

/* A part of the picture (srcX, srcY, w x h) at a screen position. */
static void DrawScenePicturePart(ScenePicture* picture, int srcX, int srcY, int w, int h, int x, int y) {
    if (w <= 0 || h <= 0) return;
    const Tex3DS_SubTexture sub = {
        .width = (u16)w,
        .height = (u16)h,
        .left = (float)srcX / picture->texWidth,
        .top = 1.0f - (float)srcY / picture->texHeight,
        .right = (float)(srcX + w) / picture->texWidth,
        .bottom = 1.0f - (float)(srcY + h) / picture->texHeight,
    };
    const C2D_Image image = { .tex = &picture->texture, .subtex = &sub };
    C2D_DrawImageAt(image, (float)x, (float)y, 0.5f, NULL, 1.0f, 1.0f);
}

/* The game's fades (in from black, out to white, flashes) are in its frame, under the pictures. */
static void VeilScenePicture(int x, int y, int w, int h) {
    uint8_t weight, level;
    if (w > 0 && h > 0 && FadeGpu3DS_Shown(&weight, &level))
        C2D_DrawRectSolid((float)x, (float)y, 0.5f, (float)w, (float)h, C2D_Color32(level, level, level, weight));
}

/* The Minish portal cutscene in Full View (Port_Widescreen_CutsceneStrips): its picture (BG1, 320
 * lines) starts 80 columns into the 400x240 view and BG1 repeats past its edges, so the presenter
 * covers the columns left and right of it, black or from the portal's strips file (its 80 columns
 * left of the original and those right of it to the view's edge, 320 lines), scrolling with the
 * picture. Each portal has its own picture: the tree stump's is 256 wide, the rock's 240. The
 * other two have no strips yet and get black, at the stump's width. */
enum { STRIPS_LEFT_W = 80, PORTAL_PICTURES = 4 };
typedef struct {
    int pictureW; /* the game's picture */
    ScenePicture strips; /* STRIPS_LEFT_W + (400 - STRIPS_LEFT_W - pictureW) columns */
} PortalStrips;
#define PORTAL_STRIPS(w, name)                                                                        \
    { .pictureW = (w),                                                                               \
      .strips = { .path = "" name, .romfsPath = "romfs:/wide-scenes/" name,         \
                  .width = 400 - (w), .height = 320, .texWidth = 256, .texHeight = 512 } }
static PortalStrips sPortalStrips[PORTAL_PICTURES] = {
    PORTAL_STRIPS(256, "portal-cutscene-strips.bin"), /* tree stump */
    PORTAL_STRIPS(240, "portal-rock-strips.bin"),
    { .pictureW = 256 }, /* no file: black */
    { .pictureW = 256 }, /* dungeon */
};

static void DrawCutsceneStrip(ScenePicture* strips, int screenX, int width, int srcX, int pictureY) {
    if (strips->state != 1)
        C2D_DrawRectSolid((float)screenX, 0.0f, 0.5f, (float)width, 240.0f, C2D_Color32(0, 0, 0, 255));
    else
        DrawScenePicturePart(strips, srcX, 0, width, 320, screenX, pictureY);
}

/* The title screen at 1x in Full View (Port_Widescreen_TitleForestShown): the game draws its 240x160
 * screen centred, and the forest goes on around it from title-forest.bin (the whole 400x240 picture,
 * the original at 80,40 of it; tools/scene-extend/to_console.py title). The light rays, the sword
 * and the logo stay inside the GBA's screen, as on the GBA. */
static ScenePicture sTitleForest = { .path = "title-forest.bin",
                                     .romfsPath = "romfs:/wide-scenes/title-forest.bin", .width = 400, .height = 240,
                                     .texWidth = 512, .texHeight = 256 };

/* The pause menu at 1x in Full View with the software renderer (Port_Widescreen_PauseBackgroundShown; the GPU one
 * draws it at 400x240 itself): its background of Ezlo caps (BG3) goes on around the 240x160 screen, the same 400x240
 * layout as the title's, made by the game from its own (ScenePictures_Pause555), so drawn in plain WIDE too. */
static ScenePicture sPauseBackground = { .width = 400, .height = 240, .texWidth = 512, .texHeight = 256,
                                         .procedural = true, .make = ScenePictures_Pause555 };

/* The file select screen at 1x in Full View (Port_Widescreen_FileSelectCaveShown): its cave goes on
 * around the 240x160 screen from fileselect-cave.bin (tools/scene-extend/to_console.py fileselect):
 * palette indices of BG palettes 11-14, which the game cycles (src/fileselect.c, sub_0805066C), so
 * the texture is rebuilt from the palettes the shown frame used whenever they change and the
 * extension shimmers with the original. */
enum { CAVE_W = SCENE_CAVE_W, CAVE_H = SCENE_CAVE_H, CAVE_TEX_W = 512, CAVE_TEX_H = 256,
       CAVE_FIRST_PALETTE = SCENE_CAVE_FIRST_PALETTE };
static struct {
    const uint8_t* indices; /* ScenePictures_CaveIndices */
    uint32_t* linear; /* the texture's picture before the display transfer tiles it */
    ScenePicture picture;
    uint16_t palettes[64]; /* BG palettes 11-14 the texture was made with */
} sCave = { .picture = { .width = CAVE_W, .height = CAVE_H, .texWidth = CAVE_TEX_W, .texHeight = CAVE_TEX_H } };

static void ReleaseCave(void) {
    if (sCave.picture.state == 1) C3D_TexDelete(&sCave.picture.texture);
    if (sCave.linear) linearFree(sCave.linear);
    sCave.indices = NULL;
    sCave.linear = NULL;
}

static bool CaveReady(bool shown) {
    ScenePicture* picture = &sCave.picture;
    if (!shown) {
        ReleaseCave();
        picture->state = 0;
        return false;
    }
    if (picture->state == 0) {
        picture->state = -1;
        sCave.indices = ScenePictures_CaveIndices();
        sCave.linear = sCave.indices ? linearAlloc(CAVE_TEX_W * CAVE_TEX_H * sizeof(uint32_t)) : NULL;
        if (!sCave.linear || !C3D_TexInit(&picture->texture, CAVE_TEX_W, CAVE_TEX_H, GPU_RGBA8)) {
            ReleaseCave();
            return false;
        }
        memset(sCave.linear, 0, CAVE_TEX_W * CAVE_TEX_H * sizeof(uint32_t));
        C3D_TexSetFilter(&picture->texture, GPU_NEAREST, GPU_NEAREST);
        picture->state = 1;
        picture->corrected = !Port_Config_GetColorCorrection(); /* made below */
    }
    if (picture->state != 1) return false;
    const uint16_t* palettes = FadeGpu3DS_ShownPalettes() + CAVE_FIRST_PALETTE * 16;
    if (picture->corrected == Port_Config_GetColorCorrection() &&
        memcmp(palettes, sCave.palettes, sizeof(sCave.palettes)) == 0)
        return true;
    memcpy(sCave.palettes, palettes, sizeof(sCave.palettes));
    picture->corrected = Port_Config_GetColorCorrection();
    uint32_t colours[64];
    for (unsigned i = 0; i < 64; ++i) colours[i] = virtuappu_mode1_rgb555_to_abgr8888(sCave.palettes[i]);
    for (unsigned y = 0; y < CAVE_H; ++y) {
        const uint8_t* row = sCave.indices + y * CAVE_W;
        uint32_t* out = sCave.linear + y * CAVE_TEX_W;
        for (unsigned x = 0; x < CAVE_W; ++x)
            if (row[x]) out[x] = colours[row[x] & 63u];
    }
    const size_t bytes = CAVE_TEX_W * CAVE_TEX_H * sizeof(uint32_t);
    Platform3DS_CleanDataCache(sCave.linear, bytes);
    SplitFlushed();
    C3D_SyncDisplayTransfer((u32*)sCave.linear, GX_BUFFER_DIM(CAVE_TEX_W, CAVE_TEX_H), (u32*)picture->texture.data,
                            GX_BUFFER_DIM(CAVE_TEX_W, CAVE_TEX_H), TextureTransfer());
    return true;
}

/* After the game's frame. frameX, frameY, frameW, frameH: where it was drawn on the top screen.
 * abgrAfter: the texture env the caller leaves set (ABGR or identity) is put back. */
static void DrawScenePictures(int frameX, int frameY, int frameW, int frameH, bool abgrAfter) {
    int pictureX, pictureY, portalIndex = 0;
    const bool portal = Port_Widescreen_CutsceneStrips(&pictureX, &pictureY, &portalIndex);
    const bool aiNative = Port_Config_GetWideAi() && frameW == 240 && frameH == 160;
    const bool title = aiNative && Port_Widescreen_TitleForestShown();
    const bool cave = aiNative && Port_Widescreen_FileSelectCaveShown();
    const bool pause = frameW == 240 && frameH == 160 && Port_Widescreen_PauseBackgroundShown();
    bool portalReady = false;
    for (int i = 0; i < PORTAL_PICTURES; ++i) {
        const bool ready = ScenePictureReady(&sPortalStrips[i].strips, portal && i == portalIndex);
        if (portal && i == portalIndex) portalReady = ready;
    }
    PortalStrips* const strips = &sPortalStrips[portal ? portalIndex : 0];
    const int pictureW = strips->pictureW, rightW = 400 - STRIPS_LEFT_W - pictureW;
    const bool titleReady = ScenePictureReady(&sTitleForest, title);
    const bool caveReady = CaveReady(cave);
    const bool pauseReady = ScenePictureReady(&sPauseBackground, pause);
    /* The pictures drawn around the 240x160 screen from a whole 400x240 picture. */
    ScenePicture* const around = titleReady ? &sTitleForest : pauseReady ? &sPauseBackground : NULL;
    if (!portal && !title && !caveReady && !around) return;
    /* Citro2D sends a batch when the texture changes, with the env set then: send the frame's
     * quad under its own env before the pictures change it. */
    C2D_Flush();
    if (portalReady || around || caveReady) ConfigureAbgrTextureEnv(); /* the pictures are in the frame's pixel order */
    if (portal) {
        /* Left of the picture, then right of it; each strip is drawn from its own columns. */
        DrawCutsceneStrip(&strips->strips, pictureX - STRIPS_LEFT_W, STRIPS_LEFT_W, 0, pictureY);
        DrawCutsceneStrip(&strips->strips, pictureX + pictureW, rightW, STRIPS_LEFT_W, pictureY);
    }
    const int left = frameX - 80, top = frameY - 40; /* the picture's top-left on screen */
    if (around) {
        DrawScenePicturePart(around, 0, 0, 400, 40, left, top);
        DrawScenePicturePart(around, 0, 200, 400, 40, left, frameY + 160);
        DrawScenePicturePart(around, 0, 40, 80, 160, left, frameY);
        DrawScenePicturePart(around, 320, 40, 80, 160, frameX + 240, frameY);
    }
    if (caveReady) {
        DrawScenePicturePart(&sCave.picture, 0, 0, 400, 40, left, top);
        DrawScenePicturePart(&sCave.picture, 0, 200, 400, 40, left, frameY + 160);
        DrawScenePicturePart(&sCave.picture, 0, 40, 80, 160, left, frameY);
        DrawScenePicturePart(&sCave.picture, 320, 40, 80, 160, frameX + 240, frameY);
    }
    C2D_Flush();
    if ((portalReady || around || caveReady) && !abgrAfter) ConfigureIdentityTextureEnv();
    if (portal) {
        VeilScenePicture(0, 0, pictureX, 240);
        VeilScenePicture(pictureX + pictureW, 0, 400 - pictureX - pictureW, 240);
    }
    if (around || caveReady) {
        VeilScenePicture(left, top, 400, 40);
        VeilScenePicture(left, frameY + 160, 400, 40);
        VeilScenePicture(left, frameY, 80, 160);
        VeilScenePicture(frameX + 240, frameY, 80, 160);
    }
}

/* The changelog always occupies the physical top screen at 400x240,
 * independent of the gameplay aspect/filter settings. */
static void RestorePauseTop(void) {
    if (!sPauseUpdateLinked) return;
    C3D_RenderTargetSetOutput(sTopTarget, GFX_TOP, GFX_LEFT, sTopOutputFlags);
    if (sStereoLinked)
        C3D_RenderTargetSetOutput(sTopRightTarget, GFX_TOP, GFX_RIGHT, sTopOutputFlags);
    sPauseUpdateLinked = false;
    if (Port_Pause3DS_Active()) {
        C3D_FrameDrawOn(sTopTarget);
        if (sStereoLinked) C3D_FrameDrawOn(sTopRightTarget);
    }
}

static bool PreparePauseUpdateTargets(void) {
    const unsigned eyes = sStereoLinked ? 2u : 1u;
    for (unsigned eye = 0; eye < eyes; ++eye) {
        if (!sPauseUpdateTargets[eye])
            sPauseUpdateTargets[eye] = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
        if (!sPauseUpdateTargets[eye]) return false;
    }
    if (!sPauseUpdateLinked) {
        C3D_RenderTargetSetOutput(sPauseUpdateTargets[0], GFX_TOP, GFX_LEFT, sTopOutputFlags);
        if (sStereoLinked)
            C3D_RenderTargetSetOutput(sPauseUpdateTargets[1], GFX_TOP, GFX_RIGHT, sTopOutputFlags);
        sPauseUpdateLinked = true;
    }
    return true;
}

static void DrawUpdateTop(void) {
    if (!Port_SecondScreen_3DS_UpdateOpen()) {
        RestorePauseTop();
        return;
    }
    const bool paused = Port_Pause3DS_Active();
    /* GX display transfer is not a padded row copy: its output dimensions
     * describe the transfer extent. Match the 512x256 texture on both sides;
     * a 400x240 input produces corrupt tiled rows on hardware. Only the
     * 400x240 viewport is painted and sampled. */
    const size_t uploadBytes = TOP_TEXTURE_WIDTH * TOP_TEXTURE_HEIGHT * sizeof(uint32_t);
    if (!sUpdateReady) {
        sUpdatePixels = linearAlloc(uploadBytes);
        if (!sUpdatePixels)
            return;
        if (!C3D_TexInit(&sUpdateTexture, TOP_TEXTURE_WIDTH, TOP_TEXTURE_HEIGHT, GPU_RGBA8)) {
            linearFree(sUpdatePixels);
            sUpdatePixels = NULL;
            return;
        }
        C3D_TexSetFilter(&sUpdateTexture, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sUpdateTexture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        memset(sUpdatePixels, 0, uploadBytes);
        sUpdateReady = true;
    }
    if (Port_SecondScreen_3DS_PaintUpdateTop(sUpdatePixels, TOP_TEXTURE_WIDTH)) {
        Platform3DS_CleanDataCache(sUpdatePixels, uploadBytes);
        SplitFlushed();
        C3D_SyncDisplayTransfer(sUpdatePixels, GX_BUFFER_DIM(TOP_TEXTURE_WIDTH, TOP_TEXTURE_HEIGHT),
                                sUpdateTexture.data, GX_BUFFER_DIM(TOP_TEXTURE_WIDTH, TOP_TEXTURE_HEIGHT),
                                TextureTransfer());
    }
    C2D_Prepare();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    Tex3DS_SubTexture sub = { .width = 400,
                              .height = 240,
                              .left = 0,
                              .top = 1,
                              .right = 400.f / TOP_TEXTURE_WIDTH,
                              .bottom = 1 - 240.f / TOP_TEXTURE_HEIGHT };
    C2D_Image image = { .tex = &sUpdateTexture, .subtex = &sub };
    if (paused && !PreparePauseUpdateTargets()) return;
    C3D_RenderTarget* targets[] = {
        paused ? sPauseUpdateTargets[0] : sTopTarget,
        sStereoLinked ? (paused ? sPauseUpdateTargets[1] : sTopRightTarget) : NULL,
    };
    ConfigureAbgrTextureEnv();
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (!targets[eye]) continue;
        C2D_TargetClear(targets[eye], C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(targets[eye]);
        C2D_DrawImageAt(image, 0, 0, 0, NULL, 1, 1);
        C2D_Flush();
    }
    ConfigureAbgrTextureEnv();
    PlatformGpu3DS_InvalidateTopBorder();
}



/* The numbers box (SHOW FPS), after ../mzm and ../sm-3ds: four small lines in a dim box at the
 * bottom-left corner, averaged over the last second. The first is the game's own rate (green from
 * 59, yellow from 50, red below); L is the logic's ms per tick, G the GPU's ms per presented
 * frame and S the rate frames reach the screen. Called once per presented frame, inside a C2D
 * scene with the ABGR texture env, so it is also where the window accumulates. */
enum { LINES = 4 };

/* Frames drawn by the software renderer: GPU preflight refused them, or the GPU path is off. */
static uint64_t CpuRenderedFrames(void) {
    PortPpuGpu3DSStats gpu;
    PortPpuGpu3DS_GetStats(&gpu);
    return gpu.fallbackFrames + gpu.disabledFrames;
}

static void DrawFpsOverlay(void) {
    static struct {
        uint64_t startMs, logicStart, workStart;
        double gpuMs;
        unsigned frames;
        uint64_t cpuStart;
    } win;
    static float gameFps, logicMs, gpuMs;
    const uint64_t nowMs = osGetTime();
    if (!sRightEye) { /* counted once a frame, drawn for each eye */
    win.gpuMs += sStats.drawingTime > 0.0f ? sStats.drawingTime : 0.0f;
    ++win.frames;
    if (win.startMs == 0) {
        win.startMs = nowMs;
        win.logicStart = Platform3DS_LogicFrameCount();
        win.workStart = Platform3DS_EngineWorkTotalTicks();
        win.cpuStart = CpuRenderedFrames();
    } else if (nowMs - win.startMs >= 1000u) {
        const uint64_t elapsed = nowMs - win.startMs;
        const uint64_t logic = Platform3DS_LogicFrameCount();
        const uint64_t work = Platform3DS_EngineWorkTotalTicks();
        const uint64_t ticks = logic - win.logicStart;
        gameFps = (float)ticks * 1000.0f / (float)elapsed;
        logicMs = ticks ? (float)((double)(work - win.workStart) * 1000.0 /
                                  (double)Platform3DS_TicksPerSecond() / (double)ticks)
                        : 0.0f;
        gpuMs = (float)(win.gpuMs / win.frames);
        win.startMs = nowMs;
        win.logicStart = logic;
        win.workStart = work;
        {
            const uint64_t cpu = CpuRenderedFrames();
            win.cpuStart = cpu;
        }
        win.gpuMs = 0.0;
        win.frames = 0;
    }

    }

    char line[LINES][12];
    snprintf(line[0], sizeof(line[0]), "%.1f", gameFps);
    snprintf(line[1], sizeof(line[1]), "L%.1f", logicMs);
    snprintf(line[2], sizeof(line[2]), "G%.1f", gpuMs);
    snprintf(line[3], sizeof(line[3]), "S%.1f", Port_PPU_3DS_CurrentFps());
    int chars = 0;
    for (int i = 0; i < LINES; ++i) {
        const int n = (int)strlen(line[i]);
        if (n > chars) chars = n;
    }
    enum { PITCH = 10, MARGIN = 2 };
    const float w = (float)(chars * 6 + 3), h = (float)(LINES * PITCH + 2);
    /* In 3D the box is in front with the HUD (BG0 at priority 0): the same move per eye, in whole pixels. */
    float depthX = 0.0f;
    if (Stereo3DS_Active()) {
        const float shift = Stereo3DS_LayerShift(PPU_GPU3DS_BG0, 0, sRightEye ? STEREO3DS_RIGHT : STEREO3DS_LEFT);
        depthX = (float)(int)(shift < 0.0f ? shift - 0.5f : shift + 0.5f);
    }
    const float boxX = (float)MARGIN + depthX, boxY = 240.0f - MARGIN - h;
    C2D_DrawRectSolid(boxX, boxY, 0.7f, w, h, C2D_Color32(0, 0, 0, 150));
    const uint32_t light = C2D_Color32(230, 232, 240, 255);
    const uint32_t colors[LINES] = {
        gameFps >= 59.0f ? C2D_Color32(110, 220, 130, 255)
                         : (gameFps >= 50.0f ? C2D_Color32(240, 200, 80, 255)
                                             : C2D_Color32(240, 100, 100, 255)),
        light, light, C2D_Color32(140, 148, 170, 255),
    };
    for (int i = 0; i < LINES; ++i)
        DrawStatusTextColor(boxX + 2.0f, boxY + 2.0f + (float)(i * PITCH), 1.0f, line[i], colors[i]);
}

/* The top screen is 400x240 and the game image rarely covers all of it, so the
 * surrounding bars have to be black -- but they only need painting when the
 * layout changes, not every frame. Clearing is a full-screen GPU fill, and
 * presentation is the largest remaining cost on an Old 3DS. Three frames of
 * clearing covers every buffer in the swap chain. */
static struct {
    float x, y, w, h;
    int style;
    unsigned width;
    bool overlay;
    bool generatedArt;
} sTopLayout;
static unsigned sTopClearFrames = 3;

void PlatformGpu3DS_InvalidateTopBorder(void) { sTopClearFrames = 3; }

static void TopLayoutChanged(float x, float y, float w, float h, int style,
                             unsigned width, bool overlay) {
    const bool generatedArt = Port_Config_GetWideAi();
    if (sTopLayout.generatedArt != generatedArt || sTopLayout.x != x || sTopLayout.y != y || sTopLayout.w != w ||
        sTopLayout.h != h || sTopLayout.style != style ||
        sTopLayout.width != width || sTopLayout.overlay != overlay) {
        sTopLayout.x = x;
        sTopLayout.y = y;
        sTopLayout.w = w;
        sTopLayout.h = h;
        sTopLayout.style = style;
        sTopLayout.width = width;
        sTopLayout.overlay = overlay;
        sTopLayout.generatedArt = generatedArt;
        sTopClearFrames = 3;
    }
}

/* Report Task 2: present the frame with one static quad instead of letting
 * citro2d rebuild and re-upload vertices every frame.
 *
 * The quad lives in linear memory, is flushed once, and is rebuilt only when
 * the layout actually changes. The PPU's own vertex shader is reused with a
 * zero offset, so no new program is needed.
 *
 * Off by default: citro2d still draws the bottom screen, so its per-frame
 * vertex flush stays either way, which caps the saving well below what the
 * report assumes. And C2D_TargetClear -- which this path must keep -- is the
 * render-to-texture barrier whose removal caused the white and black screens.
 * Enable with gpu_static_quad=1 to measure it. */
static void BuildPresentQuad(float drawX, float drawY, float drawW, float drawH,
                             unsigned width, const Tex3DS_SubTexture* sub) {
    if (!sPresentQuad) return;
    /* The top target is 240 wide by 400 tall and the display rotates it, so a
     * screen-space rectangle has to be mapped across swapped axes: the screen's
     * horizontal extent runs along the target's tall axis, and its vertical
     * extent along the narrow one. Using screen axes directly drew the frame
     * rotated a quarter turn. */
    const float x0 = (drawY / 240.0f) * 2.0f - 1.0f;
    const float x1 = ((drawY + drawH) / 240.0f) * 2.0f - 1.0f;
    const float y0 = (drawX / 400.0f) * 2.0f - 1.0f;
    const float y1 = ((drawX + drawW) / 400.0f) * 2.0f - 1.0f;
    const float u0 = sub->left, u1 = sub->right;
    const float v0 = sub->top, v1 = sub->bottom;
    /* Corner order follows the rotation: u advances along the target's y axis
     * (screen horizontal), v along its x axis (screen vertical). */
    /* u runs along the target's tall axis (screen horizontal) and v along its
     * narrow one (screen vertical); both are inverted relative to the naive
     * pairing because the display rotation reverses each. */
    const int16_t pu0 = PpuGpu3DS_PackUV(u0);
    const int16_t pu1 = PpuGpu3DS_PackUV(u1);
    const int16_t pv0 = PpuGpu3DS_PackUV(v0);
    const int16_t pv1 = PpuGpu3DS_PackUV(v1);
    sPresentQuad[0] = (PresentVertex){ x0, y0, 0.0f, pu1, pv1 };
    sPresentQuad[1] = (PresentVertex){ x0, y1, 0.0f, pu0, pv1 };
    sPresentQuad[2] = (PresentVertex){ x1, y0, 0.0f, pu1, pv0 };
    sPresentQuad[3] = (PresentVertex){ x1, y1, 0.0f, pu0, pv0 };
    Platform3DS_CleanDataCache(sPresentQuad, 4 * sizeof(*sPresentQuad));
    sPresentQuadW = drawW;
    sPresentQuadH = drawH;
    sPresentQuadWidth = width;
    sPresentQuadValid = true;
}

static void DrawTopTexture(C3D_Tex* texture, unsigned width, unsigned height, Port3DSFullViewMode mode,
                           int cropX, int cropY, bool configureAbgr) {
    if (!texture) return;
    const bool fullView = mode != PORT_3DS_FULL_VIEW_FALLBACK;
    if (!fullView) {
        /* E2: 240 to 266 columns by 160 rows. */
        if (width < 240u) width = 240u;
        if (width > 266u) width = 266u;
        height = 160u;
        cropX = cropY = 0;
    }

    const int style = Port_Config_Get3DSDisplayStyle();
    TopView3DSPlan plan;
    /* Full View frames (400x240 outdoors, 200x120 at 2x indoors) take the
     * same plan as the software path's DrawTopImage. */
    TopView3DS_BuildPlan(!fullView, fullView, Port_Config_Get3DSAspectRatio(), style, mode, (int)width,
                         (int)height, (int)width, (int)height, cropX, cropY, &plan);
    const Port3DSFullViewPresentation* presentation = &plan.source;
    sTopPresentWidth = (unsigned)presentation->renderWidth;
    sTopPresentHeight = (unsigned)presentation->renderHeight;
    sTopValidSourceWidth = width;
    sTopValidSourceHeight = height;
    sTopPresentMode = plan.mode;
    sTopCropX = presentation->sourceX;
    sTopCropY = presentation->sourceY;

    sTopSubtexture = (Tex3DS_SubTexture){
        .width = (u16)presentation->sourceWidth,
        .height = (u16)presentation->sourceHeight,
        .left = (float)presentation->sourceX / texture->width,
        .top = 1.0f - (float)presentation->sourceY / texture->height,
        .right = (float)(presentation->sourceX + presentation->sourceWidth) / texture->width,
        .bottom = 1.0f -
                  (float)(presentation->sourceY + presentation->sourceHeight) / texture->height,
    };
    const C2D_Image image = { .tex = texture, .subtex = &sTopSubtexture };
    const C2D_DrawParams params = {
        .pos = { .x = (float)plan.drawX, .y = (float)plan.drawY,
                 .w = (float)plan.drawWidth, .h = (float)plan.drawHeight },
        .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
    };

    C2D_Prepare();
    TopLayoutChanged(params.pos.x, params.pos.y, params.pos.w, params.pos.h,
                     style, width, Port_Config_GetShowFps());

    /* Keep the clear as the render-to-texture submission boundary between
     * the PICA200 PPU target and its physical-screen sampling pass. */
    if (sTopClearFrames != 0 && !sRightEye) --sTopClearFrames;
    C2D_TargetClear(sEyeTarget, TopClearColor());
    BlurJob blur;
    const bool blurBorder = TopBlurWanted(&plan, &blur);
    if (blurBorder)
        TopBlurBuild(texture, &blur);
    else
        TopBlurInvalidate();
    const unsigned intermediateScale = (unsigned)plan.sharpBilinearScale;
    const unsigned intermediateWidth =
        intermediateScale == 2u ? (unsigned)presentation->sourceWidth * 2u : 0u;
    const unsigned intermediateHeight =
        intermediateScale == 2u ? (unsigned)presentation->sourceHeight * 2u : 0u;
    const bool useSharpBilinear =
        plan.useSharpBilinear && intermediateScale == 2u &&
        sSharpBilinearTarget &&
        intermediateWidth < SHARP_BILINEAR_TEXTURE_WIDTH &&
        intermediateHeight < SHARP_BILINEAR_TEXTURE_HEIGHT;

    if (useSharpBilinear) {
        const C2D_DrawParams integerParams = {
            .pos = { .x = 0.0f, .y = 0.0f,
                     .w = (float)intermediateWidth,
                     .h = (float)intermediateHeight },
            .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
        };
        C3D_TexSetFilter(texture, GPU_NEAREST, GPU_NEAREST);
        C2D_SceneBegin(sSharpBilinearTarget);
        ConfigureIdentityTextureEnv();
        ConfigureOpaqueOverwriteBlend();
        C2D_DrawImage(image, &integerParams, NULL);

        /* Duplicate the last source column and row into guard texels. Without
         * these, the final linear sample can bleed stale pixels from the
         * unused part of the power-of-two render target. */
        const Tex3DS_SubTexture rightEdgeSubtexture = {
            .width = 1,
            .height = (u16)presentation->sourceHeight,
            .left = (float)(presentation->sourceX + presentation->sourceWidth - 1) /
                    texture->width,
            .top = 1.0f - (float)presentation->sourceY / texture->height,
            .right = (float)(presentation->sourceX + presentation->sourceWidth) /
                     texture->width,
            .bottom = 1.0f -
                      (float)(presentation->sourceY + presentation->sourceHeight) /
                          texture->height,
        };
        const Tex3DS_SubTexture bottomEdgeSubtexture = {
            .width = (u16)presentation->sourceWidth,
            .height = 1,
            .left = (float)presentation->sourceX / texture->width,
            .top = 1.0f -
                   (float)(presentation->sourceY + presentation->sourceHeight - 1) /
                       texture->height,
            .right = (float)(presentation->sourceX + presentation->sourceWidth) /
                     texture->width,
            .bottom = 1.0f -
                      (float)(presentation->sourceY + presentation->sourceHeight) /
                          texture->height,
        };
        const Tex3DS_SubTexture cornerSubtexture = {
            .width = 1, .height = 1,
            .left = rightEdgeSubtexture.left,
            .top = bottomEdgeSubtexture.top,
            .right = rightEdgeSubtexture.right,
            .bottom = bottomEdgeSubtexture.bottom,
        };
        const C2D_Image rightEdgeImage = {
            .tex = texture, .subtex = &rightEdgeSubtexture
        };
        const C2D_Image bottomEdgeImage = {
            .tex = texture, .subtex = &bottomEdgeSubtexture
        };
        const C2D_Image cornerImage = {
            .tex = texture, .subtex = &cornerSubtexture
        };
        const C2D_DrawParams rightEdgeParams = {
            .pos = { .x = (float)intermediateWidth, .y = 0.0f,
                     .w = 1.0f, .h = (float)intermediateHeight },
            .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
        };
        const C2D_DrawParams bottomEdgeParams = {
            .pos = { .x = 0.0f, .y = (float)intermediateHeight,
                     .w = (float)intermediateWidth, .h = 1.0f },
            .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
        };
        const C2D_DrawParams cornerParams = {
            .pos = { .x = (float)intermediateWidth,
                     .y = (float)intermediateHeight,
                     .w = 1.0f, .h = 1.0f },
            .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
        };
        C2D_DrawImage(rightEdgeImage, &rightEdgeParams, NULL);
        C2D_DrawImage(bottomEdgeImage, &bottomEdgeParams, NULL);
        C2D_DrawImage(cornerImage, &cornerParams, NULL);

        sSharpBilinearSubtexture = (Tex3DS_SubTexture){
            .width = (u16)intermediateWidth,
            .height = (u16)intermediateHeight,
            .left = 0.0f, .top = 1.0f,
            .right = (float)intermediateWidth / SHARP_BILINEAR_TEXTURE_WIDTH,
            .bottom = 1.0f -
                      (float)intermediateHeight / SHARP_BILINEAR_TEXTURE_HEIGHT,
        };
        const C2D_Image intermediateImage = {
            .tex = &sSharpBilinearTexture,
            .subtex = &sSharpBilinearSubtexture,
        };
        C2D_SceneBegin(sEyeTarget);
        ConfigureStandardAlphaBlend();
        if (blurBorder && !blur.over) TopBlurDraw(&blur, false);
        ConfigureIdentityTextureEnv();
        C3D_TexSetFilter(&sSharpBilinearTexture, GPU_LINEAR, GPU_LINEAR);
        C2D_DrawImage(intermediateImage, &params, NULL);
        DrawFrameEdges(&intermediateImage, &params);
        if (!sRightEye) ++sStats.sharpBilinearFrames;
    } else {
        /* If the intermediate target is unavailable, Bilinear must remain a
         * bilinear mode rather than silently becoming nearest-neighbour. */
        const GPU_TEXTURE_FILTER_PARAM filter =
            (plan.linearFilter || plan.useSharpBilinear) ? GPU_LINEAR : GPU_NEAREST;
        C2D_SceneBegin(sEyeTarget);
        ConfigureStandardAlphaBlend();
        if (blurBorder && !blur.over) TopBlurDraw(&blur, false);
        C3D_TexSetFilter(texture, filter, filter);
        if (Port_Config_GpuStaticQuad() && sPresentQuad &&
            PortPpuGpu3DS_BindPresentShader()) {
            C2D_Flush();
            if (!sPresentQuadValid || sPresentQuadW != params.pos.w ||
                sPresentQuadH != params.pos.h || sPresentQuadWidth != width) {
                BuildPresentQuad(params.pos.x, params.pos.y, params.pos.w,
                                 params.pos.h, width, &sTopSubtexture);
            }
            C3D_BufInfo bufInfo;
            BufInfo_Init(&bufInfo);
            BufInfo_Add(&bufInfo, sPresentQuad, sizeof(PresentVertex), 2, 0x10);
            C3D_SetBufInfo(&bufInfo);
            C3D_TexBind(0, texture);
            C3D_TexEnv* env = C3D_GetTexEnv(0);
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
            C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0);
            C3D_CullFace(GPU_CULL_NONE);
            C3D_DrawArrays(GPU_TRIANGLE_STRIP, 0, 4);
            C2D_Prepare();
            C2D_SceneBegin(sEyeTarget);
            ConfigureIdentityTextureEnv();
            DrawFrameEdges(&image, &params);
        } else {
            ConfigureIdentityTextureEnv();
            C2D_DrawImage(image, &params, NULL);
            DrawFrameEdges(&image, &params);
        }
        if (plan.useSharpBilinear) if (!sRightEye) ++sStats.sharpBilinearFallbacks;
    }
    if (blurBorder && blur.over) {
        C2D_Flush();
        TopBlurDraw(&blur, false);
    }
    if (configureAbgr) ConfigureAbgrTextureEnv();

    DrawScenePictures(plan.drawX, plan.drawY, plan.drawWidth, plan.drawHeight, configureAbgr);
    if (Port_Config_GetShowFps()) DrawFpsOverlay();
}


/* Between frames: the right eye's target on the screen while stereo_3ds.h says 3D is on, and
 * the top screen in 3D mode with it. */
static void UpdateStereoOutput(void) {
    bool want = Stereo3DS_Active();
    if (want && !sTopRightTarget) {
        sTopRightTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
        if (!sTopRightTarget) want = false; /* no VRAM for it: stays 2D */
    }
    if (want == sStereoLinked) return;
    if (want)
        C3D_RenderTargetSetOutput(sTopRightTarget, GFX_TOP, GFX_RIGHT, sTopOutputFlags);
    else
        C3D_RenderTargetDetachOutput(sTopRightTarget);
    gfxSet3D(want);
    sStereoLinked = want;
    sTopClearFrames = 3;
}

/* After the left eye: the right one, the same way, on its own target. */
static void BeginRightEye(void) {
    sEyeTarget = sTopRightTarget;
    sRightEye = true;
}

static void EndRightEye(void) {
    sEyeTarget = sTopTarget;
    sRightEye = false;
}

void PlatformGpu3DS_BeginTop(const uint32_t* pixels, unsigned width, unsigned height,
                             unsigned validSourceWidth, unsigned validSourceHeight,
                             Port3DSFullViewMode mode, int cropX, int cropY) {
    if (!sReady || !pixels) return;
    const u8 frameFlags = (u8)(Port_Config_GpuFrameSync() ? C3D_FRAME_SYNCDRAW : 0);
    /* Preflight may already have opened the frame before selecting the CPU
     * fallback. Reuse it so the freshly rendered image is actually uploaded. */
    if (!sFrameActive) UpdateStereoOutput();
    if (!sFrameActive && !C3D_FrameBegin(frameFlags)) {
        ++sStats.frameBeginFailures;
        return;
    }
    sFrameActive = true;
    DrawTopImage(pixels, width, height, validSourceWidth, validSourceHeight,
                 mode, cropX, cropY);
    if (sStereoLinked) {
        BeginRightEye();
        DrawTopImage(pixels, width, height, validSourceWidth, validSourceHeight, mode, cropX, cropY);
        EndRightEye();
    }
    ++sStats.topTransfers;
}

bool PlatformGpu3DS_BeginBottomOnly(void) {
    if (!sReady) return false;
    if (sFrameActive) return true;
    if (!C3D_FrameBegin(0)) {
        ++sStats.frameBeginFailures;
        return false;
    }
    sFrameActive = true;
    return true;
}

bool PlatformGpu3DS_BeginCustomTop(void) {
    if (!sReady) return false;
    if (sFrameActive) return true;
    /* SYNCDRAW waits for the previous frame's drawing to retire. The command
     * buffers the PPU builder writes into are read by the GPU asynchronously,
     * so building the next frame before that wait would overwrite geometry
     * still being drawn -- invisible under an emulator whose GPU completes
     * instantly, a flicker on hardware. */
    /* C3D_FrameBegin waits on the GX queue with no timeout, so a command list
     * the GPU never retires stops the main thread here for good: both screens
     * hold their last contents, audio keeps playing on its own core, and no
     * quick dump can be taken because the dump runs on this thread. That is
     * what a watchdog caught as "stopped at stage 20". C3D_FRAME_NONBLOCK
     * returns false instead of waiting, so a wedged GPU costs a skipped frame
     * and leaves the console responsive and diagnosable. */
    /* NONBLOCK was an emergency measure while an unbounded wait could hang the
     * console. The hang had a cause -- zero-count draws, now never submitted --
     * and the counters show the queue is not wedged: a second begin later in
     * the same frame succeeds every time. What NONBLOCK produced instead was a
     * standoff. The GPU is busy at the top of a frame, so the PICA path is
     * skipped; the software path then spends 512 KB transferring, which keeps
     * the GPU busy into the next frame, so it is skipped again. beginFail rose
     * by exactly one per frame while attempted frames sat frozen at 170.
     * Waiting is the correct behaviour: it is a frame's worth of pacing, not a
     * deadlock, and the watchdog now catches it if that ever stops being true. */
    const u8 frameFlags = (u8)(Port_Config_GpuFrameSync() ? C3D_FRAME_SYNCDRAW : 0);
    UpdateStereoOutput();
    if (!C3D_FrameBegin(frameFlags)) {
        ++sStats.frameBeginFailures;
        return false;
    }
    sFrameActive = true;
    return true;
}

PlatformGpu3DSUploadLayout PlatformGpu3DS_SetCompactUpload(bool compact) {
    sUploadLayout = PlatformGpu3DS_GetUploadLayout(sOld3DSProfile && compact);
    sStats.topUploadPitch = sUploadLayout.topPitch;
    sStats.bottomUploadPitch = sUploadLayout.bottomPitch;
    sBottomTargetValid = false; /* redraw the bottom from the new stride */
    return sUploadLayout;
}

void PlatformGpu3DS_DrawTopTexture(void* texturePointer, void* rightTexturePointer, unsigned width,
                                   unsigned height, Port3DSFullViewMode mode, int cropX, int cropY) {
    C3D_Tex* texture = texturePointer;
    if (!sFrameActive || !texture) return;
    DrawTopTexture(texture, width, height, mode, cropX, cropY, false);
    if (sStereoLinked) {
        BeginRightEye();
        DrawTopTexture(rightTexturePointer ? (C3D_Tex*)rightTexturePointer : texture, width, height, mode, cropX,
                       cropY, false);
        EndRightEye();
    }
}

bool PlatformGpu3DS_QueueRgba5551Readback(void* texturePointer, uint16_t* pixels) {
    C3D_Tex* texture = texturePointer;
    if (!sFrameActive || !texture || !texture->data || !pixels ||
        texture->fmt != GPU_RGBA5551)
        return false;
    const size_t bytes = (size_t)texture->width * texture->height * sizeof(*pixels);
    if (R_FAILED(GSPGPU_FlushDataCache(pixels, bytes))) return false;
    /* Retire the queued PPU draws before the transfer reads the target.
     * C3D_FrameSplit takes GX_CMDLIST_* flags; C3D_FRAME_SYNCDRAW belongs to
     * C3D_FrameBegin and would set GX_CMDLIST_UPDATE_GAS_ACC here. */
    SplitFlushed();
    /* The top presenter samples visible row 0 at v=1 and its software upload
     * uses the same no-flip transfer. Untiling with no flip is therefore the
     * inverse mapping: linear row y is the visible row y, not raw Morton data. */
    C3D_SyncDisplayTransfer(
        (u32*)texture->data, GX_BUFFER_DIM(texture->width, texture->height),
        (u32*)pixels, GX_BUFFER_DIM(texture->width, texture->height),
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
            GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB5A1) |
            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB5A1) |
            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    return true;
}


bool PlatformGpu3DS_EndBottom(const uint32_t* pixels, bool changed) {
    if (!sFrameActive || !pixels) return false;
    DrawUpdateTop();
    if (changed) {
        /* NOT a blocking transfer. citro3d source/renderqueue.c:417-430 shows
         * C3D_SyncDisplayTransfer only blocks when called OUTSIDE a frame:
         *
         *   if (inFrame) { C3D_FrameSplit(0); GX_DisplayTransfer(...); }
         *   else        { C3Di_SafeDisplayTransfer(...); gspWaitForPPF(); }
         *
         * This runs inside an active frame, so it queues and returns. The timer
         * below therefore measures a queue append, not DMA -- which is why the
         * emulator read 0.029 ms. That figure was correct, not an artifact.
         *
         * A count correlation (378 of these against ~362 overrunning frames)
         * made this look like the overrun mechanism. It is not. The CPU block is
         * C3D_FrameBegin -> C3Di_WaitAndClearQueue(-1), waiting on the PREVIOUS
         * frame's whole GPU workload. Shrinking this payload still helps, but by
         * reducing GPU work so that next wait is shorter -- not by shortening
         * anything here. */
        const uint64_t transferStart = svcGetSystemTick();
        const size_t bottomFlushBytes =
            (size_t)sUploadLayout.bottomPitch * 240u * sizeof(uint32_t);
        Platform3DS_CleanDataCache(pixels, bottomFlushBytes);
        SplitFlushed();
        C3D_SyncDisplayTransfer((u32*)pixels,
                                GX_BUFFER_DIM(sUploadLayout.bottomPitch, sUploadLayout.bottomRows),
                                (u32*)sBottomTexture.data, GX_BUFFER_DIM(512, 256),
                                BottomTextureTransfer());
        const uint64_t transferTicks = svcGetSystemTick() - transferStart;
        sStats.bottomTransferTicks += transferTicks;
        if (transferTicks > sStats.bottomTransferMaxTicks)
            sStats.bottomTransferMaxTicks = transferTicks;
        ++sStats.bottomTransfers;
    }
    if (!sOld3DSProfile || changed || !sBottomTargetValid) {
        sBottomSubtexture = (Tex3DS_SubTexture){
            .width = 320, .height = 240, .left = 0.0f, .top = 1.0f,
            .right = 320.0f / 512.0f, .bottom = 1.0f - 240.0f / 256.0f,
        };
        const C2D_Image image = { .tex = &sBottomTexture, .subtex = &sBottomSubtexture };
        const C2D_DrawParams params = {
            .pos = { .x = 0.0f, .y = 0.0f, .w = 320.0f, .h = 240.0f },
            .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
        };
        C2D_TargetClear(sBottomTarget, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(sBottomTarget);
        C2D_DrawImage(image, &params, NULL);
        ConfigureAbgrTextureEnv();
        sBottomTargetValid = true;
        ++sStats.bottomTargetDraws;
    } else {
        /* The physical bottom image and its hitbox generation are unchanged.
         * Old 3DS can leave that render target displayed instead of clearing,
         * drawing and scheduling an identical output transfer on every top
         * presentation. New 3DS retains the established two-target frame. */
        ++sStats.bottomTargetReuseSkips;
    }
    C2D_Flush();
    if (sC2dFlushBase && sC2dFlushSize) {
        /* One GSP round trip per presented frame: the dump's own counters show
         * 876609536 bytes / 65536 per call = 13375 calls against 13376 frames.
         * At the ~330 us platform_3ds.c:638 measured for this IPC that is
         * ~0.33 ms/frame, spent cleaning 64 KiB that svcStoreProcessDataCache
         * cleans locally in microseconds without waking core 1. */
        Platform3DS_CleanDataCache(sC2dFlushBase, sC2dFlushSize);
        sStats.boundedFlushBytes += sC2dFlushSize;
    }
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    ++sStats.frames;
    sStats.drawingTime = C3D_GetDrawingTime();
    sStats.processingTime = C3D_GetProcessingTime();
    sFrameActive = false;
    return true;
}

void PlatformGpu3DS_ShowDumpSavedOverlay(void* currentTopTexture) {
    if (!sReady || !sTopUpload || !sBottomUploads[0] || !C3D_FrameBegin(C3D_FRAME_NONBLOCK)) return;
    /* This frame paints outside the usual layout. */
    PlatformGpu3DS_InvalidateTopBorder();
    sFrameActive = true;
    /* The CPU upload is intentionally stale while PICA renders the game.
     * Re-present the live output texture so saving a dump never flashes an
     * older CPU/parity frame. */
    if (currentTopTexture)
        DrawTopTexture((C3D_Tex*)currentTopTexture, sTopValidSourceWidth, sTopValidSourceHeight, sTopPresentMode,
                       sTopCropX, sTopCropY, false);
    else
        DrawTopImage(sTopUpload, sTopPresentWidth, sTopPresentHeight,
                     sTopValidSourceWidth, sTopValidSourceHeight,
                     sTopPresentMode, sTopCropX, sTopCropY);

    /* Start a clean overlay batch. PICA scissor/blend state is global and a
     * leaked scanline clip was what truncated the end of this label. */
    C2D_Prepare();
    C2D_SceneBegin(sTopTarget);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    ConfigureStandardAlphaBlend();
    C2D_DrawRectSolid(124.0f, 12.0f, 0.7f, 152.0f, 24.0f, C2D_Color32(0, 0, 0, 220));
    DrawStatusText(141.0f, 17.0f, 2.0f, "DUMP SAVED");

    sBottomSubtexture = (Tex3DS_SubTexture){
        .width = 320, .height = 240, .left = 0.0f, .top = 1.0f,
        .right = 320.0f / 512.0f, .bottom = 1.0f - 240.0f / 256.0f,
    };
    const C2D_Image bottomImage = { .tex = &sBottomTexture, .subtex = &sBottomSubtexture };
    const C2D_DrawParams bottomParams = {
        .pos = { .x = 0.0f, .y = 0.0f, .w = 320.0f, .h = 240.0f },
        .center = { 0.0f, 0.0f }, .depth = 0.0f, .angle = 0.0f,
    };
    C2D_SceneBegin(sBottomTarget);
    C2D_DrawImage(bottomImage, &bottomParams, NULL);
    ConfigureAbgrTextureEnv();
    C2D_Flush();
    /* Bottom-only presentation path: same per-frame GSP round trip as above. */
    if (sC2dFlushBase && sC2dFlushSize) Platform3DS_CleanDataCache(sC2dFlushBase, sC2dFlushSize);
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    sFrameActive = false;
    gspWaitForEvent(GSPGPU_EVENT_VBlank0, false);
    svcSleepThread(600000000LL);
}

void PlatformGpu3DS_GetStats(PlatformGpu3DSStats* stats) {
    if (stats) *stats = sStats;
}

void PlatformGpu3DS_InvalidateBottomTarget(void) {
    /* HOME and lid sleep may invalidate or rotate the physical framebuffer.
     * Force one opaque redraw after APT resumes before Old 3DS starts reusing
     * the unchanged target again. */
    sBottomTargetValid = false;
    sTopPresentWidth = 240;
    sTopPresentHeight = 160;
    sTopValidSourceWidth = 240;
    sTopValidSourceHeight = 160;
    sTopPresentMode = PORT_3DS_FULL_VIEW_FALLBACK;
    sTopCropX = 0;
    sTopCropY = 0;
}

/* citro3d ends a frame asynchronously: the swap is a callback when its GPU
 * queue finishes. If the GPU is taken away (HOME, the lid, a jump to another
 * title) with a frame in flight, that frame is never acknowledged: every later
 * wait on the queue (C3D_RenderTargetDelete, C3D_Fini) never returns, and a
 * citro3d left half done freezes the next application, which after SITE
 * INSTALL is this game's new build (issue #2; ../sm-3ds issue #20 found it).
 * An empty frame drains the queue while the GPU is still ours; on a thread
 * with a timeout, so a queue that will not drain costs half a second. */
static void DrainThread(void* arg) {
    (void)arg;
    if (C3D_FrameBegin(0)) C3D_FrameEnd(0);
}

bool PlatformGpu3DS_DrainQueue(void) {
    if (!sReady || sFrameActive) return true;
    Thread thread = threadCreate(DrainThread, NULL, 16 * 1024, 0x30, -2, false);
    if (!thread) return false;
    /* A timeout is a positive result code: only 0 means the thread ended. */
    if (threadJoin(thread, 500ull * 1000 * 1000) != 0) return false;
    threadFree(thread);
    return true;
}

static void RunThread(void* arg) {
    ((void (*)(void))arg)();
}

bool PlatformGpu3DS_RunWithTimeout(void (*fn)(void), unsigned milliseconds) {
    Thread thread = threadCreate(RunThread, (void*)fn, 32 * 1024, 0x30, -2, false);
    if (!thread) {
        fn();
        return true;
    }
    if (threadJoin(thread, (u64)milliseconds * 1000000ull) != 0) return false; /* left stuck */
    threadFree(thread);
    return true;
}

void PlatformGpu3DS_Shutdown(void) {
    if (sUpdateReady) C3D_TexDelete(&sUpdateTexture);
    if (sUpdatePixels) linearFree(sUpdatePixels);
    sUpdateReady = false;
    sUpdatePixels = NULL;
    if (!sReady) return;
    if (sFrameActive) {
        C2D_Flush();
        if (sC2dFlushBase && sC2dFlushSize) GSPGPU_FlushDataCache(sC2dFlushBase, sC2dFlushSize);
        C3D_FrameEnd(GX_CMDLIST_FLUSH);
    }
    if (!aptShouldClose()) C3D_FrameSync();
    for (unsigned i = 0; i < 2; ++i) {
        if (sPauseUpdateTargets[i]) C3D_RenderTargetDelete(sPauseUpdateTargets[i]);
        sPauseUpdateTargets[i] = NULL;
    }
    sPauseUpdateLinked = false;
    TopBlurShutdown();
    C3D_RenderTargetDelete(sBottomTarget);
    C3D_RenderTargetDelete(sTopTarget);
    if (sTopRightTarget) C3D_RenderTargetDelete(sTopRightTarget);
    sTopRightTarget = NULL;
    sStereoLinked = false;
    if (sSharpBilinearTarget) C3D_RenderTargetDelete(sSharpBilinearTarget);
    if (sStats.sharpBilinearAvailable) C3D_TexDelete(&sSharpBilinearTexture);
    C3D_TexDelete(&sBottomTexture);
    C3D_TexDelete(&sTopTexture);
    C2D_Fini();
    C3D_Fini();
    linearFree(sBottomUploads[1]);
    linearFree(sBottomUploads[0]);
    linearFree(sTopUpload);
    sBottomUploads[0] = NULL;
    sBottomUploads[1] = NULL;
    sTopUpload = NULL;
    sC2dFlushBase = NULL;
    sC2dFlushSize = 0;
    sFrameActive = false;
    sReady = false;
    sOld3DSProfile = false;
    sBottomTargetValid = false;
    sSharpBilinearTarget = NULL;
    sUploadLayout = (PlatformGpu3DSUploadLayout){ 0 };
}
