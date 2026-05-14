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

// Draw a fullscreen red rectangle into an immutable, single-level RGBA8 render
// target using the Goodnotes RendererV5 shape: std140 UBO, gl_VertexID-generated
// triangle strip, and no vertex attributes. Then read pixels back through the
// Android snapshot path.
//
// Expected: sampled pixels are (255, 0, 0, 255).
// If direct `-gpu swiftshader` returns transparent black here while swangle is
// red, this isolates the Goodnotes empty snapshot failure to a tiny GLES case.
ReproStatus repro_run_test(uint8_t* pixels, int width, int height);

#endif
