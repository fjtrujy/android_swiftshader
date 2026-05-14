// JNI surface. Kotlin sees a single method on ReproNative:
//   runTest(outPixels) -> tiny valid GLES3 gl_VertexID draw + readback.

#include "repro.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#define LOG_TAG "swsrepro"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Build a "<success>|<error>|<centerR,G,B,A>|<cornerR,G,B,A>" summary string
// for the Kotlin side, sampling the pixel buffer at center and corner.
static jstring make_summary(JNIEnv* env, const ReproStatus* status,
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
Java_com_example_swsrepro_ReproNative_runTest(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    const int W = SWSREPRO_WIDTH, H = SWSREPRO_HEIGHT;
    const size_t byteCount = (size_t)W * (size_t)H * 4;

    jsize len = (*env)->GetArrayLength(env, out_pixels);
    if ((size_t)len < byteCount) {
        ReproStatus s = {0};
        snprintf(s.error, sizeof(s.error), "runTest outPixels too small (need %zu bytes)", byteCount);
        return make_summary(env, &s, NULL, 0, 0);
    }

    uint8_t* pixels = malloc(byteCount);
    if (!pixels) {
        ReproStatus s = {0, "malloc failed"};
        return make_summary(env, &s, NULL, 0, 0);
    }
    ReproStatus s = repro_run_test(pixels, W, H);
    if (s.success) {
        (*env)->SetByteArrayRegion(env, out_pixels, 0, (jsize)byteCount, (const jbyte*)pixels);
    }
    jstring out = make_summary(env, &s, pixels, W, H);
    free(pixels);
    return out;
}
