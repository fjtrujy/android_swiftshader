#ifndef SWSREPRO_REPRO_H
#define SWSREPRO_REPRO_H

#include <stdint.h>
#include <stddef.h>

#define SWSREPRO_WIDTH  256
#define SWSREPRO_HEIGHT 256

typedef struct {
    int success;
    char error[256];
} ReproStatus;

// Render an immutable, single-level RGBA8 source texture (filled with opaque
// red) into a destination FBO via a textured-quad shader, then glReadPixels
// back. The source texture's min filter is left at the default
// (GL_NEAREST_MIPMAP_LINEAR).
//
// Expected (GLES3 §8.17 — immutable textures are always complete):
//   every pixel (255, 0, 0, 255).
// On Android emulator `-gpu swiftshader`:
//   every pixel (0, 0, 0, 0).
// On `-gpu swangle` (ANGLE → Vulkan → SwiftShader-Vulkan):
//   every pixel (255, 0, 0, 255).
ReproStatus repro_run_test(uint8_t* pixels, int width, int height);

#endif
