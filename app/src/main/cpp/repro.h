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

// Draw a fullscreen red rectangle into a 256x256 EGL pbuffer default
// framebuffer using a gl_VertexID-generated triangle strip. Then read pixels
// back directly with glReadPixels.
//
// Expected: sampled pixels are (255, 0, 0, 255).
// If direct `-gpu swiftshader` returns transparent black here while swangle is
// red, this isolates the Goodnotes empty renderer output to a tiny valid GLES3
// case that does not rely on textures, FBO attachments, or PNG generation.
ReproStatus repro_run_test(uint8_t* pixels, int width, int height);

#endif
