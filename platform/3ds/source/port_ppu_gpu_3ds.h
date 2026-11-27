#pragma once

#include "port_ppu_gpu_3ds_model.h"


typedef struct PortPpuGpu3DSStats {
    uint64_t attemptedFrames, renderedFrames, fallbackFrames, disabledFrames;
    uint64_t tileHits, tileDecodes, atlasFlushBytes, atlasFlushCalls;
    uint64_t vertices, batches, parityChecks, parityFailures, differingPixels;
    /* Differences the blender's 8-bit constants cannot account for. */
    uint64_t structuralPixels;
    uint32_t firstDiffX, firstDiffY;
    uint64_t preflightTicks, drawTicks;
    /* Preflight split into command building and GPU cache maintenance. */
    uint64_t buildTicks, flushTicks;
    /* Indexed by PpuGpu3DSBuildReason, plus the high-water marks that say how
     * close a rendered frame came to the command-buffer limits. */
    uint64_t buildFailures[PPU_GPU3DS_BUILD_REASON_COUNT];
    uint64_t backoffFrames;
    uint32_t maxBands, maxVertices, maxBatches, maxRequiredVertices;
    uint64_t mapLayers, mapRebuilds, mapRejects[PPU_GPU3DS_MAP_REJECT_COUNT];
    uint64_t mapRebuildReason[PPU_GPU3DS_MAP_REBUILD_COUNT];
    uint64_t mapRefreshes;
    uint32_t mapLargestQuads;
    /* The frame the quick dump captured, so a host replay is comparable. */
    uint64_t lastBuildTicks;
    uint32_t lastBands, lastVertices;
    uint64_t commandBudgetFallbacks;
    /* Map slices copied into the buffer being built because the other one had them (build ahead). */
    uint64_t sliceCopies;
    uint32_t lastCommandWords, maxCommandWords;
    uint8_t lastMapLayerMask;
    bool initialized, enabled, disabled;
} PortPpuGpu3DSStats;

bool PortPpuGpu3DS_Init(void);
void PortPpuGpu3DS_Shutdown(void);
/* BG1 drawn from this picture (GBA 15-bit colours, row-major, up to 512x256) in frames whose view
 * has bg1Picture set; NULL frees it. The texture is remade when the pointer or colour correction changes. */
void PortPpuGpu3DS_SetBg1Picture(const uint16_t* gbaPixels, unsigned width, unsigned height);
/* The picture texture from palette indices instead (one byte a pixel, (palette - firstPalette) * 16 +
 * index into four BG palettes, 0 transparent, row-major, up to 512x256): its colours are remade
 * from the palettes each frame is drawn with whenever they change, so a palette the game cycles
 * animates it (the file select's cave). NULL frees it. Kept until cleared here: a NULL
 * SetBg1Picture leaves it. */
void PortPpuGpu3DS_SetIndexedPicture(const uint8_t* indices, unsigned width, unsigned height, unsigned firstPalette);
/* BG `bg`'s side strips (GBA 15-bit colours, row-major, the left strip's columns then the right one's, up to 256
 * wide and 1024 tall, 0 transparent) for frames whose view has the BG in scrollPictureMask (the Crenel paths'
 * shaft); a texture per BG, apart from the one above. With `repeat`, the bands above and below a room instead
 * (rowPictureMask, Crenel's horizontal Minish path): `width` is a power of two up to 1024 and the texture is that wide,
 * repeated sideways. NULL frees it. Remade when the pointer or colour correction changes. */
void PortPpuGpu3DS_SetScrollPicture(unsigned bg, const uint16_t* gbaPixels, unsigned width, unsigned height, bool repeat);
/* Debug view (DEBUG -> SWITCHES -> PLANE TINT, not saved): every quad of the frame painted in one flat colour
 * keeping its silhouette. 0 off; 1 LAYERS: a colour per GBA layer (BG0 magenta, BG1 cyan, BG2 orange, BG3 green,
 * sprites white), darker the further back its priority; 2 DEPTH: the depth stereo_3ds.c gives the layer, in blue,
 * dark behind the screen and bright in front of it. Backdrop and windows are left as they are. */
void PortPpuGpu3DS_SetTintMode(int mode);
int PortPpuGpu3DS_TintMode(void);
/* A frame is prepared in two steps (docs/gpu-build-ahead.md). Preflight builds it: the geometry
 * into the vertex buffer the GPU is not drawing from, the tiles into copies of the atlas and the
 * rings, so it may run before the GPU frame is claimed when PortPpuGpu3DS_BuildsAhead says so
 * (otherwise claim it first). Commit, once the frame is claimed (C3D_FrameBegin has waited for
 * the previous one), checks the command budget and moves the new tiles into the textures. */
bool PortPpuGpu3DS_BuildsAhead(void);
bool PortPpuGpu3DS_Preflight(const PpuGpu3DSFrameView* frame);
bool PortPpuGpu3DS_Commit(const PpuGpu3DSFrameView* frame);
bool PortPpuGpu3DS_DrawPrepared(void);
bool PortPpuGpu3DS_BindPresentShader(void);
void* PortPpuGpu3DS_OutputTexture(void);
/* The right eye's picture of the frame just drawn, when its layers have a depth
 * (stereo_3ds.h); NULL: both eyes show PortPpuGpu3DS_OutputTexture. */
void* PortPpuGpu3DS_OutputTextureRight(void);
void PortPpuGpu3DS_Disable(void);
bool PortPpuGpu3DS_IsDisabled(void);
void PortPpuGpu3DS_RecordDisabledFrame(void);
void PortPpuGpu3DS_GetStats(PortPpuGpu3DSStats* stats);
/* debug/tmc-dump-NNNN or debug/tmc-check-NNNN: the file prefix of the last screen
 * dump or GPU CHECK, for the frame capture and the parity artifacts. */
const char* Port_PPU_3DS_LastDumpDirectory(void);
/* GPU CHECK: asks for the next frame to be drawn by both renderers, in a numbered set of its own. */
void Port_PPU_3DS_RequestGpuCheck(void);
/* Reads back the frame currently on screen, before anything perturbs it. */
void PortPpuGpu3DS_RequestFrameCapture(void);
bool PortPpuGpu3DS_FrameCaptureRequested(void);
bool PortPpuGpu3DS_QueueFrameCapture(void);
void PortPpuGpu3DS_WriteFrameCapture(const char* prefix);

unsigned long long PortPpuGpu3DS_EmptyDrawsSkipped(void);
uint8_t PortPpuGpu3DS_FadeWeight(void);
uint8_t PortPpuGpu3DS_FadeLevel(void);
void PortPpuGpu3DS_RequestParityCheck(void);
bool PortPpuGpu3DS_ParityRequested(void);
void PortPpuGpu3DS_CaptureParityReference(const uint32_t* pixels, unsigned pitch,
                                          unsigned width, unsigned height);
bool PortPpuGpu3DS_QueueParityCopy(void);
void PortPpuGpu3DS_CancelParityCheck(void);
void PortPpuGpu3DS_DeferParityCheck(void);
bool PortPpuGpu3DS_ParityFinishedThisFrame(void);
void PortPpuGpu3DS_FinishParityCheck(void);
