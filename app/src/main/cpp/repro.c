// SwiftShader Android-emulator render bug — Goodnotes snapshot readback probe.
//
// This variant intentionally avoids the already-known immutable-texture
// sampler bug by setting a non-mipmap min filter. It instead mirrors the
// Android snapshot extraction path from Goodnotes:
//
//   1. Create a GLES3 pbuffer context.
//   2. Allocate a single-level immutable RGBA8 texture with glTexStorage2D.
//   3. Attach it to an FBO and clear four opaque color quadrants into it.
//   4. Attach that texture as READ_FRAMEBUFFER, blit into a temporary RGBA8
//      renderbuffer using swapped source Y coordinates, then glReadPixels.
//
// If this comes back transparent black on direct `-gpu swiftshader`, the
// empty Goodnotes Android recordings are likely in SwiftShader's FBO blit or
// readback path rather than in source texture sampling.

#include "repro.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "swsrepro"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define SIZE 256

typedef struct {
    uint8_t r, g, b, a;
} Pixel;

static void set_err(ReproStatus* s, const char* msg) {
    s->success = 0;
    size_t n = strlen(msg);
    if (n >= sizeof(s->error)) n = sizeof(s->error) - 1;
    memcpy(s->error, msg, n);
    s->error[n] = '\0';
}

static int check_gl(ReproStatus* s, const char* where) {
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) return 1;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s: glGetError = 0x%04X", where, err);
    set_err(s, buf);
    return 0;
}

static int check_fbo(ReproStatus* s, GLenum target, const char* where) {
    GLenum status = glCheckFramebufferStatus(target);
    if (status == GL_FRAMEBUFFER_COMPLETE) return 1;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s: framebuffer status = 0x%04X", where, status);
    set_err(s, buf);
    return 0;
}

static Pixel pixel_at(const uint8_t* pixels, int width, int x, int y) {
    size_t offset = ((size_t)y * (size_t)width + (size_t)x) * 4;
    Pixel p = {
        pixels[offset + 0],
        pixels[offset + 1],
        pixels[offset + 2],
        pixels[offset + 3],
    };
    return p;
}

static void log_samples(const char* label, const uint8_t* pixels, int width, int height) {
    Pixel bl = pixel_at(pixels, width, 0, 0);
    Pixel br = pixel_at(pixels, width, width - 1, 0);
    Pixel tl = pixel_at(pixels, width, 0, height - 1);
    Pixel tr = pixel_at(pixels, width, width - 1, height - 1);
    Pixel c = pixel_at(pixels, width, width / 2, height / 2);
    LOGI("%s: row0-left=(%u,%u,%u,%u) row0-right=(%u,%u,%u,%u) "
         "lastrow-left=(%u,%u,%u,%u) lastrow-right=(%u,%u,%u,%u) "
         "center=(%u,%u,%u,%u)",
         label,
         bl.r, bl.g, bl.b, bl.a,
         br.r, br.g, br.b, br.a,
         tl.r, tl.g, tl.b, tl.a,
         tr.r, tr.g, tr.b, tr.a,
         c.r, c.g, c.b, c.a);
}

static void clear_rect(GLint x, GLint y, GLsizei width, GLsizei height,
                       GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    glScissor(x, y, width, height);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void fill_quadrants(GLuint framebuffer) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, SIZE, SIZE);
    glEnable(GL_SCISSOR_TEST);

    const GLsizei half = SIZE / 2;
    clear_rect(0, 0, half, half, 1.f, 0.f, 0.f, 1.f);       // GL bottom-left: red
    clear_rect(half, 0, half, half, 0.f, 1.f, 0.f, 1.f);    // GL bottom-right: green
    clear_rect(0, half, half, half, 0.f, 0.f, 1.f, 1.f);    // GL top-left: blue
    clear_rect(half, half, half, half, 1.f, 1.f, 1.f, 1.f); // GL top-right: white

    glDisable(GL_SCISSOR_TEST);
}

static int read_source_direct(ReproStatus* s, GLuint source_fbo) {
    uint8_t* direct = (uint8_t*)malloc((size_t)SIZE * (size_t)SIZE * 4);
    if (!direct) {
        set_err(s, "malloc direct buffer");
        return 0;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, source_fbo);
    if (!check_fbo(s, GL_READ_FRAMEBUFFER, "direct source read FBO")) {
        free(direct);
        return 0;
    }
    glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, direct);
    if (!check_gl(s, "direct source glReadPixels")) {
        free(direct);
        return 0;
    }
    log_samples("direct source texture readback", direct, SIZE, SIZE);
    free(direct);
    return 1;
}

