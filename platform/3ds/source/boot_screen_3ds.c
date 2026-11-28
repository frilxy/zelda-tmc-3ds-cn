/* Boot screen adapted from tinaut1986/zelda-tmc-3ds v0.4.7 (GPL-3.0-or-later). */
#include "boot_screen_3ds.h"

#include <3ds.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SCREEN_HEIGHT = 240, TOP_WIDTH = 400, BOTTOM_WIDTH = 320, TRIFORCE_SIZE = 56 };

static Thread sAnimationThread;
static atomic_bool sStopAnimation;
static bool sSplashStereo;

static void DrawSplash(void) {
    FILE* image = fopen("romfs:/splash.rgb565", "rb");
    if (!image) return;
    const size_t pixelCount = TOP_WIDTH * SCREEN_HEIGHT;
    u16* pixels = malloc(pixelCount * sizeof(*pixels));
    const bool loaded = pixels && fread(pixels, sizeof(*pixels), pixelCount, image) == pixelCount;
    fclose(image);
    if (!loaded) {
        free(pixels);
        return;
    }

    const int disparity = (int)(3.0f * osGet3DSliderState() + 0.5f);
    sSplashStereo = disparity > 0;
    if (sSplashStereo) gfxSet3D(true);
    /* Seed both buffers so later swaps can only show the splash. */
    for (int pass = 0; pass < 2; ++pass) {
        for (int eye = 0; eye < (sSplashStereo ? 2 : 1); ++eye) {
            u16* fb = (u16*)gfxGetFramebuffer(GFX_TOP, eye ? GFX_RIGHT : GFX_LEFT, NULL, NULL);
            if (!fb) continue;
            const int shift = eye ? -disparity : disparity;
            for (int x = 0; x < TOP_WIDTH; ++x) {
                const int srcX = x - shift;
                for (int y = 0; y < SCREEN_HEIGHT; ++y) {
                    fb[x * SCREEN_HEIGHT + SCREEN_HEIGHT - 1 - y] =
                        srcX >= 0 && srcX < TOP_WIDTH ? pixels[y * TOP_WIDTH + srcX] : 0;
                }
            }
        }
        gfxFlushBuffers();
        gfxScreenSwapBuffers(GFX_TOP, sSplashStereo);
        gspWaitForVBlank();
    }
    free(pixels);
}

static bool TriforceContains(float x, float y) {
    if (y < 0.5f) return fabsf(x - 0.5f) <= y * 0.5f;
    const float halfWidth = (y - 0.5f) * 0.5f;
    return fabsf(x - 0.25f) <= halfWidth || fabsf(x - 0.75f) <= halfWidth;
}

static void Animate(void* argument) {
    (void)argument;
    const int left = (BOTTOM_WIDTH - TRIFORCE_SIZE) / 2;
    const int top = (SCREEN_HEIGHT - TRIFORCE_SIZE) / 2;
    for (unsigned frame = 0; !atomic_load_explicit(&sStopAnimation, memory_order_relaxed); ++frame) {
        u16* fb = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        if (!fb) break;
        const float scale = fmaxf(0.04f, fabsf(cosf((float)frame * 0.07f)));
        for (int y = 0; y < TRIFORCE_SIZE; ++y) {
            const float v = (y + 0.5f) / TRIFORCE_SIZE;
            const int shade = 255 - (int)(v * 70.0f);
            const u16 gold = (u16)(((shade >> 3) << 11) | (((shade * 205 / 255) >> 2) << 5) |
                                   ((shade * 40 / 255) >> 3));
            for (int x = 0; x < TRIFORCE_SIZE; ++x) {
                const float u = 0.5f + ((x + 0.5f) / TRIFORCE_SIZE - 0.5f) / scale;
                fb[(left + x) * SCREEN_HEIGHT + SCREEN_HEIGHT - 1 - (top + y)] =
                    TriforceContains(u, v) ? gold : 0;
            }
        }
        gfxFlushBuffers();
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
        gspWaitForVBlank();
    }
}

void BootScreen3DS_Start(void) {
    DrawSplash();
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    for (int pass = 0; pass < 2; ++pass) {
        u16* fb = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        if (fb) memset(fb, 0, BOTTOM_WIDTH * SCREEN_HEIGHT * sizeof(*fb));
        gfxFlushBuffers();
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
    }
    atomic_store_explicit(&sStopAnimation, false, memory_order_relaxed);
    sAnimationThread = threadCreate(Animate, NULL, 16 * 1024, 0x30, -2, false);
}

void BootScreen3DS_Stop(void) {
    if (sAnimationThread) {
        atomic_store_explicit(&sStopAnimation, true, memory_order_relaxed);
        threadJoin(sAnimationThread, U64_MAX);
        threadFree(sAnimationThread);
        sAnimationThread = NULL;
    }
    if (sSplashStereo) {
        gfxSet3D(false);
        sSplashStereo = false;
    }
}
