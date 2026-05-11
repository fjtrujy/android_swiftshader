#ifndef SWSREPRO_REPRO_H
#define SWSREPRO_REPRO_H

#include <stdint.h>
#include <stddef.h>

// The output framebuffer the main thread reads back. Sized 512x512 to match the
// scene we originally observed the failure on; any size produces the same result.
#define SWSREPRO_OUTPUT_W 512
#define SWSREPRO_OUTPUT_H 512

typedef struct {
    int  success;
    char error[256];
} ReproStatus;

/// Standalone minimal reproducer for a SwiftShader Android-emulator render bug:
///
///   1. Main thread creates EGL context A on the default display, with a 1x1 pbuffer
///      surface, and makes it current.
///   2. Worker pthread creates EGL context B with `share_context = A`, its own 1x1
///      pbuffer, makes it current. On context B:
///        - glGenTextures / glTexStorage2D allocates a 256x256 RGBA8 texture.
///        - PBO + glMapBufferRange writes opaque-red pixels into the mapped buffer.
///        - glUnmapBuffer + glTexSubImage2D from PBO uploads the data.
///        - glFenceSync + glFlush, then eglMakeCurrent(NO_CONTEXT) to release.
///   3. Main thread joins the worker, glWaitSync(fence), binds the worker-uploaded
///      texture, draws a full-NDC quad with normal alpha blend into a 512x512 RGBA8
///      FBO, glFinish + glReadPixels.
///
/// Expected: every pixel == (R=255, G=0, B=0, A=255).
/// On `-gpu swiftshader` (Linux x86_64 emulator, SwiftShader 4.0.0.1): every pixel
/// reads as (0, 0, 0, 0).
/// On `-gpu swangle` (ANGLE -> Vulkan -> SwiftShader-Vulkan): expected output.
ReproStatus repro_run(uint8_t* pixels);

#endif
