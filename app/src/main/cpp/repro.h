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

// Render four opaque color quadrants into an immutable, single-level RGBA8
// texture, then mirror the Goodnotes Android snapshot path: attach that texture
// to READ_FRAMEBUFFER, blit into an RGBA8 renderbuffer with source Y flipped,
// and glReadPixels back.
//
// Expected row samples after the flipped blit:
//   row0-left blue, row0-right white, lastrow-left red, lastrow-right green.
// If direct `-gpu swiftshader` returns transparent black here, the Goodnotes
// empty snapshot artifacts are likely in SwiftShader's FBO blit/readback path.
ReproStatus repro_run_test(uint8_t* pixels, int width, int height);

#endif
