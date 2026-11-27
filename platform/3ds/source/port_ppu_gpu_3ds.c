#include "platform_3ds.h"
#include "port_ppu_gpu_3ds.h"
#include "stereo_3ds.h"
#include "sprite_lean_3ds.h"
#include "zone_depth_3ds.h"

#include "platform_gpu_3ds.h"
#include "ppu_gpu_3ds_budget.h"

#include "ppu_gpu_3ds_shader_shbin.h"

extern bool Port_Config_GpuViewportOffset(void);
extern int Port_Config_GpuScissorMode(void);
extern bool Port_Config_GpuStencil(void);
void Platform3DS_Debug(const char* message);

#include <3ds.h>
#include <citro3d.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum {
    /* Half the vertex space is reserved for the per-layer map slices, so the
     * per-band fallback needs headroom of its own; 16-bit indices cap this at
     * 65536 vertices. 1.2 MB of linear memory against ~12 MB free. */
    /* Heavy frames wanted 49500 vertices against a 49148 capacity and fell back
     * to a full software render, which costs about 15 ms -- far more than the
     * memory. Indices are 16-bit, so 65536 vertices is the hard ceiling and the
     * reset quad still lands at 65532..65535. 65536 quads' worth of indices is
     * 98304. The extra ~440 KB comes from a linear heap with megabytes free. */
    PPU_GPU3DS_MAX_VERTICES = 65536,
    PPU_GPU3DS_MAX_INDICES = 98304,
    /* The tail of each buffer belongs to the backend, not the builder: four
     * vertices and six indices for a full-screen quad that resets the stencil.
     * See ClearStencilPlane. */
    PPU_GPU3DS_CLEAR_FIRST_VERTEX = PPU_GPU3DS_MAX_VERTICES - 4,
    PPU_GPU3DS_CLEAR_FIRST_INDEX = PPU_GPU3DS_MAX_INDICES - 6,
    /* And the quad before it, which copies the left eye's picture to the right (CopyLeftOutput). */
    PPU_GPU3DS_COPY_FIRST_VERTEX = PPU_GPU3DS_CLEAR_FIRST_VERTEX - 4,
    PPU_GPU3DS_COPY_FIRST_INDEX = PPU_GPU3DS_CLEAR_FIRST_INDEX - 6,
    PPU_GPU3DS_BUILD_VERTICES = PPU_GPU3DS_COPY_FIRST_VERTEX,
    PPU_GPU3DS_BUILD_INDICES = PPU_GPU3DS_COPY_FIRST_INDEX,
    PPU_GPU3DS_MAX_BATCHES = 4096,
    PPU_GPU3DS_OUTPUT_WIDTH = 512,
    PPU_GPU3DS_OUTPUT_HEIGHT = 256,
    PPU_GPU3DS_REGION_SHIFT = 14,
    /* Coalescing budget for atlas uploads: a GSP flush call costs far more
     * than flushing the slots sitting in a gap. */
    PPU_GPU3DS_ATLAS_FLUSH_RANGES = 2,
    PPU_GPU3DS_ATLAS_FLUSH_GAP = 64,
    /* A scene that falls back for this many frames will not produce a verdict
     * worth waiting for. */
    PPU_GPU3DS_PARITY_MAX_DEFERRALS = 240,
    PPU_GPU3DS_ALPHA_COMPLEMENT = 1u << 13u,
};

static PpuGpu3DSCache* sCache;
static uint32_t sDirtyBitmap[PPU_GPU3DS_SLOT_COUNT / 32];
/* The frame is built while the GPU still draws the previous one (docs/gpu-build-ahead.md):
 * into the vertex buffer the GPU is not reading, and into copies of the atlas and the rings
 * that PortPpuGpu3DS_Commit moves to the textures once the GPU is done with them.
 * sVertices is the buffer being built. */
static PpuGpu3DSVertex* sVertexBuffers[2];
static C3D_BufInfo sBufferInfo[2];
static unsigned sBuildBuffer, sDrawnBuffer = 1;
/* Generation of each background's map slice, and the one each buffer holds (0: none). */
static uint32_t sSliceGeneration[4], sBufferSliceGeneration[2][4];
static uint16_t* sAtlasShadow;
static uint16_t* sRingShadow[PPU_GPU3DS_RING_COUNT];
static uint32_t sRingCells[PPU_GPU3DS_RING_CELLS / 32];
static bool sCommitPending;
/* The second vertex buffer and the copies could be allocated: without them the build writes
 * the textures directly and the frame is claimed before it, as it always was. */
static bool sCanBuildAhead;
/* Debug: poke 1 to claim the GPU frame before building, as before the build ran ahead
 * (tmc-link.py poke gPpuGpu3DSSerialBuild 01), for timing one against the other. */
volatile uint8_t gPpuGpu3DSSerialBuild;
static PpuGpu3DSVertex* sVertices;
static uint16_t* sIndices;
static PpuGpu3DSBatch* sBatches;
static PpuGpu3DSCommandBuffer sCommands;
static uint16_t* sParityReference;
static uint16_t* sParityReadback;
static C3D_Tex sAtlas;
/* One view ring per background (port_ppu_gpu_3ds_model.h), sampled by the
 * batches whose `texture` field is 1 + the background. */
static C3D_Tex sRingTex[PPU_GPU3DS_RING_COUNT];
/* BG1 drawn from a picture (PortPpuGpu3DS_SetBg1Picture): made when one is set, freed when cleared. */
static C3D_Tex sPictureTex;
static const uint16_t* sPicturePixels;
static bool sPictureCorrected;
/* Or from palette indices (PortPpuGpu3DS_SetIndexedPicture): the indices in the texture's
 * layout, and the palettes the texels were last made from. */
static const uint8_t* sPictureIndices;
static uint8_t* sPictureSwizzled;
static unsigned sPictureFirstPalette, sPictureTileRows, sPictureTileCols;
static uint16_t sPicturePalettes[64];
static bool sPicturePalettesValid;
/* Side strips per BG (PortPpuGpu3DS_SetScrollPicture): a texture each, made when one is set, freed when cleared. */
static C3D_Tex sScrollTex[MODE1_GBA_BG_COUNT];
static const uint16_t* sScrollPixels[MODE1_GBA_BG_COUNT];
static bool sScrollCorrected[MODE1_GBA_BG_COUNT];
static unsigned sBoundTexture;
static C3D_Tex sOutputTexture;
static C3D_RenderTarget* sOutputTarget;
/* The right eye's picture when layers have a depth (stereo_3ds.h): made on first use. */
static C3D_Tex sOutputRightTexture;
static C3D_RenderTarget* sOutputRightTarget;
static bool sRightReady;
static bool sEyeShifting;
/* While a ghost copy is drawn (IsGhostBatch) nothing moves. */
static bool sNoShift;
static float sEyeShiftNdc[PPU_GPU3DS_OBJ + 1][4];
/* The eye the shifts are for, and pixels to NDC: a sprite with a depth of its own (PpuGpu3DSBatch.depth) is moved
 * by its own shift (SpritePx). */
static Stereo3DSEye sEyeCurrent;
static float sEyeToNdc;

static int RoundPx(float shift) {
    return (int)(shift < 0.0f ? shift - 0.5f : shift + 0.5f);
}
/* A sprite that leans across its columns (PpuGpu3DSBatch.side) takes its whole disparity in one eye, so that its
 * columns, moved by different amounts, overlap instead of parting (SpriteLean3DS_EyeShift). */
static int SpritePx(const PpuGpu3DSBatch* batch) {
    if (batch->side == SPRITE_LEAN_3DS_SIDE_NONE) return RoundPx(Stereo3DS_SpriteShift(batch->depth, sEyeCurrent));
    return SpriteLean3DS_EyeShift(RoundPx(Stereo3DS_SpriteShift(batch->depth, STEREO3DS_LEFT)),
                                  RoundPx(Stereo3DS_SpriteShift(batch->depth, STEREO3DS_RIGHT)), batch->side,
                                  sEyeCurrent == STEREO3DS_LEFT);
}
/* The same in whole pixels, for the batch's scissor: a layer's regions move with it, or their
 * edges cut what moved (the R button split in two, a heart cut by the next). */
static int sEyeShiftPx[PPU_GPU3DS_OBJ + 1][4];
/* The OBJ-window masks' own shift when they follow a layer (Stereo3DS_ObjWindowFollows). */
static bool sEyeWindowFollows;
static int sEyeWindowPx;
static float sEyeWindowNdc;
static DVLB_s* sShader;
static shaderProgram_s sProgram;
static int sOffsetUniform = -1;
static float sLastOffsetX, sLastOffsetY;
static C3D_AttrInfo sAttributes;
static bool sProgramInitialized;
static bool sReady;
static bool sDisabled;
static bool sPrepared;
static unsigned sPreparedWidth;
static unsigned sPreparedHeight;
static uint32_t sFrame;
static bool sParityRequested;
static unsigned sParityDeferrals;
static bool sParityReferenceReady;
static bool sParitySubmitted;
static bool sParityFinishedThisFrame;
static unsigned sParityWidth;
static unsigned sParityHeight;
/* The prepared frame's screen fade (PpuGpu3DSFrameView::fadeWeight/fadeLevel). */
static uint8_t sFadeWeight, sFadeLevel;
/* The prepared frame's iris mask (PpuGpu3DSFrameView::irisLines): quads of
 * the backdrop colour outside each line's {left, right}, one per run of equal
 * lines and side, appended after the frame's geometry and drawn in one call. */
static size_t sIrisFirstIndex, sIrisIndexCount;
static uint16_t sIrisColor;

static void AppendIrisQuad(unsigned left, unsigned top, unsigned right, unsigned bottom, unsigned width,
                           unsigned height) {
    PpuGpu3DSVertex* quad = sVertices + sCommands.vertexCount;
    const float x0 = 2.0f * (float)left / (float)width - 1.0f, x1 = 2.0f * (float)right / (float)width - 1.0f;
    const float y0 = 1.0f - 2.0f * (float)top / (float)height, y1 = 1.0f - 2.0f * (float)bottom / (float)height;
    quad[0] = (PpuGpu3DSVertex){ x0, y0, 0.0f, 0, 0 };
    quad[1] = (PpuGpu3DSVertex){ x1, y0, 0.0f, 0, 0 };
    quad[2] = (PpuGpu3DSVertex){ x1, y1, 0.0f, 0, 0 };
    quad[3] = (PpuGpu3DSVertex){ x0, y1, 0.0f, 0, 0 };
    sCommands.vertexCount += 4;
    sIrisIndexCount += 6;
}

static void AppendIrisMask(const PpuGpu3DSFrameView* frame) {
    sIrisIndexCount = 0;
    if (frame->irisLines == NULL) return;
    /* The static index pattern draws quad q from vertices 4q..4q+3 (indices 6q..). */
    sCommands.vertexCount = (sCommands.vertexCount + 3u) & ~(size_t)3u;
    sIrisFirstIndex = sCommands.vertexCount / 4u * 6u;
    sIrisColor = (uint16_t)(frame->memory.bg_palette[0] & 0x7fffu);
    unsigned first = 0;
    for (unsigned line = 1; line <= frame->height; ++line) {
        const uint16_t* a = &frame->irisLines[first * 2u];
        if (line < frame->height && frame->irisLines[line * 2u] == a[0] && frame->irisLines[line * 2u + 1u] == a[1])
            continue;
        if (sCommands.vertexCount + 8u > PPU_GPU3DS_BUILD_VERTICES) break;
        const unsigned left = a[0], right = a[1];
        if (left >= right) {
            AppendIrisQuad(0, first, frame->width, line, frame->width, frame->height);
        } else {
            if (left > 0) AppendIrisQuad(0, first, left, line, frame->width, frame->height);
            if (right < frame->width) AppendIrisQuad(right, first, frame->width, line, frame->width, frame->height);
        }
        first = line;
    }
}
/* Debug: poke 1 to skip the fade's full-screen quad (the picture then shows the
 * unfaded palettes), to time it (tmc-link.py poke gPpuGpu3DSFadeQuadDisable 01). */
volatile uint8_t gPpuGpu3DSFadeQuadDisable;
/* Debug: 1 draws both eyes whole again instead of over one base (DrawEyesOverBase), to compare the two
 * (tmc-link.py poke gPpuGpu3DSEyesWhole 01). */
volatile uint8_t gPpuGpu3DSEyesWhole;
static PortPpuGpu3DSStats sStats;

void PortPpuGpu3DS_Shutdown(void) {
    sPrepared = false;
    sReady = false;

    if (sOutputTarget) C3D_RenderTargetDelete(sOutputTarget);
    if (sOutputTexture.data) C3D_TexDelete(&sOutputTexture);
    if (sOutputRightTarget) C3D_RenderTargetDelete(sOutputRightTarget);
    if (sOutputRightTexture.data) C3D_TexDelete(&sOutputRightTexture);
    sOutputRightTarget = NULL;
    sOutputRightTexture = (C3D_Tex){ 0 };
    sRightReady = false;
    if (sPictureTex.data) C3D_TexDelete(&sPictureTex);
    memset(&sPictureTex, 0, sizeof(sPictureTex));
    sPicturePixels = NULL;
    for (unsigned bg = 0; bg < MODE1_GBA_BG_COUNT; ++bg) {
        if (sScrollTex[bg].data) C3D_TexDelete(&sScrollTex[bg]);
        memset(&sScrollTex[bg], 0, sizeof(sScrollTex[bg]));
        sScrollPixels[bg] = NULL;
    }
    free(sPictureSwizzled);
    sPictureSwizzled = NULL;
    sPictureIndices = NULL;
    if (sProgramInitialized) shaderProgramFree(&sProgram);
    if (sShader) DVLB_Free(sShader);
    for (unsigned ring = 0; ring < PPU_GPU3DS_RING_COUNT; ++ring)
        if (sRingTex[ring].data) C3D_TexDelete(&sRingTex[ring]);
    if (sAtlas.data) C3D_TexDelete(&sAtlas);
    free(sParityReference);
    if (sParityReadback) linearFree(sParityReadback);
    free(sBatches);
    if (sIndices) linearFree(sIndices);
    if (sVertexBuffers[1] && sVertexBuffers[1] != sVertexBuffers[0]) linearFree(sVertexBuffers[1]);
    if (sVertexBuffers[0]) linearFree(sVertexBuffers[0]);
    memset(sVertexBuffers, 0, sizeof(sVertexBuffers));
    sCanBuildAhead = false;
    free(sAtlasShadow);
    sAtlasShadow = NULL;
    for (unsigned ring = 0; ring < PPU_GPU3DS_RING_COUNT; ++ring) {
        free(sRingShadow[ring]);
        sRingShadow[ring] = NULL;
    }
    sCommitPending = false;
    free(sCache);

    sCache = NULL;
    sVertices = NULL;
    sIndices = NULL;
    sBatches = NULL;
    sParityReference = NULL;
    sParityReadback = NULL;
    sOutputTarget = NULL;
    sShader = NULL;
    sProgramInitialized = false;
    sDisabled = false;
    sParityRequested = false;
    sParityReferenceReady = false;
    sParitySubmitted = false;
    sParityFinishedThisFrame = false;
    sCommands = (PpuGpu3DSCommandBuffer){ 0 };
    sPreparedWidth = 0;
    sPreparedHeight = 0;
    sFrame = 0;
    sParityWidth = 0;
    sParityHeight = 0;
    sStats.initialized = false;
    sStats.enabled = false;
    sStats.disabled = false;
    sAtlas = (C3D_Tex){ 0 };
    memset(sRingTex, 0, sizeof(sRingTex));
    sOutputTexture = (C3D_Tex){ 0 };
    sProgram = (shaderProgram_s){ 0 };
    sAttributes = (C3D_AttrInfo){ 0 };
    memset(sBufferInfo, 0, sizeof(sBufferInfo));
}

