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

// Variant 11: chained frame — combines variant 9 (CPU upload via glTexSubImage2D) and
// variant 8 (texture sampling) inside one render pass on a 512x512 target FBO at the
// canonical offset (128, 128). Mirrors the renderer's actual per-frame pipeline:
//   1. Upload CPU rasterizer output to a source texture (glTexSubImage2D).
//   2. Bind target FBO, clear to transparent black.
//   3. Sample source via textured quad shader, .copy blend mode, draw offset quad.
//   4. glFinish + glReadPixels.
// Expected:
//   - center (256, 256) inside the sampled quad: (255, 0, 0, 255).
//   - corner (0, 0)   outside the quad:         (0, 0, 0, 0).
ReproStatus repro_variant11_chained_frame(uint8_t* pixels, int width, int height);

// Variant 12: multi-frame loop. Repeats variant 11's full chain 50 times against the
// same GL context, only reading back on the final iteration. Tests whether state leaks
// across frames cause SwiftShader to drift toward the (255, 255, 255, 0) sentinel.
// Expected: same as variant 11.
ReproStatus repro_variant12_multiframe_loop(uint8_t* pixels, int width, int height);

// Variant 13: instanced textured draw + normal alpha blend. Distilled from the GL trace
// of the GoodNotes Renderer's final composite step on the failing scene:
//   - 512x512 RGBA8 target FBO, cleared to (0, 0, 0, 0).
//   - 256x256 RGBA8 source texture pre-filled with opaque red (via glTexImage2D).
//   - Vertex shader uses gl_InstanceID to position 16 quads in a 4x4 grid covering NDC
//     [-1, 1].
//   - Fragment shader samples from the source texture.
//   - glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
//   - glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, instanceCount=16).
// Requires GLES3 (gl_InstanceID).
// Expected: every pixel == (255, 0, 0, 255).
ReproStatus repro_variant13_instanced_textured_blend(uint8_t* pixels, int width, int height);

// Variant 14: negative-Y viewport. The renderer's atlas-write path issues
//   `glViewport(0, -2048, 4096, 4096)` on a 2048x2048 framebuffer — viewport origin
// negative, viewport extends past the framebuffer. Spec-legal but historically a
// weak spot in software rasterizers. This variant minimises that:
//   - 256x256 RGBA8 target FBO.
//   - glViewport(0, -256, 512, 512) — half above, half below the framebuffer.
//   - glClear to (0, 0, 0, 0).
//   - Draw a full-NDC red quad. Half the quad falls outside the framebuffer; the
//     remaining half should still rasterize red into the bottom half of the framebuffer.
// Expected: bottom half of framebuffer (rows 0..128 with Y-up) red.
ReproStatus repro_variant14_negative_y_viewport(uint8_t* pixels, int width, int height);

// Variant 15: drawArraysInstanced with per-instance VERTEX ATTRIBUTE (via
// glVertexAttribDivisor), not gl_InstanceID. The renderer's strokes / textured-
// rectangles pass instance data this way, not via gl_InstanceID arithmetic.
// SwiftShader may handle per-instance attribute divisor incorrectly.
//   - 512x512 RGBA8 target FBO, cleared to (0, 0, 0, 0).
//   - 256x256 RGBA8 source texture pre-filled with opaque red.
//   - One 4-vertex VBO for the quad (divisor=0).
//   - One 16-instance VBO for per-instance tile offsets (divisor=1).
//   - Vertex shader computes NDC position = tileOffset + quadVertex * tileSize.
//   - Fragment shader samples source texture.
//   - glEnable(GL_BLEND) + glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
//   - glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 16).
// Requires GLES3.
// Expected: every pixel == (255, 0, 0, 255).
ReproStatus repro_variant15_instanced_attribute_divisor(uint8_t* pixels, int width, int height);

// Variant 16: state-pollution sequence. Mirrors the renderer's pattern of
// (a) first drawing into a large atlas FBO with `viewport(0, -size, 2*size, 2*size)`
// then (b) drawing to the final output FBO with a normal viewport. SwiftShader may
// hold onto state from the first draw that breaks the second one.
//   - Pass 1: bind atlas FBO (256x256), viewport(0, -256, 512, 512), clear, draw a
//     1-instance quad with .copy blend (mirrors `drawColoredRectangle`).
//   - Pass 2: bind output FBO (256x256), viewport(0, 0, 256, 256), clear, draw
//     instanced textured-quad (16 instances, normal blend).
//   - Read back pass-2 output.
// Expected: every pixel red (same as variant 13).
ReproStatus repro_variant16_state_pollution(uint8_t* pixels, int width, int height);

// Variant 17: drawArraysInstanced sampling from a `sampler2D[N]` array indexed by
// a per-instance vertex attribute. RULED OUT — swiftshader handled this correctly,
// swangle rejected at compile (stricter spec validator). Renderer's real shader
// must use a different pattern.
ReproStatus repro_variant17_sampler_array_dynamic_index(uint8_t* pixels, int width, int height);

// Variant 18: drawArraysInstanced sampling from a `sampler2DArray` with per-instance
// layer index. The standard GLES 3.0 way to "select among N textures per instance".
//   - 512x512 RGBA8 target FBO.
//   - One sampler2DArray texture (3D = width x height x depth) with 2 layers, each
//     filled with opaque red (via glTexImage3D).
//   - Per-instance int attribute provides layer index (alternating 0, 1).
//   - 16 instances, 4x4 grid.
//   - Normal alpha blend, glDrawArraysInstanced(..., 16).
// Expected: every pixel red.
ReproStatus repro_variant18_sampler_2d_array(uint8_t* pixels, int width, int height);

// Variant 19: SHARED EGL context cross-thread texture upload with fence sync, then
// sample on the render thread. Mirrors the renderer's `BackgroundGLUploader` pattern:
//   - Main thread: create primary EGL context A, make current with pbuffer.
//   - Worker thread (pthread): create EGL context B with `share_context = A`, make
//     current with its own pbuffer, allocate texture via glTexStorage2D, upload red
//     pixels via PBO + glTexSubImage2D, glFenceSync, glFlush, release context.
//   - Main thread: glWaitSync(fence), then sample the texture in a shader, draw
//     a full quad with normal blend, glReadPixels.
// Expected: every pixel red.
// This is the closest minimal-NDK approximation of the renderer's actual upload+sample
// flow that any of my variants have attempted. If SwiftShader has a cross-context
// texture-visibility bug, this should hit it.
ReproStatus repro_variant19_shared_context_upload_and_sample(uint8_t* pixels, int width, int height);

#endif
