#include "repro.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#define LOG_TAG "swsrepro"

// Format: "<success:0|1>|<error>|<centerR>,<centerG>,<centerB>,<centerA>|<cornerR>,<cornerG>,<cornerB>,<cornerA>"
static jstring summarize(JNIEnv* env, const ReproStatus* status,
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

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runPreamble(JNIEnv* env, jclass clazz) {
    (void)clazz;
    ReproStatus s = repro_preamble_offthread_fresh_context();
    return summarize(env, &s, NULL, 0, 0);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runTest(JNIEnv* env, jclass clazz, jbyteArray outPixels) {
    (void)clazz;
    const int W = SWSREPRO_OUTPUT_W;
    const int H = SWSREPRO_OUTPUT_H;
    const size_t byteCount = (size_t)W * (size_t)H * 4;

    jsize len = (*env)->GetArrayLength(env, outPixels);
    if ((size_t)len < byteCount) {
        ReproStatus s = {0};
        snprintf(s.error, sizeof(s.error), "outPixels too small (need %zu bytes)", byteCount);
        return summarize(env, &s, NULL, 0, 0);
    }

    uint8_t* pixels = malloc(byteCount);
    if (!pixels) {
        ReproStatus s = {0, "malloc failed"};
        return summarize(env, &s, NULL, 0, 0);
    }

    ReproStatus s = repro_test_shared_context_upload(pixels);
    if (s.success) {
        (*env)->SetByteArrayRegion(env, outPixels, 0, (jsize)byteCount, (const jbyte*)pixels);
    }
    jstring out = summarize(env, &s, pixels, W, H);
    free(pixels);
    return out;
}
