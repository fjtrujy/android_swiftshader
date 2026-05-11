#include "repro.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#define LOG_TAG "swsrepro"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Returns a single summary string to the Kotlin side. Format:
//   "<success:0|1>|<error>|<centerR>,<centerG>,<centerB>,<centerA>|<cornerR>,<cornerG>,<cornerB>,<cornerA>"
// Plus fills `outPixels` (must be at least SWSREPRO_OUTPUT_W*SWSREPRO_OUTPUT_H*4 bytes)
// with the raw RGBA8 output for PNG encoding on the Kotlin side.
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_run(JNIEnv* env, jclass clazz, jbyteArray outPixels) {
    (void)clazz;

    const int W = SWSREPRO_OUTPUT_W;
    const int H = SWSREPRO_OUTPUT_H;
    const size_t byteCount = (size_t)W * (size_t)H * 4;

    jsize len = (*env)->GetArrayLength(env, outPixels);
    if ((size_t)len < byteCount) {
        char buf[64];
        snprintf(buf, sizeof(buf), "outPixels too small (need %zu bytes)", byteCount);
        char msg[256];
        snprintf(msg, sizeof(msg), "0|%s|0,0,0,0|0,0,0,0", buf);
        return (*env)->NewStringUTF(env, msg);
    }

    uint8_t* pixels = malloc(byteCount);
    if (!pixels) {
        return (*env)->NewStringUTF(env, "0|malloc failed|0,0,0,0|0,0,0,0");
    }

    ReproStatus status = repro_run(pixels);

    int cr = 0, cg = 0, cb = 0, ca = 0;
    int kr = 0, kg = 0, kb = 0, ka = 0;
    if (status.success) {
        size_t center = ((size_t)H / 2 * W + W / 2) * 4;
        cr = pixels[center];     cg = pixels[center + 1]; cb = pixels[center + 2]; ca = pixels[center + 3];
        kr = pixels[0];          kg = pixels[1];          kb = pixels[2];          ka = pixels[3];
        (*env)->SetByteArrayRegion(env, outPixels, 0, (jsize)byteCount, (const jbyte*)pixels);
    }

    char msg[512];
    snprintf(msg, sizeof(msg), "%d|%s|%d,%d,%d,%d|%d,%d,%d,%d",
             status.success ? 1 : 0, status.error,
             cr, cg, cb, ca, kr, kg, kb, ka);
    free(pixels);
    return (*env)->NewStringUTF(env, msg);
}