static int blit_then_read(ReproStatus* s, GLuint source_fbo, uint8_t* pixels, int flip_y) {
    GLuint dst_rbo = 0;
    GLuint dst_fbo = 0;

    glGenRenderbuffers(1, &dst_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, dst_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, SIZE, SIZE);

    glGenFramebuffers(1, &dst_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    glFramebufferRenderbuffer(
        GL_DRAW_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_RENDERBUFFER,
        dst_rbo
    );
    if (!check_fbo(s, GL_DRAW_FRAMEBUFFER, flip_y ? "flipped blit draw FBO" : "plain blit draw FBO")) {
        goto fail;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, source_fbo);
    if (!check_fbo(s, GL_READ_FRAMEBUFFER, flip_y ? "flipped blit read FBO" : "plain blit read FBO")) {
        goto fail;
    }

    if (flip_y) {
        glBlitFramebuffer(
            0, SIZE, SIZE, 0,
            0, 0, SIZE, SIZE,
            GL_COLOR_BUFFER_BIT,
            GL_NEAREST
        );
    } else {
        glBlitFramebuffer(
            0, 0, SIZE, SIZE,
            0, 0, SIZE, SIZE,
            GL_COLOR_BUFFER_BIT,
            GL_NEAREST
        );
    }
    if (!check_gl(s, flip_y ? "flipped glBlitFramebuffer" : "plain glBlitFramebuffer")) {
        goto fail;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, dst_fbo);
    if (!check_fbo(s, GL_READ_FRAMEBUFFER, flip_y ? "flipped readback FBO" : "plain readback FBO")) {
        goto fail;
    }
    glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(s, flip_y ? "flipped glReadPixels" : "plain glReadPixels")) {
        goto fail;
    }

    log_samples(flip_y ? "flipped blit readback" : "plain blit readback", pixels, SIZE, SIZE);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glDeleteFramebuffers(1, &dst_fbo);
    glDeleteRenderbuffers(1, &dst_rbo);
    return 1;

fail:
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (dst_fbo) glDeleteFramebuffers(1, &dst_fbo);
    if (dst_rbo) glDeleteRenderbuffers(1, &dst_rbo);
    return 0;
}

ReproStatus repro_run_test(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    LOGI("repro_run_test: immutable render target + FBO quadrant clears + Goodnotes-style flipped glBlitFramebuffer readback");

    ReproStatus s = {0};
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint render_tex = 0, source_fbo = 0;

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) { set_err(&s, "eglGetDisplay"); goto cleanup; }
    if (!eglInitialize(display, NULL, NULL)) { set_err(&s, "eglInitialize"); goto cleanup; }

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs = 0;
    if (!eglChooseConfig(display, cfg_attribs, &config, 1, &num_configs) || num_configs < 1) {
        set_err(&s, "eglChooseConfig"); goto cleanup;
    }
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (context == EGL_NO_CONTEXT) { set_err(&s, "eglCreateContext"); goto cleanup; }
    const EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    surface = eglCreatePbufferSurface(display, config, pbuf_attribs);
    if (surface == EGL_NO_SURFACE) { set_err(&s, "eglCreatePbufferSurface"); goto cleanup; }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        set_err(&s, "eglMakeCurrent"); goto cleanup;
    }

    LOGI("GL_VENDOR='%s' GL_RENDERER='%s' GL_VERSION='%s'",
         (const char*)glGetString(GL_VENDOR),
         (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VERSION));

    glGenTextures(1, &render_tex);
    glBindTexture(GL_TEXTURE_2D, render_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, SIZE, SIZE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (!check_gl(&s, "render texture setup")) goto cleanup;

    glGenFramebuffers(1, &source_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, source_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, render_tex, 0);
    if (!check_fbo(&s, GL_FRAMEBUFFER, "source render texture FBO")) goto cleanup;

    fill_quadrants(source_fbo);
    if (!check_gl(&s, "quadrant clears")) goto cleanup;

    if (!read_source_direct(&s, source_fbo)) goto cleanup;

    {
        uint8_t* plain = (uint8_t*)malloc((size_t)SIZE * (size_t)SIZE * 4);
        if (!plain) { set_err(&s, "malloc plain blit buffer"); goto cleanup; }
        int ok = blit_then_read(&s, source_fbo, plain, 0);
        free(plain);
        if (!ok) goto cleanup;
    }

    if (!blit_then_read(&s, source_fbo, pixels, 1)) goto cleanup;

    glFinish();
    s.success = 1;

cleanup:
    if (source_fbo) glDeleteFramebuffers(1, &source_fbo);
    if (render_tex) glDeleteTextures(1, &render_tex);
    if (surface != EGL_NO_SURFACE) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
    }
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
    return s;
}
