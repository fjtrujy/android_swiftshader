#ifndef SWSREPRO_REPRO_H
#define SWSREPRO_REPRO_H

#include <stdint.h>
#include <stddef.h>

#define SWSREPRO_WIDTH  256
#define SWSREPRO_HEIGHT 256

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

#endif
