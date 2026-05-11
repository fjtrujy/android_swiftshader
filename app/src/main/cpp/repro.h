#ifndef SWSREPRO_REPRO_H
#define SWSREPRO_REPRO_H

#include <stdint.h>
#include <stddef.h>

// Output framebuffer the test reads back. 512x512 RGBA8.
#define SWSREPRO_OUTPUT_W 512
#define SWSREPRO_OUTPUT_H 512

typedef struct {
    int  success;
    char error[256];
} ReproStatus;

/// Preamble: one off-thread fresh-context GL session, joined back. Does its
/// OWN eglInit + GLES2 context creation + shader compile + draw + readback +
/// eglTerminate, all on a worker pthread. No state is shared with the test below.
///
/// This is what variant 10 in the original 18-variant investigation did. It
/// alone is sufficient to prime SwiftShader's bug — without it the cross-context
/// test below passes cleanly.
ReproStatus repro_preamble_offthread_fresh_context(void);

/// Test: shared-EGL-context cross-thread upload + main-thread sample.
///
/// 1. Main thread eglInit + creates GLES3 context A + pbuffer + makeCurrent.
/// 2. Spawns a worker pthread that creates context B with share_context = A,
///    allocates a 256x256 RGBA8 texture, uploads opaque-red pixels via PBO +
///    glTexSubImage2D, glFenceSync, glFlush, releases context, exits.
/// 3. Main thread joins, glWaitSync(fence), binds the worker-uploaded texture,
///    draws a full-NDC quad with normal alpha blend, glReadPixels.
///
/// Fills `pixels` (must be at least SWSREPRO_OUTPUT_W*SWSREPRO_OUTPUT_H*4 bytes)
/// with the raw RGBA8 output.
///
/// Expected output: every pixel == (R=255, G=0, B=0, A=255).
/// On `-gpu swiftshader` (Linux x86_64 emulator, SwiftShader 4.0.0.1) AFTER the
/// preamble has run: every pixel reads as (0, 0, 0, 0).
ReproStatus repro_test_shared_context_upload(uint8_t* pixels);

#endif
