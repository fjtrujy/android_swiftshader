// padding.c — STUBBED-OUT historical variants (bisect-padding-size experiment).
//
// Original: this file contained ~1600 lines of GLES code for 17 unused
// "variants" from the bisection that found the SwiftShader Android-emulator
// render bug. None were ever called from Kotlin (MainActivity only calls
// variant 10 + variant 19, defined in repro.c). They were kept because
// aggressive stripping made the bug stop reproducing.
//
// This file replaces every variant body with a trivial stub. The JNI exports
// and function signatures stay so the symbols remain in the binary. Goal:
// determine whether the trigger is
//   (a) binary size / .text padding alone — bug still reproduces with stubs, or
//   (b) specific code patterns — bug stops reproducing with stubs.
//
// If (a): we can drop padding.c entirely and use dummy `.text` padding
// (inline-asm NOPs) of the minimum size needed.
// If (b): we need to bisect further to find which variants are load-bearing.

#include "repro_internal.h"

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/bitmap.h>
#include <android/log.h>

#define LOG_TAG "swsrepro"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Stubbed variant bodies. Each returns success=1 with zeroed pixels.
// Signatures match the originals so the JNI exports below can reference them.
// ---------------------------------------------------------------------------

static ReproStatus stub_variant(uint8_t* pixels, int width, int height) {
    (void)pixels; (void)width; (void)height;
    return (ReproStatus){.success = 1, .error = ""};
}

ReproStatus repro_variant2_fbo_clear_readpixels(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant3_fbo_clear_readpixels_into_bitmap(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant4_shader_fullscreen_quad(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant5_msaa_resolve(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant6_srgb_framebuffer(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant7_offset_quad_copy_blend(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant8_texture_sampling(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant9_texsubimage_upload(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant11_chained_frame(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant12_multiframe_loop(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant13_instanced_textured_blend(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant14_negative_y_viewport(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant15_instanced_attribute_divisor(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant16_state_pollution(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant17_sampler_array_dynamic_index(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}
ReproStatus repro_variant18_sampler_2d_array(uint8_t* pixels, int width, int height) {
    return stub_variant(pixels, width, height);
}

// ---------------------------------------------------------------------------
// Dead-but-linked JNI exports. None are called from Kotlin; they exist as
// exported `JNIEXPORT` symbols (visibility=default) so the linker preserves
// the variant function bodies above.
// ---------------------------------------------------------------------------

static jstring padding_make_summary(JNIEnv* env, const ReproStatus* status,
                                     const uint8_t* pixels, int width, int height) {
    int cr = 0, cg = 0, cb = 0, ca = 0;
    int kr = 0, kg = 0, kb = 0, ka = 0;
    if (status->success && pixels) {
        size_t center = ((size_t)height / 2 * width + width / 2) * 4;
        cr = pixels[center];     cg = pixels[center + 1]; cb = pixels[center + 2]; ca = pixels[center + 3];
        kr = pixels[0];          kg = pixels[1];          kb = pixels[2];          ka = pixels[3];
    }
    char buf[512];
    snprintf(buf, sizeof(buf), "%d|%s|%d,%d,%d,%d|%d,%d,%d,%d",
             status->success ? 1 : 0, status->error,
             cr, cg, cb, ca, kr, kg, kb, ka);
    return (*env)->NewStringUTF(env, buf);
}

typedef ReproStatus (*PaddingVariantFn)(uint8_t* pixels, int width, int height);

static jstring padding_run_256(JNIEnv* env, jbyteArray out_pixels, PaddingVariantFn fn) {
    jsize len = (*env)->GetArrayLength(env, out_pixels);
    if (len < SWSREPRO_WIDTH * SWSREPRO_HEIGHT * 4) {
        ReproStatus s = {0, "out_pixels too small"};
        return padding_make_summary(env, &s, NULL, 0, 0);
    }
    uint8_t* pixels = malloc(SWSREPRO_WIDTH * SWSREPRO_HEIGHT * 4);
    if (!pixels) { ReproStatus s = {0, "malloc failed"}; return padding_make_summary(env, &s, NULL, 0, 0); }
    ReproStatus s = fn(pixels, SWSREPRO_WIDTH, SWSREPRO_HEIGHT);
    if (s.success) {
        (*env)->SetByteArrayRegion(env, out_pixels, 0,
                                    SWSREPRO_WIDTH * SWSREPRO_HEIGHT * 4,
                                    (const jbyte*)pixels);
    }
    jstring out = padding_make_summary(env, &s, pixels, SWSREPRO_WIDTH, SWSREPRO_HEIGHT);
    free(pixels);
    return out;
}

static jstring padding_run_512(JNIEnv* env, jbyteArray out_pixels, PaddingVariantFn fn) {
    const int W = SWSREPRO_V7_WIDTH, H = SWSREPRO_V7_HEIGHT;
    jsize len = (*env)->GetArrayLength(env, out_pixels);
    if (len < W * H * 4) {
        ReproStatus s = {0, "padding out_pixels too small (need 512*512*4)"};
        return padding_make_summary(env, &s, NULL, 0, 0);
    }
    uint8_t* pixels = malloc(W * H * 4);
    if (!pixels) { ReproStatus s = {0, "malloc failed"}; return padding_make_summary(env, &s, NULL, 0, 0); }
    ReproStatus s = fn(pixels, W, H);
    if (s.success) {
        (*env)->SetByteArrayRegion(env, out_pixels, 0, W * H * 4, (const jbyte*)pixels);
    }
    jstring out = padding_make_summary(env, &s, pixels, W, H);
    free(pixels);
    return out;
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant2(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return padding_run_256(env, out_pixels, repro_variant2_fbo_clear_readpixels);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant3(JNIEnv* env, jclass clazz, jobject bitmap) {
    (void)clazz;
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) {
        ReproStatus s = {0, "AndroidBitmap_getInfo failed"};
        return padding_make_summary(env, &s, NULL, 0, 0);
    }
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        ReproStatus s = {0, "bitmap format must be RGBA_8888"};
        return padding_make_summary(env, &s, NULL, 0, 0);
    }
    void* mapped = NULL;
    if (AndroidBitmap_lockPixels(env, bitmap, &mapped) != ANDROID_BITMAP_RESULT_SUCCESS || !mapped) {
        ReproStatus s = {0, "AndroidBitmap_lockPixels failed"};
        return padding_make_summary(env, &s, NULL, 0, 0);
    }
    ReproStatus s = repro_variant3_fbo_clear_readpixels_into_bitmap(
        (uint8_t*)mapped, (int)info.width, (int)info.height);
    jstring out = padding_make_summary(env, &s, (const uint8_t*)mapped, (int)info.width, (int)info.height);
    AndroidBitmap_unlockPixels(env, bitmap);
    return out;
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant4(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant4_shader_fullscreen_quad);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant5(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant5_msaa_resolve);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant6(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant6_srgb_framebuffer);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant7(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant7_offset_quad_copy_blend);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant8(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant8_texture_sampling);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant9(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant9_texsubimage_upload);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant11(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant11_chained_frame);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant12(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant12_multiframe_loop);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant13(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant13_instanced_textured_blend);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant14(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant14_negative_y_viewport);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant15(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant15_instanced_attribute_divisor);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant16(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant16_state_pollution);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant17(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant17_sampler_array_dynamic_index);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant18(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant18_sampler_2d_array);
}
