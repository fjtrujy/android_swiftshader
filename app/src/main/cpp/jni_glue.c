#include "repro.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <android/bitmap.h>
#include <android/log.h>

#define LOG_TAG "swsrepro"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Builds a Kotlin-side ReproResult object via reflection-free direct field writes.
// To keep this simple we return a String that encodes:
//   "<success:0|1>|<error>|<centerR>,<centerG>,<centerB>,<centerA>|<cornerR>,<cornerG>,<cornerB>,<cornerA>"
// and a separate byte[] for the pixels. Kotlin parses the summary string and
// constructs the Bitmap itself.
static jstring make_summary(JNIEnv* env, const ReproStatus* status,
                            const uint8_t* pixels, int width, int height) {
    int cr = 0, cg = 0, cb = 0, ca = 0;
    int kr = 0, kg = 0, kb = 0, ka = 0;
    if (status->success && pixels) {
        size_t center = ((size_t)height / 2 * width + width / 2) * 4;
        size_t corner = 0;
        cr = pixels[center];     cg = pixels[center + 1]; cb = pixels[center + 2]; ca = pixels[center + 3];
        kr = pixels[corner];     kg = pixels[corner + 1]; kb = pixels[corner + 2]; ka = pixels[corner + 3];
    }
    char buf[512];
    snprintf(buf, sizeof(buf), "%d|%s|%d,%d,%d,%d|%d,%d,%d,%d",
             status->success ? 1 : 0,
             status->error,
             cr, cg, cb, ca,
             kr, kg, kb, ka);
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant2(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    jsize len = (*env)->GetArrayLength(env, out_pixels);
    if (len < SWSREPRO_WIDTH * SWSREPRO_HEIGHT * 4) {
        ReproStatus s = {0, "out_pixels too small"};
        return make_summary(env, &s, NULL, 0, 0);
    }

    uint8_t* pixels = malloc(SWSREPRO_WIDTH * SWSREPRO_HEIGHT * 4);
    if (!pixels) { ReproStatus s = {0, "malloc failed"}; return make_summary(env, &s, NULL, 0, 0); }

    ReproStatus s = repro_variant2_fbo_clear_readpixels(pixels, SWSREPRO_WIDTH, SWSREPRO_HEIGHT);

    if (s.success) {
        (*env)->SetByteArrayRegion(env, out_pixels, 0,
                                    SWSREPRO_WIDTH * SWSREPRO_HEIGHT * 4,
                                    (const jbyte*)pixels);
    }
    jstring summary = make_summary(env, &s, pixels, SWSREPRO_WIDTH, SWSREPRO_HEIGHT);
    free(pixels);
    return summary;
}

// Helper: runs a variant that writes into a malloc'd buffer, then copies into out_pixels.
typedef ReproStatus (*VariantFn)(uint8_t* pixels, int width, int height);

static jstring run_bytearray_variant(JNIEnv* env, jbyteArray out_pixels, VariantFn fn) {
    jsize len = (*env)->GetArrayLength(env, out_pixels);
    if (len < SWSREPRO_WIDTH * SWSREPRO_HEIGHT * 4) {
        ReproStatus s = {0, "out_pixels too small"};
        return make_summary(env, &s, NULL, 0, 0);
    }
    uint8_t* pixels = malloc(SWSREPRO_WIDTH * SWSREPRO_HEIGHT * 4);
    if (!pixels) { ReproStatus s = {0, "malloc failed"}; return make_summary(env, &s, NULL, 0, 0); }

    ReproStatus s = fn(pixels, SWSREPRO_WIDTH, SWSREPRO_HEIGHT);

    if (s.success) {
        (*env)->SetByteArrayRegion(env, out_pixels, 0,
                                    SWSREPRO_WIDTH * SWSREPRO_HEIGHT * 4,
                                    (const jbyte*)pixels);
    }
    jstring summary = make_summary(env, &s, pixels, SWSREPRO_WIDTH, SWSREPRO_HEIGHT);
    free(pixels);
    return summary;
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant4(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return run_bytearray_variant(env, out_pixels, repro_variant4_shader_fullscreen_quad);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant5(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return run_bytearray_variant(env, out_pixels, repro_variant5_msaa_resolve);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant6(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return run_bytearray_variant(env, out_pixels, repro_variant6_srgb_framebuffer);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant8(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return run_bytearray_variant(env, out_pixels, repro_variant8_texture_sampling);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant9(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return run_bytearray_variant(env, out_pixels, repro_variant9_texsubimage_upload);
}

static jstring run_v512_variant(JNIEnv* env, jbyteArray out_pixels,
                                 const char* name, VariantFn fn) {
    const int W = SWSREPRO_V7_WIDTH, H = SWSREPRO_V7_HEIGHT;
    jsize len = (*env)->GetArrayLength(env, out_pixels);
    if (len < W * H * 4) {
        ReproStatus s = {0};
        snprintf(s.error, sizeof(s.error), "%s out_pixels too small (need 512*512*4)", name);
        return make_summary(env, &s, NULL, 0, 0);
    }
    uint8_t* pixels = malloc(W * H * 4);
    if (!pixels) { ReproStatus s = {0, "malloc failed"}; return make_summary(env, &s, NULL, 0, 0); }

    ReproStatus s = fn(pixels, W, H);

    if (s.success) {
        (*env)->SetByteArrayRegion(env, out_pixels, 0, W * H * 4, (const jbyte*)pixels);
    }
    jstring summary = make_summary(env, &s, pixels, W, H);
    free(pixels);
    return summary;
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant10(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return run_v512_variant(env, out_pixels, "variant10", repro_variant10_offthread_gl);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant11(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return run_v512_variant(env, out_pixels, "variant11", repro_variant11_chained_frame);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant12(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return run_v512_variant(env, out_pixels, "variant12", repro_variant12_multiframe_loop);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant13(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    // Variant 13 uses a 512x512 target (mirrors renderer's page size).
    return run_v512_variant(env, out_pixels, "variant13", repro_variant13_instanced_textured_blend);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant14(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    // Variant 14 uses 256x256 — viewport is then (0, -256, 512, 512).
    return run_bytearray_variant(env, out_pixels, repro_variant14_negative_y_viewport);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant15(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return run_v512_variant(env, out_pixels, "variant15", repro_variant15_instanced_attribute_divisor);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant16(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return run_bytearray_variant(env, out_pixels, repro_variant16_state_pollution);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant17(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return run_v512_variant(env, out_pixels, "variant17", repro_variant17_sampler_array_dynamic_index);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant7(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    const int W = SWSREPRO_V7_WIDTH, H = SWSREPRO_V7_HEIGHT;
    jsize len = (*env)->GetArrayLength(env, out_pixels);
    if (len < W * H * 4) {
        ReproStatus s = {0, "variant7 out_pixels too small (need 512*512*4)"};
        return make_summary(env, &s, NULL, 0, 0);
    }
    uint8_t* pixels = malloc(W * H * 4);
    if (!pixels) { ReproStatus s = {0, "malloc failed"}; return make_summary(env, &s, NULL, 0, 0); }

    ReproStatus s = repro_variant7_offset_quad_copy_blend(pixels, W, H);

    if (s.success) {
        (*env)->SetByteArrayRegion(env, out_pixels, 0, W * H * 4, (const jbyte*)pixels);
    }
    jstring summary = make_summary(env, &s, pixels, W, H);
    free(pixels);
    return summary;
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant3(JNIEnv* env, jclass clazz, jobject bitmap) {
    (void)clazz;
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) {
        ReproStatus s = {0, "AndroidBitmap_getInfo failed"};
        return make_summary(env, &s, NULL, 0, 0);
    }
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        ReproStatus s = {0, "bitmap format must be RGBA_8888"};
        return make_summary(env, &s, NULL, 0, 0);
    }

    void* mapped = NULL;
    if (AndroidBitmap_lockPixels(env, bitmap, &mapped) != ANDROID_BITMAP_RESULT_SUCCESS || !mapped) {
        ReproStatus s = {0, "AndroidBitmap_lockPixels failed"};
        return make_summary(env, &s, NULL, 0, 0);
    }

    ReproStatus s = repro_variant3_fbo_clear_readpixels_into_bitmap(
        (uint8_t*)mapped, (int)info.width, (int)info.height);

    jstring summary = make_summary(env, &s, (const uint8_t*)mapped, (int)info.width, (int)info.height);

    AndroidBitmap_unlockPixels(env, bitmap);
    return summary;
}
