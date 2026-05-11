#include "repro.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <android/log.h>

#define LOG_TAG "swsrepro"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static void set_err(ReproStatus* s, const char* msg) {
    s->success = 0;
    size_t n = strlen(msg);
    if (n >= sizeof(s->error)) n = sizeof(s->error) - 1;
    memcpy(s->error, msg, n);
    s->error[n] = '\0';
}

// Brings up an offscreen GLES2 context backed by a 1x1 pbuffer, runs `do_work`,
// then tears everything down. `do_work` does the FBO clear + readback.
typedef ReproStatus (*WorkFn)(uint8_t* pixels, int width, int height);

static ReproStatus run_with_gl(uint8_t* pixels, int width, int height, WorkFn do_work) {
    ReproStatus s = {0};

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) { set_err(&s, "eglGetDisplay returned EGL_NO_DISPLAY"); return s; }

    if (!eglInitialize(display, NULL, NULL)) { set_err(&s, "eglInitialize failed"); return s; }

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      0,
        EGL_STENCIL_SIZE,    0,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs = 0;
    if (!eglChooseConfig(display, cfg_attribs, &config, 1, &num_configs) || num_configs < 1) {
        set_err(&s, "eglChooseConfig failed"); eglTerminate(display); return s;
    }

    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (context == EGL_NO_CONTEXT) { set_err(&s, "eglCreateContext failed"); eglTerminate(display); return s; }

    const EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbuf_attribs);
    if (surface == EGL_NO_SURFACE) {
        set_err(&s, "eglCreatePbufferSurface failed");
        eglDestroyContext(display, context); eglTerminate(display); return s;
    }

    if (!eglMakeCurrent(display, surface, surface, context)) {
        set_err(&s, "eglMakeCurrent failed");
        eglDestroySurface(display, surface); eglDestroyContext(display, context); eglTerminate(display); return s;
    }

    const char* vendor   = (const char*)glGetString(GL_VENDOR);
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    const char* version  = (const char*)glGetString(GL_VERSION);
    LOGI("GL_VENDOR='%s' GL_RENDERER='%s' GL_VERSION='%s'",
         vendor ? vendor : "(null)",
         renderer ? renderer : "(null)",
         version ? version : "(null)");

    s = do_work(pixels, width, height);

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return s;
}

// The actual GL work: create an RGBA texture-backed FBO, clear to opaque red,
// glReadPixels the result into `pixels`.
static ReproStatus fbo_clear_readpixels(uint8_t* pixels, int width, int height) {
    ReproStatus s = {0};

    GLuint texture = 0, fbo = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        char buf[128];
        snprintf(buf, sizeof(buf), "FBO incomplete, status=0x%04X", status);
        set_err(&s, buf);
        goto cleanup;
    }

    glViewport(0, 0, width, height);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f); // OPAQUE RED
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    GLenum gl_err = glGetError();
    if (gl_err != GL_NO_ERROR) {
        char buf[128];
        snprintf(buf, sizeof(buf), "glGetError after readback = 0x%04X", gl_err);
        set_err(&s, buf);
        goto cleanup;
    }

    s.success = 1;

cleanup:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (texture) glDeleteTextures(1, &texture);
    return s;
}

ReproStatus repro_variant2_fbo_clear_readpixels(uint8_t* pixels, int width, int height) {
    LOGI("variant2: clear RGBA8 FBO to opaque-red, glReadPixels to malloc buffer (%dx%d)", width, height);
    return run_with_gl(pixels, width, height, fbo_clear_readpixels);
}

ReproStatus repro_variant3_fbo_clear_readpixels_into_bitmap(uint8_t* mapped_pixels, int width, int height) {
    LOGI("variant3: clear RGBA8 FBO to opaque-red, glReadPixels into AndroidBitmap-locked buffer (%dx%d)", width, height);
    return run_with_gl(mapped_pixels, width, height, fbo_clear_readpixels);
}