extern bool Port_Config_GetColorCorrection(void);

/* The picture texture, the colour one or the indexed one, gone. */
static void ReleasePicture(void) {
    if (sPictureTex.data) C3D_TexDelete(&sPictureTex);
    memset(&sPictureTex, 0, sizeof(sPictureTex));
    sPicturePixels = NULL;
    free(sPictureSwizzled);
    sPictureSwizzled = NULL;
    sPictureIndices = NULL;
    sBoundTexture = UINT8_MAX;
}

static bool MakePictureTexture(void) {
    if (!C3D_TexInit(&sPictureTex, PPU_GPU3DS_PICTURE_TEX_WIDTH, PPU_GPU3DS_PICTURE_TEX_HEIGHT, GPU_RGBA5551)) {
        memset(&sPictureTex, 0, sizeof(sPictureTex));
        return false;
    }
    memset(sPictureTex.data, 0, sPictureTex.size);
    C3D_TexSetFilter(&sPictureTex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sPictureTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sBoundTexture = UINT8_MAX; /* rebind */
    return true;
}

void PortPpuGpu3DS_SetIndexedPicture(const uint8_t* indices, unsigned width, unsigned height, unsigned firstPalette) {
    if (indices == sPictureIndices && (indices == NULL || firstPalette == sPictureFirstPalette)) return;
    if (sPictureIndices != NULL || indices != NULL) ReleasePicture();
    if (indices == NULL || width > PPU_GPU3DS_PICTURE_TEX_WIDTH || height > PPU_GPU3DS_PICTURE_TEX_HEIGHT ||
        firstPalette + 4u > 16u)
        return;
    sPictureSwizzled = calloc(PPU_GPU3DS_PICTURE_TEX_WIDTH * PPU_GPU3DS_PICTURE_TEX_HEIGHT, 1);
    if (sPictureSwizzled == NULL || !MakePictureTexture()) {
        ReleasePicture();
        return;
    }
    /* In the texture's layout (8x8 Morton tiles, tile rows from the top, as the rings), so the
     * texels are remade by one walk over the tiles the picture covers. */
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            sPictureSwizzled[((y >> 3) * (PPU_GPU3DS_PICTURE_TEX_WIDTH >> 3) + (x >> 3)) * 64u +
                             PpuGpu3DS_MortonIndex(x & 7u, y & 7u)] = indices[y * width + x];
    sPictureIndices = indices;
    sPictureFirstPalette = firstPalette;
    sPictureTileRows = (height + 7u) >> 3;
    sPictureTileCols = (width + 7u) >> 3;
    sPicturePalettesValid = false;
}

/* The indexed picture's texels from the palettes the frame is drawn with (unfaded under a GPU
 * fade, as the tiles), when they or the colour correction changed. Run once the GPU frame is
 * claimed, as the atlas's writes are. */
static bool RefreshIndexedPicture(const PpuGpu3DSFrameView* frame) {
    if (sPictureIndices == NULL || sPictureTex.data == NULL) return true;
    const uint16_t* palettes = frame->memory.bg_palette + sPictureFirstPalette * 16u;
    const bool corrected = Port_Config_GetColorCorrection();
    if (sPicturePalettesValid && corrected == sPictureCorrected &&
        memcmp(palettes, sPicturePalettes, sizeof(sPicturePalettes)) == 0)
        return true;
    memcpy(sPicturePalettes, palettes, sizeof(sPicturePalettes));
    sPictureCorrected = corrected;
    sPicturePalettesValid = true;
    uint16_t colours[64];
    for (unsigned i = 0; i < 64; ++i) colours[i] = (i & 15u) ? PpuGpu3DS_PackRgba5551(palettes[i], true) : 0;
    uint16_t* texels = (uint16_t*)sPictureTex.data;
    const size_t rowTexels = (size_t)(PPU_GPU3DS_PICTURE_TEX_WIDTH >> 3) * 64u;
    const size_t used = (size_t)sPictureTileCols * 64u;
    for (unsigned row = 0; row < sPictureTileRows; ++row) {
        const uint8_t* in = sPictureSwizzled + row * rowTexels;
        uint16_t* out = texels + row * rowTexels;
        for (size_t i = 0; i < used; ++i) out[i] = in[i] ? colours[in[i] & 63u] : 0;
    }
    return Platform3DS_CleanDataCache(texels, sPictureTileRows * rowTexels * sizeof(uint16_t));
}

void PortPpuGpu3DS_SetBg1Picture(const uint16_t* gbaPixels, unsigned width, unsigned height) {
    const bool corrected = Port_Config_GetColorCorrection();
    if (sPictureIndices != NULL) {
        if (gbaPixels == NULL) return; /* the indexed picture is not this one's to clear */
        ReleasePicture();
    }
    if (gbaPixels == sPicturePixels && (gbaPixels == NULL || corrected == sPictureCorrected)) return;
    ReleasePicture();
    if (gbaPixels == NULL || width > PPU_GPU3DS_PICTURE_TEX_WIDTH || height > PPU_GPU3DS_PICTURE_TEX_HEIGHT ||
        !MakePictureTexture())
        return;
    /* 8x8 Morton tiles, tile rows from the top (v = 1), as the rings are laid out. */
    uint16_t* texels = (uint16_t*)sPictureTex.data;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            texels[((y >> 3) * (PPU_GPU3DS_PICTURE_TEX_WIDTH >> 3) + (x >> 3)) * 64u +
                   PpuGpu3DS_MortonIndex(x & 7u, y & 7u)] = PpuGpu3DS_PackRgba5551(gbaPixels[y * width + x], gbaPixels[y * width + x] != 0);
    C3D_TexFlush(&sPictureTex);
    sPicturePixels = gbaPixels;
    sPictureCorrected = corrected;
}

void PortPpuGpu3DS_SetScrollPicture(unsigned bg, const uint16_t* gbaPixels, unsigned width, unsigned height, bool repeat) {
    if (bg >= MODE1_GBA_BG_COUNT) return;
    const bool corrected = Port_Config_GetColorCorrection();
    if (gbaPixels == sScrollPixels[bg] && (gbaPixels == NULL || corrected == sScrollCorrected[bg])) return;
    C3D_Tex* const tex = &sScrollTex[bg];
    if (tex->data) C3D_TexDelete(tex);
    memset(tex, 0, sizeof(*tex));
    sScrollPixels[bg] = NULL;
    sBoundTexture = UINT8_MAX; /* rebind */
    const unsigned texHeight = PpuGpu3DS_ScrollPictureTexHeight(height);
    const unsigned texWidth = repeat ? width : PPU_GPU3DS_SCROLL_PICTURE_TEX_WIDTH;
    if (gbaPixels == NULL || width > texWidth || height > texHeight) return;
    /* A repeated picture is exactly its texture's width, which the GPU wants a power of two from 8 to 1024. */
    if (repeat && (texWidth < 8u || texWidth > 1024u || (texWidth & (texWidth - 1u)) != 0)) return;
    if (!C3D_TexInit(tex, texWidth, texHeight, GPU_RGBA5551)) {
        memset(tex, 0, sizeof(*tex));
        return;
    }
    memset(tex->data, 0, tex->size);
    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(tex, repeat ? GPU_REPEAT : GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    /* 8x8 Morton tiles, tile rows from the top (v = 1), as the other picture. */
    uint16_t* texels = (uint16_t*)tex->data;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            texels[((y >> 3) * (texWidth >> 3) + (x >> 3)) * 64u +
                   PpuGpu3DS_MortonIndex(x & 7u, y & 7u)] =
                PpuGpu3DS_PackRgba5551(gbaPixels[y * width + x], gbaPixels[y * width + x] != 0);
    C3D_TexFlush(tex);
    sScrollPixels[bg] = gbaPixels;
    sScrollCorrected[bg] = corrected;
}

bool PortPpuGpu3DS_Init(void) {
    if (sReady) return true;

    memset(&sStats, 0, sizeof(sStats));
    sCache = malloc(sizeof(*sCache));
    for (unsigned buffer = 0; buffer < 2u; ++buffer)
        sVertexBuffers[buffer] = linearMemAlign(PPU_GPU3DS_MAX_VERTICES * sizeof(PpuGpu3DSVertex), 0x80);
    sVertices = sVertexBuffers[0];
    /* 512 KiB and 2 MiB of ordinary heap: the copies the build writes the tiles into. */
    sAtlasShadow = calloc(PPU_GPU3DS_ATLAS_PIXELS, sizeof(uint16_t));
    for (unsigned ring = 0; ring < PPU_GPU3DS_RING_COUNT; ++ring)
        sRingShadow[ring] = calloc((size_t)PPU_GPU3DS_RING_TEX_WIDTH * PPU_GPU3DS_RING_TEX_HEIGHT, sizeof(uint16_t));
    sIndices = linearMemAlign(PPU_GPU3DS_MAX_INDICES * sizeof(*sIndices), 0x80);
    sBatches = malloc(PPU_GPU3DS_MAX_BATCHES * sizeof(*sBatches));
    sParityReference = malloc(PPU_GPU3DS_OUTPUT_WIDTH * PPU_GPU3DS_OUTPUT_HEIGHT *
                              sizeof(*sParityReference));
    sParityReadback = linearMemAlign(PPU_GPU3DS_OUTPUT_WIDTH * PPU_GPU3DS_OUTPUT_HEIGHT *
                                         sizeof(*sParityReadback),
                                     0x80);
    if (!sCache || !sVertexBuffers[0] || !sIndices || !sBatches || !sParityReference || !sParityReadback)
        goto fail;
    sCanBuildAhead = sVertexBuffers[1] && sAtlasShadow;
    for (unsigned ring = 0; ring < PPU_GPU3DS_RING_COUNT; ++ring)
        if (!sRingShadow[ring]) sCanBuildAhead = false;
    if (!sCanBuildAhead) {
        if (sVertexBuffers[1]) linearFree(sVertexBuffers[1]);
        sVertexBuffers[1] = sVertexBuffers[0];
        free(sAtlasShadow);
        sAtlasShadow = NULL;
        for (unsigned ring = 0; ring < PPU_GPU3DS_RING_COUNT; ++ring) {
            free(sRingShadow[ring]);
            sRingShadow[ring] = NULL;
        }
        Platform3DS_Debug("[tmc3ds] gpu: no memory to build ahead of the GPU\n");
    }
    memset(sParityReference, 0,
           PPU_GPU3DS_OUTPUT_WIDTH * PPU_GPU3DS_OUTPUT_HEIGHT *
               sizeof(*sParityReference));
    memset(sParityReadback, 0,
           PPU_GPU3DS_OUTPUT_WIDTH * PPU_GPU3DS_OUTPUT_HEIGHT *
               sizeof(*sParityReadback));
    Platform3DS_CleanDataCache(
        sParityReadback, PPU_GPU3DS_OUTPUT_WIDTH * PPU_GPU3DS_OUTPUT_HEIGHT *
                             sizeof(*sParityReadback));

    PpuGpu3DS_CacheInit(sCache);
    /* Indices never change, so fill them once and flush them once. */
    PpuGpu3DS_FillStaticIndices(sIndices, PPU_GPU3DS_MAX_INDICES);
    Platform3DS_CleanDataCache(sIndices, PPU_GPU3DS_MAX_INDICES * sizeof(*sIndices));

    /* The builder stops short of the reset quad at the end of both buffers. */
    for (unsigned buffer = 0; buffer < 2u; ++buffer) {
        PpuGpu3DSVertex* quad = sVertexBuffers[buffer] + PPU_GPU3DS_CLEAR_FIRST_VERTEX;
        /* The static index pattern is 0,1,2, 0,2,3 (PpuGpu3DS_FillStaticIndices), so
         * the corners go round the perimeter: top-left, top-right, bottom-right,
         * bottom-left. They used to be in z order (TL, TR, BL, BR), which covers
         * only three quarters of the screen: the right triangle (top-right,
         * bottom-right, centre) was never reset. */
        static const float kX[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
        static const float kY[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
        for (unsigned corner = 0; corner < 4u; ++corner) {
            quad[corner].x = kX[corner];
            quad[corner].y = kY[corner];
            quad[corner].z = 0.0f;
            quad[corner].u = 0;
            quad[corner].v = 0;
        }
        Platform3DS_CleanDataCache(quad, 4u * sizeof(*quad));
    }
    PpuGpu3DS_CommandInit(&sCommands, sVertices, PPU_GPU3DS_BUILD_VERTICES, sIndices,
                          PPU_GPU3DS_BUILD_INDICES, sBatches, PPU_GPU3DS_MAX_BATCHES);

    if (!C3D_TexInit(&sAtlas, PPU_GPU3DS_ATLAS_SIDE, PPU_GPU3DS_ATLAS_SIDE,
                     GPU_RGBA5551))
        goto fail;
    memset(sAtlas.data, 0, sAtlas.size);
    C3D_TexFlush(&sAtlas);
    C3D_TexSetFilter(&sAtlas, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sAtlas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    /* A layer without a ring just keeps drawing a quad per tile. */
    for (unsigned ring = 0; ring < PPU_GPU3DS_RING_COUNT; ++ring) {
        if (!C3D_TexInit(&sRingTex[ring], PPU_GPU3DS_RING_TEX_WIDTH, PPU_GPU3DS_RING_TEX_HEIGHT,
                         GPU_RGBA5551)) {
            memset(&sRingTex[ring], 0, sizeof(sRingTex[ring]));
            continue;
        }
        memset(sRingTex[ring].data, 0, sRingTex[ring].size);
        C3D_TexFlush(&sRingTex[ring]);
        C3D_TexSetFilter(&sRingTex[ring], GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&sRingTex[ring], GPU_REPEAT, GPU_REPEAT);
        PpuGpu3DS_RingSetStorage(sCache, ring, sCanBuildAhead ? sRingShadow[ring] : (uint16_t*)sRingTex[ring].data);
    }

    if (!C3D_TexInitVRAM(&sOutputTexture, PPU_GPU3DS_OUTPUT_WIDTH,
                         PPU_GPU3DS_OUTPUT_HEIGHT, GPU_RGBA5551))
        goto fail;
    C3D_TexSetFilter(&sOutputTexture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sOutputTexture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sOutputTarget = C3D_RenderTargetCreateFromTex(&sOutputTexture, GPU_TEXFACE_2D, 0,
                                                  GPU_RB_DEPTH24_STENCIL8);
    if (!sOutputTarget) goto fail;
    /* Windows and blending are built entirely on the stencil, so without one
     * every gated batch resolves to whatever the bits happen to hold: the
     * screen comes out uniformly clear-coloured or uniformly blended rather
     * than merely imperfect. citro3d hands back a target regardless and only
     * clears depthBuf/depthMask, so check them and leave the frame to the
     * software rasterizer rather than draw something meaningless. */
    {
        const C3D_FrameBuf* fb = &sOutputTarget->frameBuf;
        char line[160];
        snprintf(line, sizeof(line),
                 "[tmc3ds] gpu depth buffer %p fmt=%d mask=%d vram free %zu\n",
                 fb->depthBuf, (int)fb->depthFmt, (int)fb->depthMask,
                 (size_t)vramSpaceFree());
        Platform3DS_Debug(line);
        if (!fb->depthBuf || fb->depthFmt != GPU_RB_DEPTH24_STENCIL8 ||
            (fb->depthMask & 0x1) == 0) {
            Platform3DS_Debug("[tmc3ds] no stencil attachment; PICA200 disabled\n");
            goto fail;
        }
    }

    sShader = DVLB_ParseFile((u32*)ppu_gpu_3ds_shader_shbin,
                             ppu_gpu_3ds_shader_shbin_size);
    if (!sShader || sShader->numDVLE == 0) goto fail;
    if (R_FAILED(shaderProgramInit(&sProgram))) goto fail;
    sProgramInitialized = true;
    if (R_FAILED(shaderProgramSetVsh(&sProgram, &sShader->DVLE[0]))) goto fail;
    sOffsetUniform = shaderInstanceGetUniformLocation(sProgram.vertexShader,
                                                      "uOffset");
    if (sOffsetUniform < 0) goto fail;

    AttrInfo_Init(&sAttributes);
    if (AttrInfo_AddLoader(&sAttributes, 0, GPU_FLOAT, 3) < 0 ||
        AttrInfo_AddLoader(&sAttributes, 1, GPU_SHORT, 2) < 0)
        goto fail;
    for (unsigned buffer = 0; buffer < 2u; ++buffer) {
        BufInfo_Init(&sBufferInfo[buffer]);
        if (BufInfo_Add(&sBufferInfo[buffer], sVertexBuffers[buffer], sizeof(PpuGpu3DSVertex), 2, 0x10) < 0)
            goto fail;
    }
    sBuildBuffer = 0;
    sDrawnBuffer = 1;
    memset(sSliceGeneration, 0, sizeof(sSliceGeneration));
    memset(sBufferSliceGeneration, 0, sizeof(sBufferSliceGeneration));

    sDisabled = false;
    sPrepared = false;
    sReady = true;
    sStats.initialized = true;
    sStats.enabled = true;
    return true;

fail:
    PortPpuGpu3DS_Shutdown();
    return false;
}

static u32 ClearColor(uint16_t gbaColor) {
    /* The output target is GPU_RGBA5551, so C3D_FrameBufClear fills 16 bits at
     * a time and only the low half of this value ever reaches memory. Returning
     * RGBA8888 meant a black backdrop cleared to 0x00ff -- red 0, green 0,
     * blue 31, alpha 1, i.e. saturated blue. That is the blue behind the intro
     * cutscene text, and it is why the parity surface read gpu=00ff where the
     * software renderer had 0001. Platform-identical, so it is wrong under an
     * emulator too. */
    const u32 packed = PpuGpu3DS_PackRgba5551(gbaColor, true);
    return (packed << 16u) | packed;
}

static unsigned sBuildBackoff, sBuildBackoffLength;

static bool FinishPreflight(bool ready, uint64_t startTick) {
    sStats.preflightTicks += svcGetSystemTick() - startTick;
    if (sCache) {
        sStats.tileHits = sCache->hits;
        sStats.tileDecodes = sCache->decodes;
    }
    if (!ready) ++sStats.fallbackFrames;
    return ready;
}

bool PortPpuGpu3DS_BuildsAhead(void) {
    return sCanBuildAhead && gPpuGpu3DSSerialBuild == 0;
}

/* The map slices the frame draws from its buffer without rebuilding them must hold what the
 * model kept: a slice rebuilt last frame went into the other buffer, so it is copied over (a
 * read of memory the GPU is drawing from, which is harmless). Returns the first vertex
 * written, for the flush. */
static size_t KeepSlicesCurrent(bool built) {
    const unsigned buffer = sBuildBuffer, other = 1u - sBuildBuffer;
    size_t firstWritten = SIZE_MAX;
    if (sVertexBuffers[buffer] == sVertexBuffers[other]) return firstWritten; /* one buffer */
    for (unsigned bg = 0; bg < 4u; ++bg) {
        const size_t first = (size_t)bg * PPU_GPU3DS_MAP_SLICE_VERTICES;
        if ((sCommands.mapDirtyMask & (1u << bg)) != 0) {
            /* Rebuilt into this buffer (also by a build that failed: the model keeps it). */
            sBufferSliceGeneration[buffer][bg] = ++sSliceGeneration[bg];
            continue;
        }
        if (!built || sSliceGeneration[bg] == 0 || sBufferSliceGeneration[buffer][bg] == sSliceGeneration[bg] ||
            sBufferSliceGeneration[other][bg] != sSliceGeneration[bg] || sCommands.dynamicFirstVertex <= first)
            continue;
        memcpy(sVertexBuffers[buffer] + first, sVertexBuffers[other] + first,
               PPU_GPU3DS_MAP_SLICE_VERTICES * sizeof(PpuGpu3DSVertex));
        sBufferSliceGeneration[buffer][bg] = sSliceGeneration[bg];
        if (first < firstWritten) firstWritten = first;
        ++sStats.sliceCopies;
    }
    return firstWritten;
}

static bool FailPreflight(uint64_t startTick) {
    sCommands.vertexCount = 0;
    sCommands.indexCount = 0;
    sCommands.batchCount = 0;
    return FinishPreflight(false, startTick);
}

bool PortPpuGpu3DS_Preflight(const PpuGpu3DSFrameView* frame) {
    const uint64_t startTick = svcGetSystemTick();
    ++sStats.attemptedFrames;
    sPrepared = false;
    sCommitPending = false;
    if (!sReady || sDisabled || !frame || !frame->memory.bg_palette ||
        !frame->memory.obj_palette)
        return FinishPreflight(false, startTick);

    /* Never the buffer the last drawn frame is in: the GPU may still be reading it. A frame
     * built but not drawn leaves it free, and the next one is built there again. */
    sBuildBuffer = 1u - sDrawnBuffer;
    sVertices = sVertexBuffers[sBuildBuffer];
    sCommands.vertices = sVertices;
    /* A frame the builder could not express costs ~25 ms before it gives up, and the next frame of
     * the same scene fails the same way: after a failure the GPU is not tried again for a growing
     * number of frames (8, 16, ... 60), so a scene it cannot draw runs at the software renderer's
     * speed with one wasted build a second instead of one every frame. */
    if (sBuildBackoff > 0) {
        --sBuildBackoff;
        ++sStats.backoffFrames;
        return FinishPreflight(false, startTick);
    }
    sCommands.vertexCount = 0;
    sCommands.indexCount = 0;
    sCommands.batchCount = 0;
    const uint64_t buildStartTick = svcGetSystemTick();
    Platform3DS_SetStage(22);
    PpuGpu3DS_CacheBeginFrame(sCache, frame->memory.bg_palette,
                              frame->memory.obj_palette, ++sFrame);
    if (!PpuGpu3DS_BuildCommands(frame, sCache, sCanBuildAhead ? sAtlasShadow : (uint16_t*)sAtlas.data,
                                 &sCommands)) {
        KeepSlicesCurrent(false);
        sStats.buildTicks += svcGetSystemTick() - buildStartTick;
        if (sCommands.failReason < PPU_GPU3DS_BUILD_REASON_COUNT)
            ++sStats.buildFailures[sCommands.failReason];
        if (sCommands.bandCount > sStats.maxBands)
            sStats.maxBands = sCommands.bandCount;
        if (sCommands.requiredVertices > sStats.maxRequiredVertices)
            sStats.maxRequiredVertices = sCommands.requiredVertices;
        sBuildBackoffLength = sBuildBackoffLength ? (sBuildBackoffLength < 30u ? sBuildBackoffLength * 2u : 60u) : 8u;
        sBuildBackoff = sBuildBackoffLength;
        return FinishPreflight(false, startTick);
    }
    const size_t copiedFirstVertex = KeepSlicesCurrent(true);
    sBuildBackoffLength = 0;
    sStats.lastBuildTicks = svcGetSystemTick() - buildStartTick;
    sStats.buildTicks += sStats.lastBuildTicks;
    sStats.lastBands = sCommands.bandCount;
    sStats.lastVertices = (uint32_t)sCommands.vertexCount;
    sStats.lastMapLayerMask = sCommands.mapLayerMask;
    for (unsigned reason = 0; reason < PPU_GPU3DS_MAP_REJECT_COUNT; ++reason)
        sStats.mapRejects[reason] += sCommands.mapReject[reason];
    for (unsigned reason = 0; reason < PPU_GPU3DS_MAP_REBUILD_COUNT; ++reason)
        sStats.mapRebuildReason[reason] += sCommands.mapRebuild[reason];
    sStats.mapRefreshes += sCommands.mapRefresh;
    for (unsigned bg = 0; bg < 4u; ++bg) {
        if ((sCommands.mapLayerMask & (1u << bg)) != 0) ++sStats.mapLayers;
    }
    /* mapDirtyMask is the subset that had to re-emit quads. Against mapLayers
     * this gives the reuse hit rate, which is what decides whether the
     * per-frame signature walk is earning its cost. */
    for (unsigned bg = 0; bg < 4u; ++bg) {
        if ((sCommands.mapDirtyMask & (1u << bg)) != 0) ++sStats.mapRebuilds;
    }
    if (sCommands.mapLargestQuads > sStats.mapLargestQuads)
        sStats.mapLargestQuads = sCommands.mapLargestQuads;
    if (sCommands.bandCount > sStats.maxBands)
        sStats.maxBands = sCommands.bandCount;
    if (sCommands.vertexCount > sStats.maxVertices)
        sStats.maxVertices = (uint32_t)sCommands.vertexCount;
    if (sCommands.batchCount > sStats.maxBatches)
        sStats.maxBatches = (uint32_t)sCommands.batchCount;

    /* The vertices are this buffer's alone, so they are flushed now. Same economics as the
     * atlas: one call spanning the rebuilt slices and the per-band geometry beats a call per
     * region. A slice that was kept from last frame is only re-flushed when it sits between
     * two that were not, which costs bytes rather than calls. */
    const uint64_t flushStartTick = svcGetSystemTick();
    Platform3DS_SetStage(24);
    const size_t irisFirstVertex = sCommands.vertexCount;
    AppendIrisMask(frame);
    size_t geometryFirstVertex = sCommands.dynamicFirstVertex;
    if (sIrisIndexCount != 0 && irisFirstVertex < geometryFirstVertex) geometryFirstVertex = irisFirstVertex;
    if (copiedFirstVertex < geometryFirstVertex) geometryFirstVertex = copiedFirstVertex;
    for (unsigned bg = 0; bg < 4u; ++bg) {
        if ((sCommands.mapDirtyMask & (1u << bg)) == 0) continue;
        const size_t sliceVertex = (size_t)bg * PPU_GPU3DS_MAP_SLICE_VERTICES;
        if (sliceVertex < geometryFirstVertex) geometryFirstVertex = sliceVertex;
    }
    if (sCommands.vertexCount > geometryFirstVertex &&
        !Platform3DS_CleanDataCache(sVertices + geometryFirstVertex,
                                    (sCommands.vertexCount - geometryFirstVertex) * sizeof(*sVertices)))
        return FailPreflight(startTick);
    sStats.flushTicks += svcGetSystemTick() - flushStartTick;
    sCommitPending = true;
    return FinishPreflight(true, startTick);
}

/* The atlas slots decoded since the last commit, copied from the build's copy to the texture
 * and flushed. Only the slots decoded this frame need uploading, but each
 * GSPGPU_FlushDataCache is a GSP round trip measured at ~330 us on an Old
 * 3DS -- a hundred times the cost of the few kilobytes a call covers. So
 * the dirty slots are coalesced into a handful of ranges and the extra
 * bytes are paid gladly to avoid the calls. */
static bool CommitAtlas(void) {
    if (sCache->dirtyCount == 0) return true;
    unsigned firstSlot = PPU_GPU3DS_SLOT_COUNT, lastSlot = 0;
    uint16_t* const texels = (uint16_t*)sAtlas.data;
    memset(sDirtyBitmap, 0, sizeof(sDirtyBitmap));
    for (unsigned index = 0; index < sCache->dirtyCount; ++index) {
        const unsigned slot = sCache->dirtySlots[index];
        if (sAtlasShadow)
            memcpy(texels + (size_t)slot * 64u, sAtlasShadow + (size_t)slot * 64u, 64u * sizeof(uint16_t));
        sDirtyBitmap[slot / 32u] |= 1u << (slot % 32u);
        if (slot < firstSlot) firstSlot = slot;
        if (slot > lastSlot) lastSlot = slot;
    }

    /* Walk the bitmap into runs, merging any gap smaller than the bytes a
     * separate call would cost. */
    unsigned rangeFirst[PPU_GPU3DS_ATLAS_FLUSH_RANGES];
    unsigned rangeLast[PPU_GPU3DS_ATLAS_FLUSH_RANGES];
    unsigned rangeCount = 0;
    for (unsigned slot = firstSlot; slot <= lastSlot; ++slot) {
        if ((sDirtyBitmap[slot / 32u] & (1u << (slot % 32u))) == 0) continue;
        if (rangeCount != 0 &&
            slot - rangeLast[rangeCount - 1] <= PPU_GPU3DS_ATLAS_FLUSH_GAP) {
            rangeLast[rangeCount - 1] = slot;
            continue;
        }
        if (rangeCount == PPU_GPU3DS_ATLAS_FLUSH_RANGES) {
            /* Out of ranges: one span covers everything left. */
            rangeLast[rangeCount - 1] = lastSlot;
            break;
        }
        rangeFirst[rangeCount] = slot;
        rangeLast[rangeCount] = slot;
        ++rangeCount;
    }

    bool flushed = true;
    for (unsigned range = 0; range < rangeCount && flushed; ++range) {
        const size_t bytes =
                ((size_t)rangeLast[range] - rangeFirst[range] + 1u) * 64u *
                sizeof(uint16_t);
        flushed = Platform3DS_CleanDataCache(texels + (size_t)rangeFirst[range] * 64u, bytes);
        if (flushed) sStats.atlasFlushBytes += bytes;
    }
    sStats.atlasFlushCalls += rangeCount;
    /* A failed flush keeps the slots dirty: the next commit copies and flushes them again. */
    if (flushed) PpuGpu3DS_CacheClearDirty(sCache);
    return flushed;
}

/* The ring cells written since the last commit, likewise. */
static bool CommitRings(void) {
    for (unsigned ring = 0; ring < PPU_GPU3DS_RING_COUNT; ++ring) {
        size_t offset, bytes;
        if (!sRingTex[ring].data || !PpuGpu3DS_RingTakeDirty(sCache, ring, &offset, &bytes, sRingCells)) continue;
        uint16_t* const texels = (uint16_t*)sRingTex[ring].data;
        if (sRingShadow[ring]) {
            const unsigned firstCell = (unsigned)(offset / (64u * sizeof(uint16_t)));
            const unsigned lastCell = firstCell + (unsigned)(bytes / (64u * sizeof(uint16_t))) - 1u;
            for (unsigned word = firstCell / 32u; word <= lastCell / 32u; ++word) {
                uint32_t bits = sRingCells[word];
                if (bits == 0xffffffffu) {
                    memcpy(texels + (size_t)word * 32u * 64u, sRingShadow[ring] + (size_t)word * 32u * 64u,
                           32u * 64u * sizeof(uint16_t));
                    continue;
                }
                while (bits != 0) {
                    const unsigned cell = word * 32u + (unsigned)__builtin_ctz(bits);
                    bits &= bits - 1u;
                    memcpy(texels + (size_t)cell * 64u, sRingShadow[ring] + (size_t)cell * 64u, 64u * sizeof(uint16_t));
                }
            }
        }
        if (!Platform3DS_CleanDataCache((uint8_t*)texels + offset, bytes)) return false;
        sStats.atlasFlushBytes += bytes;
        ++sStats.atlasFlushCalls;
    }
    return true;
}

bool PortPpuGpu3DS_Commit(const PpuGpu3DSFrameView* frame) {
    const uint64_t startTick = svcGetSystemTick();
    if (!sCommitPending) return false;
    sCommitPending = false;
    /* Geometry capacity does not bound the hardware command list: every
     * window band repeats state and draw commands, including OBJ prepasses.
     * Reject before emitting any PPU commands. The caller then renders this
     * same frame in software and retries next frame; the atlas and ring copies
     * stay dirty until a commit moves them. */
    const size_t commandWords = PpuGpu3DS_CommandWordsRequired(&sCommands);
    sStats.lastCommandWords = (uint32_t)commandWords;
    if (commandWords > sStats.maxCommandWords)
        sStats.maxCommandWords = (uint32_t)commandWords;
    if (!PpuGpu3DS_CommandBudgetFits(commandWords, gpuCmdBuf != NULL,
                                    gpuCmdBufSize, gpuCmdBufOffset)) {
        ++sStats.commandBudgetFallbacks;
        return FailPreflight(startTick);
    }
    Platform3DS_SetStage(23);
    if (!CommitAtlas() || !CommitRings() || ((frame->pictureMask & 0x8u) && !RefreshIndexedPicture(frame)))
        return FailPreflight(startTick);

    Platform3DS_SetStage(25);
    sStats.flushTicks += svcGetSystemTick() - startTick;
    sPreparedWidth = frame->width;
    sPreparedHeight = frame->height;
    sFadeWeight = frame->fadeWeight;
    sFadeLevel = frame->fadeLevel;
    sPrepared = true;
    return FinishPreflight(true, startTick);
}

/* Set for the frame when any batch writes the object-window stencil bit.
 * Regions OUTSIDE and OBJWIN only mean something relative to that bit, so
 * without a writer the test can only reject work that should have drawn --
 * and both halves of an alpha pair carry it, so a stale bit drops the pixel
 * entirely and leaves the clear colour behind. */
static bool sHasObjWindow;

static unsigned BatchRegion(const PpuGpu3DSBatch* batch) {
    const unsigned region = batch->color >> PPU_GPU3DS_REGION_SHIFT;
    /* Collapse both object-window regions onto "no window" when nothing
     * writes the bit they test. */
    if (!sHasObjWindow && region < 2u) return 2u;
    return region;
}

static u32 Coefficient8(unsigned coefficient) {
    return coefficient == 16u ? 0xffu : coefficient * 255u / 16u;
}

/* Reprogramming the combiners and the blender writes a good many registers
 * into the command list, and consecutive batches usually want exactly the same
 * configuration, so each is applied only when it actually changes. The keys
 * cover every field the two functions read. */
/* Resolved once per frame from the config: the offset that places the frame
 * where the presenter samples it. */
static u32 sViewportOffset;

static uint32_t sTevKey = 0xffffffffu;
static uint32_t sBlendKey = 0xffffffffu;
/* The scissor is reprogrammed per batch, and a frame can carry 391 of them
 * while consecutive batches usually cover the same rectangle -- a band's
 * layers, or an alpha pair drawn twice over identical geometry. Each call
 * writes registers into the command list, so skipping the unchanged ones makes
 * the list shorter, which is GSP work on core 1 as well as CPU here. Same
 * treatment the TEV and blender already get. */
static uint32_t sScissorKeyLow = 0xffffffffu;
static uint32_t sScissorKeyHigh = 0xffffffffu;

static void SetScissorCached(u32 left, u32 bottom, u32 right, u32 top) {
    const uint32_t low = ((uint32_t)left << 16u) | (uint32_t)(bottom & 0xffffu);
    const uint32_t high = ((uint32_t)right << 16u) | (uint32_t)(top & 0xffffu);
    if (low == sScissorKeyLow && high == sScissorKeyHigh) return;
    sScissorKeyLow = low;
    sScissorKeyHigh = high;
    C3D_SetScissor(GPU_SCISSOR_NORMAL, left, bottom, right, top);
}

static int sTintMode;
void PortPpuGpu3DS_SetTintMode(int mode) { sTintMode = mode < 0 || mode > 2 ? 0 : mode; }
int PortPpuGpu3DS_TintMode(void) { return sTintMode; }

/* A zone depth copy (DrawZoneBatch) is tinted as the plane it is drawn at: this layer and priority, or -1. */
static int sTintLayer = -1, sTintPriority;

/* The flat colour (0xBBGGRR, with a bit 24 mark) a batch is painted in under the tint view; 0: not tinted. */
static uint32_t TintOf(const PpuGpu3DSBatch* batch) {
    if (sTintMode == 0 || batch->objWindow || batch->layer > PPU_GPU3DS_OBJ) return 0;
    const unsigned layer = sTintLayer >= 0 ? (unsigned)sTintLayer : batch->layer;
    const unsigned priority = (sTintLayer >= 0 ? (unsigned)sTintPriority : batch->priority) & 3u;
    unsigned r, g, b;
    if (sTintMode == 1) {
        static const unsigned kRgb[5][3] = { { 255, 0, 255 }, { 0, 255, 255 }, { 255, 150, 0 }, { 0, 220, 0 }, { 255, 255, 255 } };
        static const unsigned kScale[4] = { 256, 192, 140, 96 };
        r = kRgb[layer][0] * kScale[priority] / 256u;
        g = kRgb[layer][1] * kScale[priority] / 256u;
        b = kRgb[layer][2] * kScale[priority] / 256u;
    } else {
        /* A sprite with a depth of its own (PpuGpu3DSBatch.depth, quarter pixels) is where that puts it, not on its
         * layer's: tall sprites show their lean, tile row by tile row. */
        float depth = batch->depth != 0 && batch->layer == PPU_GPU3DS_OBJ
                              ? 0.25f * (float)batch->depth
                              : (float)Stereo3DS_LayerDepth(layer, priority);
        depth = depth < -4.0f ? -4.0f : depth > 8.0f ? 8.0f : depth;
        const float t = depth + 4.0f; /* 0..12 */
        r = (unsigned)(t * 5.0f);
        g = (unsigned)(t * 10.0f);
        b = 40u + (unsigned)(t * 17.0f);
    }
    return 0x1000000u | r | (g << 8u) | (b << 16u);
}

static uint32_t TevKey(const PpuGpu3DSBatch* batch) {
    return (uint32_t)batch->effect | ((uint32_t)batch->evy << 8u);
}

static uint32_t BlendKey(const PpuGpu3DSBatch* batch) {
    return (uint32_t)batch->effect | ((uint32_t)batch->eva << 8u) |
           ((uint32_t)batch->evb << 16u) | ((uint32_t)batch->multiply << 24u);
}

static uint32_t sTintKey;

static void ResetTev(const PpuGpu3DSBatch* batch) {
    const uint32_t key = TevKey(batch);
    const uint32_t tint = TintOf(batch);
    if (key == sTevKey && tint == sTintKey) return;
    sTevKey = key;
    sTintKey = tint;
    for (int stage = 0; stage < 3; ++stage)
        C3D_TexEnvInit(C3D_GetTexEnv(stage));
    C3D_TexEnv* texture = C3D_GetTexEnv(0);
    C3D_TexEnvSrc(texture, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0,
                  GPU_TEXTURE0);
    C3D_TexEnvFunc(texture, C3D_Both, GPU_REPLACE);
    if (tint != 0) {
        /* The picture's alpha as it is, its colour one flat tone: brightening, darkening and blending would hide
         * the difference between a dark picture and a far plane. */
        C3D_TexEnvColor(texture, 0xff000000u | (tint & 0xffffffu));
        C3D_TexEnvSrc(texture, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        return;
    }
    if (batch->effect != PPU_GPU3DS_EFFECT_BRIGHTEN &&
        batch->effect != PPU_GPU3DS_EFFECT_DARKEN)
        return;

    C3D_TexEnv* effect = C3D_GetTexEnv(1);
    const u32 constant =
            (batch->effect == PPU_GPU3DS_EFFECT_BRIGHTEN ? 0x00ffffffu : 0) |
            (Coefficient8(batch->evy) << 24u);
    C3D_TexEnvColor(effect, constant);
    C3D_TexEnvSrc(effect, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS,
                  GPU_CONSTANT);
    C3D_TexEnvOpRgb(effect, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(effect, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(effect, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS,
                  GPU_PREVIOUS);
    C3D_TexEnvFunc(effect, C3D_Alpha, GPU_REPLACE);
}

static void ResetBlend(const PpuGpu3DSBatch* batch) {
    const uint32_t key = BlendKey(batch) | (TintOf(batch) != 0 ? 0x80000000u : 0u);
    if (key == sBlendKey) return;
    sBlendKey = key;
    if (batch->effect == PPU_GPU3DS_EFFECT_ALPHA && batch->multiply && TintOf(batch) == 0) {
        /* Multiply: src * dst + dst * 0.25, the pixel under it times (its colour + 0.25); shadows' colours are dark,
         * so it always darkens, the lighter bands less. 0.25 keeps it near the GBA's blend over a light ground (room
         * 03's yellow) and clearly darker over the dark mountain, where the blend lightened (0.5 was too faint). */
        C3D_BlendingColor(0x40404040u);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_CONSTANT_COLOR, GPU_ONE, GPU_ZERO);
    } else if (batch->effect == PPU_GPU3DS_EFFECT_ALPHA && TintOf(batch) == 0) {
        const u32 eva = Coefficient8(batch->eva);
        const u32 evb = Coefficient8(batch->evb);
        C3D_BlendingColor(eva | (eva << 8u) | (eva << 16u) | (evb << 24u));
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR,
                       GPU_CONSTANT_ALPHA, GPU_ONE, GPU_ZERO);
    } else {
        C3D_BlendingColor(0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO,
                       GPU_ONE, GPU_ZERO);
    }
}

static void SetWindowStencil(const PpuGpu3DSBatch* batch) {
    if (!Port_Config_GpuStencil()) {
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
        return;
    }
    const unsigned region = BatchRegion(batch);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
    if (region == 0u)
        C3D_StencilTest(true, GPU_EQUAL, 0, 0x04, 0);
    else if (region == 1u)
        C3D_StencilTest(true, GPU_EQUAL, 0x04, 0x04, 0);
    else
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0);
}

/* Stencil bit 0x08 marks a pixel whose top layer so far is a second blend target. With the eyes
 * drawn over one shared picture (DrawEyesOverBase) the base keeps the mark in 0x10 as well, and
 * each eye's layers in front read and write only their own copy: the eyes share one depth-stencil
 * buffer, so what the first eye wrote would otherwise reach the second. */
static unsigned sTarget2Bit = 0x08u, sTarget2WriteMask = 0x08u;

/* Every path that relies on the stencil keeps the depth test enabled with
 * GPU_ALWAYS rather than disabling it. The two are equivalent as tests, but on
 * PICA200 the stencil belongs to the depth-stencil stage, and a console draws
 * nothing for the stencil-gated batches while an emulator draws them -- which
 * is exactly the band that came back as the clear colour. */
static void SetColorStencil(const PpuGpu3DSBatch* batch) {
    if (!Port_Config_GpuStencil()) {
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
        return;
    }
    const unsigned region = BatchRegion(batch);
    const bool alphaBranch =
            batch->effect == PPU_GPU3DS_EFFECT_ALPHA ||
            (batch->color & PPU_GPU3DS_ALPHA_COMPLEMENT) != 0;
    const bool oldTarget =
            batch->effect == PPU_GPU3DS_EFFECT_ALPHA;
    unsigned mask = region < 2u ? 0x04u : 0;
    unsigned ref = region == 1u ? 0x04u : 0;
    if (alphaBranch) {
        mask |= sTarget2Bit;
        if (oldTarget) ref |= sTarget2Bit;
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP,
                      oldTarget == (batch->target2 != 0)
                              ? GPU_STENCIL_KEEP
                              : GPU_STENCIL_INVERT);
    } else {
        if (batch->target2) ref |= sTarget2WriteMask;
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP,
                      GPU_STENCIL_REPLACE);
    }
    C3D_StencilTest(true, mask ? GPU_EQUAL : GPU_ALWAYS, ref, mask, sTarget2WriteMask);
}

/* The vertex shader adds this to every position; only map-space background
 * batches use a non-zero value. */
static void SetBatchOffset(float x, float y) {
    if (sOffsetUniform < 0 || (x == sLastOffsetX && y == sLastOffsetY)) return;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sOffsetUniform, x, y, 0.0f, 0.0f);
    sLastOffsetX = x;
    sLastOffsetY = y;
}

/* Counted so the log can show the stall condition was actually present and is
 * now being skipped, rather than leaving that as an inference. */
static unsigned long long sEmptyDrawsSkipped;

unsigned long long PortPpuGpu3DS_EmptyDrawsSkipped(void) {
    return sEmptyDrawsSkipped;
}

static void DrawBatch(const PpuGpu3DSBatch* batch) {
    /* A zero-count draw never signals completion on PICA200: the GX queue's
     * interrupt count never reaches the number of entries queued, and the next
     * C3D_FrameBegin waits on it forever. Frames that froze the console carried
     * 4 to 16 of these; frames that rendered carried none. */
    if (batch->indexCount == 0) {
        ++sEmptyDrawsSkipped;
        return;
    }
    if (batch->texture != sBoundTexture) {
        sBoundTexture = batch->texture;
        C3D_TexBind(0, batch->texture == 0                                   ? &sAtlas
                       : batch->texture == PPU_GPU3DS_PICTURE_TEXTURE        ? &sPictureTex
                       : batch->texture >= PPU_GPU3DS_SCROLL_PICTURE_TEXTURE
                           ? &sScrollTex[(batch->texture - PPU_GPU3DS_SCROLL_PICTURE_TEXTURE) & 3u]
                           : &sRingTex[batch->texture - 1u]);
    }
    SetBatchOffset(batch->offsetX + (sEyeShifting && !sNoShift && batch->layer <= PPU_GPU3DS_OBJ
                                             ? (batch->objWindow && sEyeWindowFollows ? sEyeWindowNdc
                                                : batch->depth != 0 ? (float)SpritePx(batch) * sEyeToNdc
                                                        : sEyeShiftNdc[batch->layer][batch->priority & 3u])
                                             : 0.0f),
                   batch->offsetY);
    const u32 yOffset = sViewportOffset;
    u32 top, bottom;
    switch (Port_Config_GpuScissorMode()) {
        case 0:
            /* Horizontal clipping only; a band's own geometry already covers
             * the right scanlines apart from tile-row overhang. */
            top = 0;
            bottom = PPU_GPU3DS_OUTPUT_HEIGHT;
            break;
        case 2:
            top = yOffset + batch->firstLine;
            bottom = yOffset + batch->firstLine + batch->lineCount;
            break;
        default: {
            /* Unsigned: a band overhanging the bottom of the frame wrapped this
             * to ~4e9, and citro3d packs the scissor as top | (bottom << 16),
             * so the box became garbage silently. */
            const unsigned end = (unsigned)batch->firstLine + batch->lineCount;
            const unsigned clampedEnd =
                    end > sPreparedHeight ? sPreparedHeight : end;
            top = yOffset + sPreparedHeight - clampedEnd;
            bottom = yOffset + sPreparedHeight - batch->firstLine;
            break;
        }
    }
    u32 left = batch->scissorLeft, right = batch->scissorRight;
    if (sEyeShifting && !sNoShift && batch->layer <= PPU_GPU3DS_OBJ) {
        const int shift = batch->objWindow && sEyeWindowFollows ? sEyeWindowPx
                          : batch->depth != 0                   ? SpritePx(batch)
                                                                 : sEyeShiftPx[batch->layer][batch->priority & 3u];
        const int movedLeft = (int)left + shift, movedRight = (int)right + shift;
        left = movedLeft < 0 ? 0u : (u32)movedLeft;
        right = movedRight > (int)sPreparedWidth ? sPreparedWidth : (movedRight < 0 ? 0u : (u32)movedRight);
    }
    SetScissorCached(left, top, right, bottom);
    C3D_DrawElements(GPU_TRIANGLES, (int)batch->indexCount,
                     C3D_UNSIGNED_SHORT, sIndices + batch->firstIndex);
}

static bool FinishDraw(bool rendered, uint64_t startTick) {
    sStats.drawTicks += svcGetSystemTick() - startTick;
    if (rendered) {
        ++sStats.renderedFrames;
        sStats.vertices += sCommands.vertexCount;
        sStats.batches += sCommands.batchCount;
    }
    return rendered;
}

/* One-shot dump of the batch list for the first multi-band frame. The fault
 * being chased is a whole band not appearing, so what matters is whether its
 * batches exist and with what state -- which no host replay can answer, since
 * the host command stream is already known to be correct. */
static bool sBatchesLogged;

static void LogBatchList(void) {
    /* Log the frame parity is about to compare -- that is the frame that comes
     * out wrong, and the title screen that logs first is not it. */
    if (sBatchesLogged || !sParityReferenceReady) return;
    sBatchesLogged = true;
    char line[224];
    snprintf(line, sizeof(line), "[tmc3ds] batches=%u viewport=%lu\n",
             (unsigned)sCommands.batchCount, (unsigned long)sViewportOffset);
    Platform3DS_Debug(line);
    for (size_t i = 0; i < sCommands.batchCount && i < 20u; ++i) {
        const PpuGpu3DSBatch* b = &sCommands.batches[i];
        snprintf(line, sizeof(line),
                 "[tmc3ds]  b%u L%u p%u line=%u+%u idx=%u+%u sc=%u..%u eff=%u "
                 "t2=%u win=%02x col=%04x\n",
                 (unsigned)i, b->layer, b->priority, b->firstLine, b->lineCount,
                 b->firstIndex, b->indexCount, b->scissorLeft, b->scissorRight,
                 b->effect, b->target2, b->windowControl, b->color);
        Platform3DS_Debug(line);
    }
}

static void ClearStencilPlane(void) {
    SetBatchOffset(0.0f, 0.0f);
    const u32 yOffset = sViewportOffset;
    SetScissorCached(0, yOffset, sPreparedWidth, yOffset + sPreparedHeight);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    /* Stencil and depth, no colour: the quad is at z = 0, the depth the target clear writes. The
     * right eye shares the left's depth-stencil (EnsureRightOutput), and both target clears are
     * queued before either eye's commands run, so the second eye would start from the first
     * eye's depth: sprites cut where the other eye's were. */
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH);
    C3D_StencilTest(true, GPU_ALWAYS, 0, 0xff, 0xff);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT,
                     sIndices + PPU_GPU3DS_CLEAR_FIRST_INDEX);
}

/* What a DrawOutput call draws. ALL: the whole frame. BASE: only the layers on the screen plane,
 * without the iris and fade; FRONT_LEFT / FRONT_RIGHT: over it, only the layers with a depth, then
 * the iris and fade (DrawEyesOverBase). */
typedef enum OutputPass { PASS_ALL, PASS_BASE, PASS_FRONT_LEFT, PASS_FRONT_RIGHT } OutputPass;

/* The layers off the screen plane this frame: their shift is not zero (SetEyeShifts). */
static bool sFrontLayer[PPU_GPU3DS_OBJ + 1][4];
/* Whether a sprite is among them: only then does each eye reset the depth (ClearDepthPlane). */
static bool sFrontHasObj;

/* Sprites that stay put but are drawn with the layers in front, because the game draws them over a sprite or layer in
 * front (EyesOverBaseRefusal): a front pass draws in the batches' order, so they land over it as on the GBA. */
static bool sPromoted[PPU_GPU3DS_MAX_BATCHES];

static bool IsFrontBatch(const PpuGpu3DSBatch* batch) {
    if (sPromoted[batch - sCommands.batches]) return true;
    /* An OBJ window that follows a layer (Stereo3DS_ObjWindowFollows) moves with that layer, not with the sprites'. */
    if (batch->objWindow && sEyeWindowFollows) return sEyeWindowPx != 0;
    /* Its eye's shift is the other's negated: in front for both or for neither. */
    /* Decided from the left eye's move, which is nonzero whenever the disparity is (the right eye's can be 0 for
     * a 1 px disparity), whichever eye is being drawn. */
    if (batch->front) return true;
    if (batch->depth != 0) return Stereo3DS_SpriteShift(batch->depth, STEREO3DS_LEFT) != 0.0f;
    return batch->layer <= PPU_GPU3DS_OBJ && sFrontLayer[batch->layer][batch->priority & 3u];
}

/* A batch forced to the layers in front (the big Chuchu's pieces) is not depth tested: the colour pass of a sprite draws
 * only where the depth the pre-pass wrote is equal, and with many small batches of one sprite some of those draws were
 * lost (two rows of tiles of its legs, in one eye, about one frame in twelve). Its batches are already in the order
 * the game draws them, back to front. */
static bool IsPainterBatch(const PpuGpu3DSBatch* batch) {
    return batch->front && batch->layer == PPU_GPU3DS_OBJ && !batch->semiTransparent;
}

static bool PassDraws(OutputPass pass, const PpuGpu3DSBatch* batch) {
    return pass == PASS_ALL || (pass == PASS_BASE) != IsFrontBatch(batch);
}

/* The right eye starts as a copy of the left's base picture: one quad sampling the left output, colour only, so
 * the shared depth-stencil keeps the base's marks. Commands run in order, so it reads the base before the left
 * eye's own layers are drawn over it. */
static void CopyLeftOutput(void) {
    PpuGpu3DSVertex* quad = sVertices + PPU_GPU3DS_COPY_FIRST_VERTEX;
    /* Same corners as the reset quad (top-left, top-right, bottom-right, bottom-left), each sampling the output
     * texel under it: the viewport's bottom edge is target row sViewportOffset. */
    const float u1 = (float)sPreparedWidth / (float)PPU_GPU3DS_OUTPUT_WIDTH;
    const float v0 = (float)sViewportOffset / (float)PPU_GPU3DS_OUTPUT_HEIGHT;
    const float v1 = (float)(sViewportOffset + sPreparedHeight) / (float)PPU_GPU3DS_OUTPUT_HEIGHT;
    static const float kX[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    static const float kY[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
    for (unsigned corner = 0; corner < 4u; ++corner) {
        quad[corner].x = kX[corner];
        quad[corner].y = kY[corner];
        quad[corner].z = 0.0f;
        quad[corner].u = PpuGpu3DS_PackUV(kX[corner] < 0.0f ? 0.0f : u1);
        quad[corner].v = PpuGpu3DS_PackUV(kY[corner] < 0.0f ? v0 : v1);
    }
    Platform3DS_CleanDataCache(quad, 4u * sizeof(*quad));
    for (int stage = 0; stage < 3; ++stage)
        C3D_TexEnvInit(C3D_GetTexEnv(stage));
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    sTevKey = 0xffffffffu;
    sTintKey = 0xffffffffu;
    C3D_TexSetFilter(&sOutputTexture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexBind(0, &sOutputTexture);
    sBoundTexture = UINT8_MAX;
    C3D_BlendingColor(0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    sBlendKey = 0xffffffffu;
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0);
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
    SetBatchOffset(0.0f, 0.0f);
    SetScissorCached(0, sViewportOffset, sPreparedWidth, sViewportOffset + sPreparedHeight);
    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, sIndices + PPU_GPU3DS_COPY_FIRST_INDEX);
}

/* Depth back to the clear value, stencil kept: the sprites in front pick their order among themselves again
 * (they are in front of every base sprite whatever the depth holds), so neither eye sees the other's. */
static void ClearDepthPlane(void) {
    SetBatchOffset(0.0f, 0.0f);
    SetScissorCached(0, sViewportOffset, sPreparedWidth, sViewportOffset + sPreparedHeight);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH);
    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, sIndices + PPU_GPU3DS_CLEAR_FIRST_INDEX);
}

/* A background layer that stands out of the screen leaves, where it moves, a strip of whatever is under it: a hole
 * (the arch of a door sits on a layer, the ground under it has none: black) or the outline of the layer below, which
 * the moved layer used to hide. Each eye draws such a layer (BG1-BG3 in front, the world only) twice, the unmoved copy
 * first: the strip then shows the layer's own pixels (its edge, as if it were one pixel thicker), not black. */
static bool sUnderlay;
static bool IsGhostBatch(const PpuGpu3DSBatch* batch) {
    /* Not a blended layer (the Minish Woods' fog, BG3 at priority 0): its picture is translucent and wholly covers the
     * frame, so there is no strip to fill, and drawing the unmoved copy first made its blend pass mark every pixel it
     * covered as done (the stencil mark flips), so the moved copy drew nowhere there: the fog did not move, and cost
     * four draws a band in each eye instead of two. */
    return sUnderlay && !batch->objWindow && batch->depth == 0 && batch->layer >= PPU_GPU3DS_BG1 &&
           batch->layer <= PPU_GPU3DS_BG3 && sFrontLayer[batch->layer][batch->priority & 3u] &&
           batch->effect != PPU_GPU3DS_EFFECT_ALPHA && (batch->color & PPU_GPU3DS_ALPHA_COMPLEMENT) == 0 &&
           !Stereo3DS_LayerSeeThrough(batch->layer, batch->priority & 3u);
}

/* Zone depth fixes (docs/stereo-3d.md, "Zone depth fixes"; zone_depth_3ds.h): the pixels of one layer inside a rectangle
 * drawn once more, moved as another layer's plane is. Each copy is drawn right after the layer's own last batch
 * (`anchor`), in the layer's own place in the drawing order: over the layer's unmoved picture (which fills the strip the
 * moved copy uncovers) and under everything the game draws after the layer (sprites, the layers in front), so a layer in
 * front keeps its opaque pixels and only its transparent ones show the moved copy. It is drawn in the pass that draws its
 * anchor and moved by toBg's shift for the eye; a copy that moves but whose layer is drawn in the base has no shift
 * there, so that frame is drawn whole (SPLIT_ZONE_UNDER_BASE). Resolved once a frame (PrepareZoneDraws); none outside the
 * world scene, in a room with no fix, or with the rectangle off the frame. */
typedef struct ZoneDraw {
    ZoneDepth3DSRect rect;
    size_t anchor, first; /* the last and the first batch of layer `bg` */
    uint8_t bg, toBg, toPriority;
} ZoneDraw;
static ZoneDraw sZoneDraws[ZONE_DEPTH_3DS_MAX_PER_ROOM];
static unsigned sZoneDrawCount;

static int EyeLayerPx(unsigned layer, unsigned priority, Stereo3DSEye eye) {
    return RoundPx(Stereo3DS_LayerShift(layer, priority & 3u, eye));
}

/* More than any eye's shift (the depths are -16..16 px at the full slider, split between the eyes). */
enum { ZONE_MARGIN = 16 };

static void PrepareZoneDraws(void) {
    sZoneDrawCount = 0;
    /* The tint views show the zones' planes with the slider down too (ZoneDepth3DS_BeginFrame has them then). */
    if (!sUnderlay && sTintMode == 0) return;
    const ZoneDepth3DSZone* zones;
    const unsigned count = ZoneDepth3DS_Zones(&zones);
    if (count == 0) return;
    /* The first and last batch of each background layer (the backdrop is batch 0). */
    size_t first[4] = { 0 }, last[4] = { 0 };
    for (size_t i = 1; i < sCommands.batchCount; ++i) {
        const PpuGpu3DSBatch* batch = &sCommands.batches[i];
        if (batch->objWindow || batch->layer > PPU_GPU3DS_BG3) continue;
        if (first[batch->layer] == 0) first[batch->layer] = i;
        last[batch->layer] = i;
    }
    for (unsigned z = 0; z < count; ++z) {
        const unsigned bg = zones[z].bg, to = zones[z].toBg;
        if (last[bg] == 0 || last[to] == 0) continue; /* a layer not drawn this frame: no plane to take */
        const unsigned bgPriority = sCommands.batches[last[bg]].priority & 3u;
        const unsigned toPriority = sCommands.batches[last[to]].priority & 3u;
        /* On the same plane in both eyes: the layer is already where the fix would put it (unless a tint view paints the
         * planes, which differ even when the slider rounds them to one shift). */
        if (EyeLayerPx(bg, bgPriority, STEREO3DS_LEFT) == EyeLayerPx(to, toPriority, STEREO3DS_LEFT) &&
            EyeLayerPx(bg, bgPriority, STEREO3DS_RIGHT) == EyeLayerPx(to, toPriority, STEREO3DS_RIGHT) &&
            (sTintMode == 0 || Stereo3DS_LayerDepth(bg, bgPriority) == Stereo3DS_LayerDepth(to, toPriority)))
            continue;
        /* Off the frame, even moved: nothing to draw (and no reason to give up the base picture). */
        const ZoneDepth3DSRect* rect = &zones[z].rect;
        if (rect->right <= -ZONE_MARGIN || rect->left >= (int)sPreparedWidth + ZONE_MARGIN || rect->bottom <= 0 ||
            rect->top >= (int)sPreparedHeight)
            continue;
        ZoneDraw* draw = &sZoneDraws[sZoneDrawCount++];
        draw->rect = *rect;
        draw->anchor = last[bg];
        draw->first = first[bg];
        draw->bg = (uint8_t)bg;
        draw->toBg = (uint8_t)to;
        draw->toPriority = (uint8_t)toPriority;
    }
}

/* One batch of the zone's layer, clipped to the zone and moved by `shift` pixels (ZoneDepth3DS_BatchScissor), tinted
 * (PLANE TINT) as toBg at `toPriority`. */
static void DrawZoneBatch(const PpuGpu3DSBatch* batch, const ZoneDepth3DSRect* zone, int shift, unsigned toBg,
                          unsigned toPriority) {
    if (batch->indexCount == 0) return;
    ZoneDepth3DSRect cut;
    if (!ZoneDepth3DS_BatchScissor(zone, batch->scissorLeft, batch->scissorRight, batch->firstLine,
                                   (int)batch->firstLine + batch->lineCount, shift, (int)sPreparedWidth,
                                   (int)sPreparedHeight, &cut))
        return;
    if (batch->texture != sBoundTexture) {
        sBoundTexture = batch->texture;
        C3D_TexBind(0, batch->texture == 0                                   ? &sAtlas
                       : batch->texture == PPU_GPU3DS_PICTURE_TEXTURE        ? &sPictureTex
                       : batch->texture >= PPU_GPU3DS_SCROLL_PICTURE_TEXTURE
                           ? &sScrollTex[(batch->texture - PPU_GPU3DS_SCROLL_PICTURE_TEXTURE) & 3u]
                           : &sRingTex[batch->texture - 1u]);
    }
    SetBatchOffset(batch->offsetX + (float)shift * (2.0f / (float)sPreparedWidth), batch->offsetY);
    sTintLayer = (int)toBg;
    sTintPriority = (int)toPriority;
    ResetTev(batch);
    sTintLayer = -1;
    ResetBlend(batch);
    C3D_AlphaTest(true, GPU_GREATER, 0);
    SetColorStencil(batch);
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
    /* Lines in the target as DrawBatch has them: flipped, except with gpu_scissor_mode 2. */
    const u32 yOffset = sViewportOffset;
    const bool flipped = Port_Config_GpuScissorMode() != 2;
    const u32 top = flipped ? yOffset + sPreparedHeight - (u32)cut.bottom : yOffset + (u32)cut.top;
    const u32 bottom = flipped ? yOffset + sPreparedHeight - (u32)cut.top : yOffset + (u32)cut.bottom;
    SetScissorCached((u32)cut.left, top, (u32)cut.right, bottom);
    C3D_DrawElements(GPU_TRIANGLES, (int)batch->indexCount, C3D_UNSIGNED_SHORT, sIndices + batch->firstIndex);
}

/* The zones anchored at batch `index`, just drawn. */
static void DrawZonesAfter(size_t index) {
    for (unsigned z = 0; z < sZoneDrawCount; ++z) {
        const ZoneDraw* draw = &sZoneDraws[z];
        if (draw->anchor != index) continue;
        const int shift = sEyeShifting ? sEyeShiftPx[draw->toBg][draw->toPriority & 3u] : 0;
        for (size_t i = draw->first; i <= draw->anchor; ++i) {
            const PpuGpu3DSBatch* batch = &sCommands.batches[i];
            if (batch->layer == draw->bg && !batch->objWindow) DrawZoneBatch(batch, &draw->rect, shift, draw->toBg, draw->toPriority);
        }
    }
}

/* The prepared frame drawn on one output target, each layer moved by its eye's shift
 * (sEyeShiftNdc, none for a single picture); `pass` says which part of it (OutputPass). */
static bool DrawOutput(C3D_RenderTarget* target, OutputPass pass) {
    const bool front = pass == PASS_FRONT_LEFT || pass == PASS_FRONT_RIGHT;
    /* citro2d programs the scissor for its own draws between our frames, so
     * the cache cannot survive across a frame boundary. */
    sScissorKeyLow = 0xffffffffu;
    sScissorKeyHigh = 0xffffffffu;
    /* The layers in front go over the base already drawn; the right eye's picture is all copied from the
     * left, so only its colour is cleared (outside the view) and the shared depth-stencil is left alone. */
    if (pass != PASS_FRONT_LEFT)
        C3D_RenderTargetClear(target, pass == PASS_FRONT_RIGHT ? C3D_CLEAR_COLOR : C3D_CLEAR_ALL,
                              ClearColor(sCommands.batches[0].color), 0);
    if (!C3D_FrameDrawOn(target)) return false;
    /* Scanline 0 has to land in target row 0, where the presenter samples it,
     * so the viewport sits at the top of the 512x256 surface; the scissor is
     * in the same space and must be rebased with it. */
    const u32 yOffset = sViewportOffset;
    C3D_SetViewport(0, yOffset, sPreparedWidth, sPreparedHeight);
    SetScissorCached(0, yOffset, sPreparedWidth, yOffset + sPreparedHeight);
    /* Depth is emitted as -(512 - order)/513 (obj_order_depth), so state the mapping instead of
     * inheriting whatever the Citro2D presenter last configured. */
    C3D_DepthMap(true, -1.0f, 0.0f);
    C3D_BindProgram(&sProgram);
    C3D_SetAttrInfo(&sAttributes);
    C3D_SetBufInfo(&sBufferInfo[sBuildBuffer]);
    C3D_CullFace(GPU_CULL_NONE);
    /* Nothing about the previous frame's state survives here. */
    sLastOffsetX = 1.0f;
    sLastOffsetY = 1.0f;
    SetBatchOffset(0.0f, 0.0f);
    sTevKey = 0xffffffffu;
    sBlendKey = 0xffffffffu;
    sTintKey = 0xffffffffu;
    for (int stage = 0; stage < 6; ++stage)
        C3D_TexEnvInit(C3D_GetTexEnv(stage));
    C3D_TexSetFilter(&sAtlas, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sAtlas, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0, &sAtlas);
    sBoundTexture = 0;
    /* The atlas and the view rings were rewritten in place for this frame (Commit), and the PICA's
     * texture cache can still hold lines of them from the last one: citro3d's texture-unit update writes
     * GPUREG_TEXUNIT_CONFIG with a byte mask that leaves bit 16 (clear the texture cache) out. A ring
     * cell reused for another map row then came out as its old (empty) texels on one line, beside a dark
     * room's light (issue #44). Only that byte is written, so the units citro3d enabled stay as they are. */
    GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, 1u << 16);
    if (pass == PASS_ALL || pass == PASS_BASE) LogBatchList();
    /* The base keeps the blend-target mark in both bits; each eye in front uses its own. */
    sTarget2Bit = pass == PASS_FRONT_RIGHT ? 0x10u : 0x08u;
    sTarget2WriteMask = pass == PASS_BASE ? 0x18u : sTarget2Bit;
    /* Windows and blending read the stencil as though it starts at zero every
     * frame. citro3d's target clear is the only thing that guarantees that,
     * and it skips the depth/stencil fill entirely when the attachment is
     * missing while still clearing colour -- which reads on screen as a frame
     * that is uniformly clear-coloured (every gated batch rejected) or
     * uniformly blended (only the alpha half passing). Resetting the plane
     * with a draw costs one quad and depends on nothing. The layers in front
     * read the marks the base left (DrawEyesOverBase), so they keep them. */
    if (!front) ClearStencilPlane();
    else {
        if (pass == PASS_FRONT_RIGHT) CopyLeftOutput();
        if (sFrontHasObj) ClearDepthPlane();
    }

    for (size_t i = 1; i < sCommands.batchCount; ++i) {
        const PpuGpu3DSBatch* batch = &sCommands.batches[i];
        if (!batch->objWindow || front) continue; /* DrawEyesOverBase: the OBJ windows are all in the base */
        ResetTev(batch);
        ResetBlend(batch);
        C3D_AlphaTest(true, GPU_GREATER, 0);
        C3D_DepthTest(true, GPU_ALWAYS, (GPU_WRITEMASK)0);
        C3D_StencilTest(true, GPU_ALWAYS, 0x04, 0xff, 0x04);
        C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP,
                      GPU_STENCIL_REPLACE);
        DrawBatch(batch);
    }

    for (size_t i = 1; i < sCommands.batchCount; ++i) {
        const PpuGpu3DSBatch* batch = &sCommands.batches[i];
        if (batch->layer != PPU_GPU3DS_OBJ || batch->objWindow || !PassDraws(pass, batch)) continue;
        if (IsPainterBatch(batch)) continue; /* no depth: it is drawn in the order the list has them */
        ResetTev(batch);
        ResetBlend(batch);
        C3D_AlphaTest(true, GPU_GREATER, 0);
        SetWindowStencil(batch);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_DEPTH);
        DrawBatch(batch);
    }

    for (size_t i = 1; i < sCommands.batchCount; ++i) {
        const PpuGpu3DSBatch* batch = &sCommands.batches[i];
        if (batch->objWindow || !PassDraws(pass, batch)) continue;
        if (batch->layer == PPU_GPU3DS_BACKDROP) {
            ResetTev(batch);
            ResetBlend(batch);
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            C3D_DepthTest(true, GPU_ALWAYS, (GPU_WRITEMASK)0);
            C3D_StencilTest(true, GPU_ALWAYS, sTarget2WriteMask, 0, sTarget2WriteMask);
            C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP,
                          GPU_STENCIL_REPLACE);
            DrawBatch(batch);
            continue;
        }
        ResetTev(batch);
        ResetBlend(batch);
        C3D_AlphaTest(true, GPU_GREATER, 0);
        SetColorStencil(batch);
        if (batch->layer == PPU_GPU3DS_OBJ)
            C3D_DepthTest(true, IsPainterBatch(batch) ? GPU_ALWAYS : GPU_EQUAL, GPU_WRITE_COLOR);
        else
            C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
        if (pass != PASS_BASE && IsGhostBatch(batch)) {
            sNoShift = true;
            DrawBatch(batch);
            sNoShift = false;
        }
        DrawBatch(batch);
        if (sZoneDrawCount != 0) DrawZonesAfter(i);
        /* A sprite's colour draw is sometimes lost on the Old 3DS (the Zz bubble's one tile in Hyrule Castle's
         * bedroom, about one frame in four): measured on the console, drawing the opaque OBJ batches twice
         * brought it from 3-9 bad frames in 66 to 0 in three recordings; the same batches twice in the depth
         * pre-pass did not (8-10). The second draw rewrites the same pixels, so it costs only the draw. The root
         * cause is not known (docs/PLAN.md, decisions log 2026-10-20). */
        if (batch->layer == PPU_GPU3DS_OBJ && batch->effect != PPU_GPU3DS_EFFECT_ALPHA && !batch->semiTransparent)
            DrawBatch(batch);
    }

    /* The iris (PpuGpu3DSFrameView::irisLines): the backdrop outside the circle,
     * before the fade quad so a fade darkens it as PAL_RAM's backdrop was. */
    if (sIrisIndexCount != 0 && pass != PASS_BASE) {
        for (int stage = 0; stage < 3; ++stage)
            C3D_TexEnvInit(C3D_GetTexEnv(stage));
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        const u32 r = sIrisColor & 31u, g = (sIrisColor >> 5u) & 31u, b = (sIrisColor >> 10u) & 31u;
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env, ((r << 3u) | (r >> 2u)) | (((g << 3u) | (g >> 2u)) << 8u) |
                                     (((b << 3u) | (b >> 2u)) << 16u) | 0xff000000u);
        C3D_BlendingColor(0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0);
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
        SetBatchOffset(0.0f, 0.0f);
        SetScissorCached(0, yOffset, sPreparedWidth, yOffset + sPreparedHeight);
        C3D_DrawElements(GPU_TRIANGLES, (int)sIrisIndexCount, C3D_UNSIGNED_SHORT, sIndices + sIrisFirstIndex);
    }

    /* A uniform screen fade (docs/gpu-fade.md): the picture above was drawn from
     * the unfaded palettes, so one full-screen quad of the fade's grey, as
     * opaque as its weight, over the finished frame is the game's fade. Blending
     * is linear, so it commutes with the alpha blends and the fades the frame
     * already did; the target's 5-bit channels quantise it as PAL_RAM did. */
    if (sFadeWeight != 0 && gPpuGpu3DSFadeQuadDisable == 0 && pass != PASS_BASE) {
        for (int stage = 0; stage < 3; ++stage)
            C3D_TexEnvInit(C3D_GetTexEnv(stage));
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env, (u32)sFadeLevel | ((u32)sFadeLevel << 8u) | ((u32)sFadeLevel << 16u) |
                                     ((u32)sFadeWeight << 24u));
        C3D_BlendingColor(0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_ZERO, GPU_ONE);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0);
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
        SetBatchOffset(0.0f, 0.0f);
        SetScissorCached(0, yOffset, sPreparedWidth, yOffset + sPreparedHeight);
        C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT,
                         sIndices + PPU_GPU3DS_CLEAR_FIRST_INDEX);
    }

    for (int stage = 0; stage < 3; ++stage)
        C3D_TexEnvInit(C3D_GetTexEnv(stage));
    C3D_BlendingColor(0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO,
                   GPU_ONE, GPU_ZERO);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    /* Scissor state is global Citro3D state, not render-target state.  Leaving
     * the last GBA scanline band active clips Citro2D's following top-screen
     * pass at screen x=240: this is why the end of DUMP SAVED was missing.
     * Disable it before returning to the presenter and invalidate the cache so
     * the next PPU frame cannot mistake the disabled state for its first band. */
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    sScissorKeyLow = 0xffffffffu;
    sScissorKeyHigh = 0xffffffffu;
    return true;
}

