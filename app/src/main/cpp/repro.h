#ifndef SWSREPRO_REPRO_H
#define SWSREPRO_REPRO_H

#include <stdint.h>
#include <stddef.h>

#define SWSREPRO_WIDTH  256
#define SWSREPRO_HEIGHT 256

// Variant 7 mirrors the GoodNotes Renderer's canonical failing scene exactly,
// so the framebuffer is 512x512 to match the page size that historically failed.
#define SWSREPRO_V7_WIDTH  512
#define SWSREPRO_V7_HEIGHT 512

// Result of a single variant run.
// On success: error[0] == 0, pixels filled with width*height*4 bytes RGBA.
// On failure: error contains a null-terminated description.
typedef struct {
    int success;
    char error[256];
} ReproStatus;

// Variant 2: clear an offscreen RGBA8 framebuffer to OPAQUE RED and glReadPixels into `pixels`.
// `pixels` must point to at least width*height*4 bytes.
// Expected output: every pixel == (R=255, G=0, B=0, A=255).
// On SwiftShader Android emulator, observed: every pixel == (R=255, G=255, B=255, A=0).
ReproStatus repro_variant2_fbo_clear_readpixels(uint8_t* pixels, int width, int height);

// Variant 3: same as variant 2, but `pixels` points to an AndroidBitmap_lockPixels-mapped buffer.
// The caller is responsible for locking/unlocking the AndroidBitmap.
ReproStatus repro_variant3_fbo_clear_readpixels_into_bitmap(uint8_t* mapped_pixels, int width, int height);

// Variant 4: shader-based draw. Clear FBO to opaque BLACK, then rasterize a full-viewport
// quad whose fragment shader emits opaque GREEN. Tests shader compilation + linking + draw.
// Expected: every pixel == (0, 255, 0, 255).
ReproStatus repro_variant4_shader_fullscreen_quad(uint8_t* pixels, int width, int height);

// Variant 5: MSAA-resolve path. Allocate an MSAA renderbuffer (RGBA8, 4 samples), use it as
// the color attachment of FBO_A. Clear to opaque RED. Resolve to a single-sample texture-FBO_B
// via glBlitFramebuffer. Read FBO_B back.
// Expected: every pixel == (255, 0, 0, 255).
// Requires GLES3 (glRenderbufferStorageMultisample + glBlitFramebuffer).
ReproStatus repro_variant5_msaa_resolve(uint8_t* pixels, int width, int height);

// Variant 6: sRGB color attachment. RGBA8 texture but with GL_SRGB8_ALPHA8 internal format.
// Clear to opaque RED. Read back.
// Expected: every pixel == (255, 0, 0, 255) (sRGB encoding of 1.0 is still 1.0).
// Requires GLES3.
ReproStatus repro_variant6_srgb_framebuffer(uint8_t* pixels, int width, int height);

// Variant 7: mirrors GoodNotes Renderer's `drawColoredRectangle` (`.copy` blendMode) call.
// 512x512 RGBA8 FBO, clear to transparent black (0, 0, 0, 0), then draw a 256x256 red quad
// at pixel offset (128, 128) using `glDisable(GL_BLEND) + glBlendFunc(GL_ONE, GL_ZERO)`.
// Expected:
//   - center pixel (256, 256) inside the quad: (255, 0, 0, 255)
//   - corner pixel (0, 0)   outside the quad: (0, 0, 0, 0)
ReproStatus repro_variant7_offset_quad_copy_blend(uint8_t* pixels, int width, int height);

// Variant 8: texture-sampling. Create a 256x256 source texture initialized to red
// via `glTexImage2D` with non-NULL data. Bind a second texture as the target FBO.
// Draw a fullscreen quad whose fragment shader samples from the source texture.
// Mirrors the Renderer's `drawTexturedRectangles` call.
// Expected: every pixel == (255, 0, 0, 255).
ReproStatus repro_variant8_texture_sampling(uint8_t* pixels, int width, int height);

// Variant 9: CPU → GL upload via `glTexSubImage2D`. Allocate a CPU buffer, fill it with
// solid red, allocate an empty GL texture, upload via glTexSubImage2D, bind as FBO,
// `glReadPixels` back. Mirrors the Renderer's `.syncCPU` rasterizer upload path.
// Expected: every pixel == (255, 0, 0, 255).
ReproStatus repro_variant9_texsubimage_upload(uint8_t* pixels, int width, int height);

// Variant 10: off-thread GL. Spawn a worker pthread that does the EGL init + variant 7
// (offset red quad with .copy blend on transparent black) + cleanup. Main thread waits
// via `pthread_join`. Mirrors the Renderer running off the activity's main thread.
// Expected: same as variant 7.
ReproStatus repro_variant10_offthread_gl(uint8_t* pixels, int width, int height);

#endif
