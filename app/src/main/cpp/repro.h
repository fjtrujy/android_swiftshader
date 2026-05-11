#ifndef SWSREPRO_REPRO_H
#define SWSREPRO_REPRO_H

#include <stdint.h>
#include <stddef.h>

#define SWSREPRO_WIDTH  512
#define SWSREPRO_HEIGHT 512

// Result of a run.
// On success: error[0] == 0, pixels filled with width*height*4 bytes RGBA.
// On failure: error contains a null-terminated description.
typedef struct {
    int success;
    char error[256];
} ReproStatus;

// SHARED EGL context cross-thread texture upload with fence sync, then
// sample on the render thread:
//   - Main thread: create primary EGL context A, make current with pbuffer.
//   - Worker thread (pthread): create EGL context B with `share_context = A`,
//     make current with its own pbuffer, allocate texture via glTexStorage2D,
//     upload red pixels via PBO + glTexSubImage2D, glFenceSync, glFlush,
//     release context.
//   - Main thread: glWaitSync(fence), then sample the texture in a shader,
//     draw a full quad with normal blend, glReadPixels.
// Expected: every pixel red.
// On Android emulator `-gpu swiftshader`: every pixel (0, 0, 0, 0).
// On `-gpu swangle` (ANGLE → Vulkan → SwiftShader-Vulkan): correct red.
ReproStatus repro_run_test(uint8_t* pixels, int width, int height);

#endif