/* The right eye's output (docs/stereo-3d.md), made the first time a frame needs it. */
static void NoteRightOutputFailed(void) {
    static bool noted;
    if (noted) return;
    noted = true;
    char line[96];
    snprintf(line, sizeof(line), "[tmc3ds] stereo: no right-eye output, vram free %zu\n", (size_t)vramSpaceFree());
    Platform3DS_Debug(line);
}

static bool EnsureRightOutput(void) {
    if (sOutputRightTarget) return true;
    if (!C3D_TexInitVRAM(&sOutputRightTexture, PPU_GPU3DS_OUTPUT_WIDTH, PPU_GPU3DS_OUTPUT_HEIGHT, GPU_RGBA5551)) {
        sOutputRightTexture = (C3D_Tex){ 0 };
        NoteRightOutputFailed();
        return false;
    }
    C3D_TexSetFilter(&sOutputRightTexture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&sOutputRightTexture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    /* The left eye's depth and stencil are shared: the eyes are drawn one after the other and each
     * draw clears them first. A buffer of its own (512 KiB) did not fit in the New 3DS's VRAM beside
     * the presenter's right-eye target, and both eyes got the left picture. */
    sOutputRightTarget = C3D_RenderTargetCreateFromTex(&sOutputRightTexture, GPU_TEXFACE_2D, 0, -1);
    if (sOutputRightTarget)
        C3D_FrameBufDepth(&sOutputRightTarget->frameBuf, sOutputTarget->frameBuf.depthBuf, GPU_RB_DEPTH24_STENCIL8);
    if (!sOutputRightTarget || !sOutputRightTarget->frameBuf.depthBuf) {
        /* No VRAM left for it: both eyes keep the left picture. */
        if (sOutputRightTarget) C3D_RenderTargetDelete(sOutputRightTarget);
        sOutputRightTarget = NULL;
        C3D_TexDelete(&sOutputRightTexture);
        sOutputRightTexture = (C3D_Tex){ 0 };
        NoteRightOutputFailed();
        return false;
    }
    return true;
}

static void SetEyeShifts(bool shifted, Stereo3DSEye eye) {
    sEyeShifting = shifted;
    if (!shifted) return;
    const float toNdc = 2.0f / (float)sPreparedWidth;
    for (unsigned layer = 0; layer <= PPU_GPU3DS_OBJ; ++layer)
        for (unsigned priority = 0; priority < 4; ++priority) {
            /* Whole pixels: the art and its scissor move together, with no pixel doubled. */
            const float shift = Stereo3DS_LayerShift(layer, priority, eye);
            const int pixels = (int)(shift < 0.0f ? shift - 0.5f : shift + 0.5f);
            sEyeShiftPx[layer][priority] = pixels;
            sEyeShiftNdc[layer][priority] = (float)pixels * toNdc;
        }
    sEyeCurrent = eye;
    sEyeToNdc = toNdc;
    unsigned followLayer, followPriority;
    sEyeWindowFollows = Stereo3DS_ObjWindowFollows(&followLayer, &followPriority);
    if (sEyeWindowFollows) {
        sEyeWindowPx = sEyeShiftPx[followLayer][followPriority & 3u];
        sEyeWindowNdc = sEyeShiftNdc[followLayer][followPriority & 3u];
    }
}

/* Why the eyes cannot be drawn over one base picture this frame (0: they can). */
enum { SPLIT_OK, SPLIT_WINDOW_FOLLOWS, SPLIT_FRONT_WINDOW, SPLIT_FRONT_BLENDED_OBJ, SPLIT_BASE_OVER_FRONT, SPLIT_FORCED_WHOLE,
       SPLIT_NOTHING_IN_FRONT, SPLIT_LINES_UNCLIPPED, SPLIT_ZONE_UNDER_BASE };

/* With the left eye's shifts set (SetEyeShifts): marks the layers in front (sFrontLayer) and says whether the
 * frame can be drawn as one base picture with them over it, each eye moving only those (DrawEyesOverBase), or,
 * SPLIT_NOTHING_IN_FRONT, needs one picture for both eyes (no batch is on a layer in front: the HUD hidden).
 * It can when nothing on the screen plane is drawn after a layer in front on the same lines, no OBJ window
 * moves, and no sprite in front blends: the GBA blends a sprite with the background under it,
 * never with a lower sprite, which the base would have drawn there. The lines are a batch's only while the scissor
 * cuts them (gpu_scissor_mode 0 lets a band's tile rows overhang). */
enum { EYES_FRONT_OBJ_MAX = 192 };

/* The most a sprite moves in either eye, in whole pixels, from its two shifts. */
static int EyeMovePx(float left, float right) {
    const int l = (int)(left < 0.0f ? -left + 0.5f : left + 0.5f);
    const int r = (int)(right < 0.0f ? -right + 0.5f : right + 0.5f);
    return l > r ? l : r;
}

static int EyesOverBaseRefusal(void) {
    memset(sPromoted, 0, sizeof(sPromoted[0]) * sCommands.batchCount);
    for (unsigned layer = 0; layer <= PPU_GPU3DS_OBJ; ++layer)
        for (unsigned priority = 0; priority < 4; ++priority)
            sFrontLayer[layer][priority] = sEyeShiftPx[layer][priority] != 0;
    /* A window that follows a layer on the screen plane (the map screens' cut-out stays with their grey, BG2) is
     * where it is in both eyes: the base draws it and its marks serve both. */
    if (sEyeWindowFollows && sEyeWindowPx != 0) return SPLIT_WINDOW_FOLLOWS;
    if (Port_Config_GpuScissorMode() == 0) return SPLIT_LINES_UNCLIPPED;
    /* A zone that moves (its plane is in front) drawn after its layer's batch the base draws: the base has no shift, and
     * the front passes come after what the base draws over the layer (sprites), which must stay over the copy. */
    for (unsigned z = 0; z < sZoneDrawCount; ++z)
        if (sFrontLayer[sZoneDraws[z].toBg][sZoneDraws[z].toPriority & 3u] &&
            !IsFrontBatch(&sCommands.batches[sZoneDraws[z].anchor]))
            return SPLIT_ZONE_UNDER_BASE;
    sFrontHasObj = false;
    /* frontLines: the lines any batch in front covers. frontBgLines: the same for those that are not sprites. A sprite is
     * only as wide as its own box, so a sprite left on the screen plane is refused only where it meets a sprite in front
     * (frontObjs: its box, grown by the most it moves in either eye), not wherever the two share a line: Link's feet
     * (depth 0) beside his leaning body (depth 3) are on the same lines and never touch. */
    uint32_t frontLines[(PPU_GPU3DS_OUTPUT_HEIGHT + 31) / 32] = { 0 };
    uint32_t frontBgLines[(PPU_GPU3DS_OUTPUT_HEIGHT + 31) / 32] = { 0 };
    struct { int left, right, top, bottom; uint8_t object; } frontObjs[EYES_FRONT_OBJ_MAX];
    unsigned frontObjCount = 0;
    bool frontObjsFull = false;
    bool anyFront = false;
    for (size_t i = 1; i < sCommands.batchCount; ++i) {
        const PpuGpu3DSBatch* batch = &sCommands.batches[i];
        const unsigned first = batch->firstLine;
        unsigned end = first + batch->lineCount;
        if (end > PPU_GPU3DS_OUTPUT_HEIGHT) end = PPU_GPU3DS_OUTPUT_HEIGHT;
        if (IsFrontBatch(batch)) {
            if (batch->objWindow) return SPLIT_FRONT_WINDOW;
            /* A semi-transparent sprite with no second target to blend with (BLDCNT 0: the map screens' cursor) is
             * drawn opaque, effect NONE (append_layer_batches): only a real blend needs what is under it. */
            if (batch->layer == PPU_GPU3DS_OBJ &&
                (batch->effect == PPU_GPU3DS_EFFECT_ALPHA || (batch->color & PPU_GPU3DS_ALPHA_COMPLEMENT) != 0))
                return SPLIT_FRONT_BLENDED_OBJ;
            for (unsigned line = first; line < end; ++line) {
                frontLines[line / 32u] |= 1u << (line % 32u);
                if (batch->layer != PPU_GPU3DS_OBJ) frontBgLines[line / 32u] |= 1u << (line % 32u);
            }
            anyFront = true;
            if (batch->layer == PPU_GPU3DS_OBJ) {
                sFrontHasObj = true;
                if (frontObjCount < EYES_FRONT_OBJ_MAX) {
                    /* Forced to the front without a depth (the big Chuchu's pieces): assume the most. */
                    const int move = batch->depth != 0 ? EyeMovePx(Stereo3DS_SpriteShift(batch->depth, STEREO3DS_LEFT),
                                                                   Stereo3DS_SpriteShift(batch->depth, STEREO3DS_RIGHT))
                                                       : 8;
                    frontObjs[frontObjCount].left = (int)batch->scissorLeft - move;
                    frontObjs[frontObjCount].right = (int)batch->scissorRight + move;
                    frontObjs[frontObjCount].top = (int)first;
                    frontObjs[frontObjCount].bottom = (int)end;
                    frontObjs[frontObjCount].object = batch->objectIndex;
                    ++frontObjCount;
                } else {
                    frontObjsFull = true;
                }
            }
        } else if (anyFront && !batch->objWindow) {
            if (batch->layer != PPU_GPU3DS_OBJ || frontObjsFull) {
                bool over = false;
                for (unsigned line = first; line < end && !over; ++line)
                    over = (frontLines[line / 32u] & (1u << (line % 32u))) != 0;
                if (!over) continue;
                /* A background over what is in front (the Minish Woods' shadow layer, BG3 over the sprites with a
                 * depth) is drawn with the front passes, unmoved, over what they have drawn. */
                if (batch->layer > PPU_GPU3DS_BG3 || frontObjsFull) return SPLIT_BASE_OVER_FRONT;
                sPromoted[i] = true;
                for (unsigned line = first; line < end; ++line) {
                    frontLines[line / 32u] |= 1u << (line % 32u);
                    frontBgLines[line / 32u] |= 1u << (line % 32u);
                }
            } else {
                bool over = false;
                for (unsigned line = first; line < end && !over; ++line)
                    over = (frontBgLines[line / 32u] & (1u << (line % 32u))) != 0;
                for (unsigned f = 0; f < frontObjCount && !over; ++f) {
                    /* The runs of one sprite (its rows of tiles at different depths) share its box but not its pixels. */
                    if (frontObjs[f].object == batch->objectIndex) continue;
                    over = (int)first < frontObjs[f].bottom && (int)end > frontObjs[f].top &&
                           (int)batch->scissorLeft < frontObjs[f].right && (int)batch->scissorRight > frontObjs[f].left;
                }
                if (!over) continue;
                /* A sprite the game draws over one in front, or over a layer in front: drawn with them, unmoved, so it
                 * lands over them. One that blends needs what is under it, which the base has not got there. */
                if (batch->effect == PPU_GPU3DS_EFFECT_ALPHA || (batch->color & PPU_GPU3DS_ALPHA_COMPLEMENT) != 0 ||
                    frontObjCount >= EYES_FRONT_OBJ_MAX)
                    return SPLIT_BASE_OVER_FRONT;
                sPromoted[i] = true;
                sFrontHasObj = true;
                frontObjs[frontObjCount].left = (int)batch->scissorLeft;
                frontObjs[frontObjCount].right = (int)batch->scissorRight;
                frontObjs[frontObjCount].top = (int)first;
                frontObjs[frontObjCount].bottom = (int)end;
                frontObjs[frontObjCount].object = batch->objectIndex;
                ++frontObjCount;
                for (unsigned line = first; line < end; ++line) frontLines[line / 32u] |= 1u << (line % 32u);
            }
        }
    }
    return anyFront ? SPLIT_OK : SPLIT_NOTHING_IN_FRONT;
}

/* The layers on the screen plane are drawn once, on the left output; the right eye's output copies that picture,
 * and only the layers in front are drawn for each eye with its shift. Drawing the whole frame twice doubled the
 * GPU's work: in the Deepwood Shrine's dark room (area 0x48 room 0x10, New 3DS) 9.0 ms a frame in 2D, 17.1 with
 * both eyes whole (40 fps), 12.6 this way (54 fps; docs/PLAN.md, decisions log 2026-10-18). */
static bool DrawEyesOverBase(void) {
    SetEyeShifts(false, STEREO3DS_LEFT);
    if (!DrawOutput(sOutputTarget, PASS_BASE)) return false;
    if (EnsureRightOutput()) {
        SetEyeShifts(true, STEREO3DS_RIGHT);
        sRightReady = DrawOutput(sOutputRightTarget, PASS_FRONT_RIGHT);
    }
    SetEyeShifts(true, STEREO3DS_LEFT);
    return DrawOutput(sOutputTarget, PASS_FRONT_LEFT);
}

static void NoteEyesPath(int refusal) {
    (void)refusal;
}

bool PortPpuGpu3DS_DrawPrepared(void) {
    const uint64_t startTick = svcGetSystemTick();
    sRightReady = false;
    if (!sReady || sDisabled || !sPrepared || sCommands.batchCount == 0)
        return FinishDraw(false, startTick);
    sPrepared = false;
    sViewportOffset = Port_Config_GpuViewportOffset()
                              ? PPU_GPU3DS_OUTPUT_HEIGHT - sPreparedHeight
                              : 0u;
    sHasObjWindow = false;
    for (size_t i = 1; i < sCommands.batchCount; ++i) {
        if (sCommands.batches[i].objWindow) { sHasObjWindow = true; break; }
    }
    /* With a layer off the screen plane each eye gets its own picture; otherwise the presenter
     * shows this one to both. */
    const bool stereo = Stereo3DS_HasDepth();
    sUnderlay = stereo && Stereo3DS_WorldScene();
    SetEyeShifts(stereo, STEREO3DS_LEFT);
    PrepareZoneDraws();
    bool drawn;
    const int refusal = !stereo ? SPLIT_OK : gPpuGpu3DSEyesWhole ? SPLIT_FORCED_WHOLE : EyesOverBaseRefusal();
    if (stereo) NoteEyesPath(refusal);
    if (stereo && refusal == SPLIT_OK) {
        drawn = DrawEyesOverBase();
    } else if (stereo && refusal == SPLIT_NOTHING_IN_FRONT) {
        /* No batch moves: one picture, which the presenter shows to both eyes. */
        SetEyeShifts(false, STEREO3DS_LEFT);
        drawn = DrawOutput(sOutputTarget, PASS_ALL);
    } else {
        drawn = DrawOutput(sOutputTarget, PASS_ALL);
        if (drawn && stereo && EnsureRightOutput()) {
            SetEyeShifts(true, STEREO3DS_RIGHT);
            sRightReady = DrawOutput(sOutputRightTarget, PASS_ALL);
        }
    }
    SetEyeShifts(false, STEREO3DS_LEFT);
    if (drawn) sDrawnBuffer = sBuildBuffer;
    return FinishDraw(drawn, startTick);
}

/* The presenter reuses this shader for its static quad, so its vertices must be
 * PpuGpu3DSVertex itself (platform_gpu_3ds.c aliases PresentVertex to it) --
 * the attribute info bound here describes the packed 16-byte layout, including
 * the int16 UV the shader rescales by 1/PPU_GPU3DS_UV_SCALE. */
bool PortPpuGpu3DS_BindPresentShader(void) {
    if (!sReady) return false;
    C3D_BindProgram(&sProgram);
    C3D_SetAttrInfo(&sAttributes);
    SetBatchOffset(0.0f, 0.0f);
    return true;
}

void* PortPpuGpu3DS_OutputTexture(void) {
    return sReady && !sDisabled ? &sOutputTexture : NULL;
}

void* PortPpuGpu3DS_OutputTextureRight(void) {
    return sReady && !sDisabled && sRightReady ? &sOutputRightTexture : NULL;
}

void PortPpuGpu3DS_Disable(void) {
    /* Retiring the GPU path is the loudest event this renderer has: everything
     * afterwards runs on the software rasterizer, and the reason is otherwise
     * only visible in a dump taken before the evidence is overwritten. */
    if (!sDisabled) {
        char line[192];
        snprintf(line, sizeof(line),
                 "[tmc3ds] PICA200 retired: parity %llu/%llu differ=%llu "
                 "structural=%llu first=%lu,%lu offset=%lu\n",
                 (unsigned long long)sStats.parityChecks,
                 (unsigned long long)sStats.parityFailures,
                 (unsigned long long)sStats.differingPixels,
                 (unsigned long long)sStats.structuralPixels,
                 (unsigned long)sStats.firstDiffX,
                 (unsigned long)sStats.firstDiffY,
                 (unsigned long)sViewportOffset);
        Platform3DS_Debug(line);
    }
    sDisabled = true;
    sPrepared = false;
    sParityRequested = false;
    sParityReferenceReady = false;
    sParitySubmitted = false;
    sStats.enabled = false;
    sStats.disabled = true;
}

bool PortPpuGpu3DS_IsDisabled(void) {
    return sDisabled;
}

void PortPpuGpu3DS_RecordDisabledFrame(void) {
    if (sReady && sDisabled) ++sStats.disabledFrames;
}

void PortPpuGpu3DS_GetStats(PortPpuGpu3DSStats* stats) {
    if (!stats) return;
    sStats.initialized = sReady;
    sStats.enabled = sReady && !sDisabled;
    sStats.disabled = sDisabled;
    *stats = sStats;
}

/* ---------------------------------------------------------------------------
 * Last-frame capture
 *
 * A fault that corrects itself the moment a dump is taken cannot be captured
 * by the parity path: that path re-renders in software and rewinds HDMA before
 * anything is written, so it records the healthy frame. This reads back the
 * output texture at the very start of a frame, while it still holds what the
 * screen is actually showing, and touches no game state at all.
 * ------------------------------------------------------------------------ */
static bool sCaptureRequested;
static bool sCaptureSubmitted;

void PortPpuGpu3DS_RequestFrameCapture(void) {
    if (sReady && !sDisabled && !sCaptureSubmitted) sCaptureRequested = true;
}

bool PortPpuGpu3DS_FrameCaptureRequested(void) {
    return sCaptureRequested;
}

bool PortPpuGpu3DS_QueueFrameCapture(void) {
    if (!sCaptureRequested || !sParityReadback) return false;
    sCaptureRequested = false;
    if (!PlatformGpu3DS_QueueRgba5551Readback(&sOutputTexture, sParityReadback))
        return false;
    sCaptureSubmitted = true;
    return true;
}

void PortPpuGpu3DS_WriteFrameCapture(const char* prefix) {
    const char* directory = prefix;
    if (!sCaptureSubmitted || !directory || directory[0] == 0) return;
    sCaptureSubmitted = false;
    if (R_FAILED(GSPGPU_InvalidateDataCache(
                sParityReadback, PPU_GPU3DS_OUTPUT_WIDTH *
                                         PPU_GPU3DS_OUTPUT_HEIGHT *
                                         sizeof(*sParityReadback))))
        return;
    char path[224];
    snprintf(path, sizeof(path), "%s/gpu-frame.bin", directory);
    FILE* file = fopen(path, "wb");
    if (!file) return;
    fwrite(sParityReadback, sizeof(*sParityReadback),
           PPU_GPU3DS_OUTPUT_WIDTH * PPU_GPU3DS_OUTPUT_HEIGHT, file);
    fclose(file);
}

/* The fade the frame last built applied on the GPU (weight 0: none), for the
 * frame recorder. */
uint8_t PortPpuGpu3DS_FadeWeight(void) { return sFadeWeight; }
uint8_t PortPpuGpu3DS_FadeLevel(void) { return sFadeLevel; }

void PortPpuGpu3DS_RequestParityCheck(void) {
    if (sReady && !sDisabled && !sParityRequested && !sParitySubmitted) {
        sParityRequested = true;
        sParityReferenceReady = false;
        sParityDeferrals = 0;
    }
}

bool PortPpuGpu3DS_ParityRequested(void) {
    return sParityRequested;
}

void PortPpuGpu3DS_CaptureParityReference(const uint32_t* pixels, unsigned pitch,
                                          unsigned width, unsigned height) {
    if (!sParityRequested || !pixels || pitch < width ||
        width > PPU_GPU3DS_OUTPUT_WIDTH || height > PPU_GPU3DS_OUTPUT_HEIGHT) {
        sParityRequested = false;
        sParityReferenceReady = false;
        return;
    }
    for (unsigned y = 0; y < height; ++y) {
        const uint32_t* source = pixels + (size_t)y * pitch;
        uint16_t* reference = sParityReference + (size_t)y * PPU_GPU3DS_OUTPUT_WIDTH;
        for (unsigned x = 0; x < width; ++x)
            reference[x] = PpuGpu3DS_PackAbgr8888(source[x]);
    }
    sParityWidth = width;
    sParityHeight = height;
    sParityReferenceReady = true;
}

bool PortPpuGpu3DS_QueueParityCopy(void) {
    if (!sParityRequested || !sParityReferenceReady ||
        !PlatformGpu3DS_QueueRgba5551Readback(&sOutputTexture, sParityReadback))
        return false;
    sParityRequested = false;
    sParityReferenceReady = false;
    sParitySubmitted = true;
    return true;
}

void PortPpuGpu3DS_DeferParityCheck(void) {
    if (!sParityRequested) return;
    if (++sParityDeferrals > PPU_GPU3DS_PARITY_MAX_DEFERRALS) {
        PortPpuGpu3DS_CancelParityCheck();
        return;
    }
    /* Keep the request alive but drop the captured reference: the next frame
     * the GPU can build gets a fresh one taken from its own state. */
    sParityReferenceReady = false;
}

void PortPpuGpu3DS_CancelParityCheck(void) {
    sParityRequested = false;
    sParityReferenceReady = false;
    sParityDeferrals = 0;
}

bool PortPpuGpu3DS_ParityFinishedThisFrame(void) {
    return sParityFinishedThisFrame;
}

/* A 5-bit channel pair that differs by one step is the documented floor of
 * PICA200 blending: the GBA folds `(a*eva + b*evb) >> 4` with four fractional
 * bits, and an 8-bit blender constant cannot carry the last one. Anything
 * larger is a real renderer defect. */
static bool ChannelsWithinOneStep(uint16_t reference, uint16_t readback) {
    static const unsigned shifts[3] = { 11u, 6u, 1u };
    for (unsigned channel = 0; channel < 3; ++channel) {
        const int left = (int)((reference >> shifts[channel]) & 0x1fu);
        const int right = (int)((readback >> shifts[channel]) & 0x1fu);
        const int delta = left > right ? left - right : right - left;
        if (delta > 1) return false;
    }
    return (reference & 1u) == (readback & 1u);
}

static void WriteParitySurfaces(uint64_t differing) {
    const char* directory = Port_PPU_3DS_LastDumpDirectory(); /* a prefix: <prefix>-<name> */
    if (!directory || directory[0] == 0) return;
    static const struct {
        const char* name;
        const uint16_t* pixels;
    } surfaces[2] = { { "parity-cpu.bin", NULL }, { "parity-gpu.bin", NULL } };
    const uint16_t* sources[2] = { sParityReference, sParityReadback };
    char path[224];
    for (unsigned index = 0; index < 2; ++index) {
        snprintf(path, sizeof(path), "%s/%s", directory, surfaces[index].name);
        FILE* file = fopen(path, "wb");
        if (!file) continue;
        fwrite(sources[index], sizeof(uint16_t),
               PPU_GPU3DS_OUTPUT_WIDTH * PPU_GPU3DS_OUTPUT_HEIGHT, file);
        fclose(file);
    }

    snprintf(path, sizeof(path), "%s/parity-diff.txt", directory);
    FILE* report = fopen(path, "w");
    if (!report) return;
    fprintf(report, "differing %llu of %u visible pixels (%ux%u)\n",
            (unsigned long long)differing, sParityWidth * sParityHeight,
            sParityWidth, sParityHeight);
    unsigned listed = 0;
    for (unsigned y = 0; y < sParityHeight && listed < 256u; ++y) {
        const uint16_t* reference =
                sParityReference + (size_t)y * PPU_GPU3DS_OUTPUT_WIDTH;
        const uint16_t* readback =
                sParityReadback + (size_t)y * PPU_GPU3DS_OUTPUT_WIDTH;
        for (unsigned x = 0; x < sParityWidth && listed < 256u; ++x) {
            if (reference[x] == readback[x]) continue;
            fprintf(report, "%u,%u cpu=%04x gpu=%04x %s\n", x, y, reference[x],
                    readback[x],
                    ChannelsWithinOneStep(reference[x], readback[x])
                            ? "one-step"
                            : "STRUCTURAL");
            ++listed;
        }
    }
    fclose(report);
}

void PortPpuGpu3DS_FinishParityCheck(void) {
    sParityFinishedThisFrame = false;
    if (!sParitySubmitted) return;
    /* FrameBegin waits for the prior Citro3D queue, including the queued
     * readback, and leaves this frame active for the selected render path. */
    if (!PlatformGpu3DS_BeginCustomTop()) {
        /* Busy, not broken: leave the comparison armed for a later frame rather
         * than retiring the renderer over a skipped frame begin. */
        return;
    }
    sParitySubmitted = false;
    sParityFinishedThisFrame = true;
    ++sStats.parityChecks;
    if (R_FAILED(GSPGPU_InvalidateDataCache(
            sParityReadback, PPU_GPU3DS_OUTPUT_WIDTH * PPU_GPU3DS_OUTPUT_HEIGHT *
                                 sizeof(*sParityReadback)))) {
        ++sStats.parityFailures;
        PortPpuGpu3DS_Disable();
        return;
    }

    uint64_t differing = 0;
    uint64_t structural = 0;
    uint32_t firstX = 0;
    uint32_t firstY = 0;
    /* The viewport places scanline y in target row y and the untiled readback
     * keeps that row order, so both buffers index identically. */
    for (unsigned y = 0; y < sParityHeight; ++y) {
        const uint16_t* reference =
            sParityReference + (size_t)y * PPU_GPU3DS_OUTPUT_WIDTH;
        const uint16_t* readback =
            sParityReadback + (size_t)y * PPU_GPU3DS_OUTPUT_WIDTH;
        for (unsigned x = 0; x < sParityWidth; ++x) {
            if (reference[x] == readback[x]) continue;
            if (differing == 0) {
                firstX = x;
                firstY = y;
            }
            ++differing;
            if (!ChannelsWithinOneStep(reference[x], readback[x])) ++structural;
        }
    }

    if (differing == 0) return;
    sStats.differingPixels += differing;
    sStats.structuralPixels += structural;
    sStats.firstDiffX = firstX;
    sStats.firstDiffY = firstY;
    WriteParitySurfaces(differing);
    if (structural != 0) {
        /* What the first differing pixels were, and the fade in force: enough
         * to tell a fade parameter from a renderer defect without a dump. */
        char line[200];
        snprintf(line, sizeof(line),
                 "[tmc3ds] parity: differing=%llu structural=%llu first=%u,%u cpu=%04x gpu=%04x fade=%u/%u\n",
                 (unsigned long long)differing, (unsigned long long)structural, (unsigned)firstX,
                 (unsigned)firstY,
                 sParityReference[(size_t)firstY * PPU_GPU3DS_OUTPUT_WIDTH + firstX],
                 sParityReadback[(size_t)firstY * PPU_GPU3DS_OUTPUT_WIDTH + firstX], (unsigned)sFadeWeight,
                 (unsigned)sFadeLevel);
        Platform3DS_Debug(line);
    }
    if (structural == 0) return;
    /* A difference the blender cannot explain is counted and written out, but
     * it no longer retires the renderer: the check is asked for by a person
     * (GPU CHECK, the link), and dropping to the software path for the rest of
     * the session turned a few odd pixels into 25 fps and no wide view. */
    ++sStats.parityFailures;
}
