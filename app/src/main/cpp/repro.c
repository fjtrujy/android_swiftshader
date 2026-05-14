// Tiny valid GLES3 repro for the Android emulator direct SwiftShader path.
//
// The draw is deliberately boring: make a 256x256 EGL pbuffer current,
// generate a full-screen triangle strip from gl_VertexID, output opaque red,
// and read exact pixels back from the default framebuffer.

#include "repro.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <android/log.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "swsrepro"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define SIZE 256
typedef struct {
    uint8_t r, g, b, a;
} Pixel;

static const char* VERTEX_SHADER =
    "#version 300 es\n"
    "precision highp float;\n"
    "const vec2 kPosition[4] = vec2[4](\n"
    "  vec2(-1.0, -1.0),\n"
    "  vec2( 1.0, -1.0),\n"
    "  vec2(-1.0,  1.0),\n"
    "  vec2( 1.0,  1.0)\n"
    ");\n"
    "void main() {\n"
    "  gl_Position = vec4(kPosition[gl_VertexID], 0.0, 1.0);\n"
    "}\n";

static const char* FRAGMENT_SHADER =
    "#version 300 es\n"
    "precision highp float;\n"
    "out vec4 fColor;\n"
    "void main() { fColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";

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

static int check_fbo(ReproStatus* s, const char* where) {
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE) return 1;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s: framebuffer status = 0x%04X", where, status);
    set_err(s, buf);
    return 0;
}

static void log_egl_config(EGLDisplay display, EGLConfig config) {
    EGLint surface_type = 0;
    EGLint renderable_type = 0;
    EGLint conformant = 0;
    EGLint red = 0, green = 0, blue = 0, alpha = 0;
    EGLint sample_buffers = 0, samples = 0;

    eglGetConfigAttrib(display, config, EGL_SURFACE_TYPE, &surface_type);
    eglGetConfigAttrib(display, config, EGL_RENDERABLE_TYPE, &renderable_type);
    eglGetConfigAttrib(display, config, EGL_CONFORMANT, &conformant);
    eglGetConfigAttrib(display, config, EGL_RED_SIZE, &red);
    eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &green);
    eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &blue);
    eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &alpha);
    eglGetConfigAttrib(display, config, EGL_SAMPLE_BUFFERS, &sample_buffers);
    eglGetConfigAttrib(display, config, EGL_SAMPLES, &samples);

    LOGI("EGL config: surface=0x%04X renderable=0x%04X conformant=0x%04X "
         "rgba=(%d,%d,%d,%d) sampleBuffers=%d samples=%d",
         surface_type, renderable_type, conformant,
         red, green, blue, alpha, sample_buffers, samples);
}

static int configure_default_framebuffer(ReproStatus* s, const char* where) {
    const GLenum back = GL_BACK;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!check_fbo(s, where)) return 0;

    glDrawBuffers(1, &back);
    glReadBuffer(GL_BACK);
    glViewport(0, 0, SIZE, SIZE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DITHER);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    return check_gl(s, where);
}

static GLuint compile_shader(GLenum type, const char* source, ReproStatus* s) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return shader;

    char log[512] = {0};
    glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
    char buf[768];
    snprintf(buf, sizeof(buf), "shader compile (type=0x%04X) failed: %s", type, log);
    set_err(s, buf);
    glDeleteShader(shader);
    return 0;
}

static GLuint create_program(ReproStatus* s) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SHADER, s);
    if (!vs) return 0;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER, s);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok) return program;

    char log[512] = {0};
    glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
    char buf[768];
    snprintf(buf, sizeof(buf), "program link failed: %s", log);
    set_err(s, buf);
    glDeleteProgram(program);
    return 0;
}

static Pixel pixel_at(const uint8_t* pixels, int x, int y) {
    size_t offset = ((size_t)y * SIZE + (size_t)x) * 4;
    Pixel p = {
        pixels[offset + 0],
        pixels[offset + 1],
        pixels[offset + 2],
        pixels[offset + 3],
    };
    return p;
}

static int validate_red(ReproStatus* s, const uint8_t* pixels) {
    Pixel first = pixel_at(pixels, 0, 0);
    Pixel center = pixel_at(pixels, SIZE / 2, SIZE / 2);
    Pixel last = pixel_at(pixels, SIZE - 1, SIZE - 1);
    LOGI("readback: first=(%u,%u,%u,%u) center=(%u,%u,%u,%u) last=(%u,%u,%u,%u)",
         first.r, first.g, first.b, first.a,
         center.r, center.g, center.b, center.a,
         last.r, last.g, last.b, last.a);

    if (first.r == 255 && first.g == 0 && first.b == 0 && first.a == 255 &&
        center.r == 255 && center.g == 0 && center.b == 0 && center.a == 255 &&
        last.r == 255 && last.g == 0 && last.b == 0 && last.a == 255) {
        return 1;
    }

    char buf[256];
    snprintf(
        buf, sizeof(buf),
        "readback mismatch: first=(%u,%u,%u,%u), center=(%u,%u,%u,%u), last=(%u,%u,%u,%u)",
        first.r, first.g, first.b, first.a,
        center.r, center.g, center.b, center.a,
        last.r, last.g, last.b, last.a
    );
    set_err(s, buf);
    return 0;
}

static int draw_red_quad(ReproStatus* s) {
    GLuint program = 0;
    GLuint vao = 0;

    program = create_program(s);
    if (!program) return 0;
    glUseProgram(program);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    if (!configure_default_framebuffer(s, "draw default framebuffer")) goto fail;
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (!check_gl(s, "glDrawArrays")) goto fail;

    glBindVertexArray(0);
    glUseProgram(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    return 1;

fail:
    glBindVertexArray(0);
    glUseProgram(0);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (program) glDeleteProgram(program);
    return 0;
}

ReproStatus repro_run_test(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    LOGI("repro_run_test: EGL pbuffer default framebuffer + gl_VertexID quad");

    ReproStatus s = {0};
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) { set_err(&s, "eglGetDisplay"); goto cleanup; }
    if (!eglInitialize(display, NULL, NULL)) { set_err(&s, "eglInitialize"); goto cleanup; }

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_CONFORMANT,      EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_SAMPLE_BUFFERS, 0,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs = 0;
    if (!eglChooseConfig(display, cfg_attribs, &config, 1, &num_configs) || num_configs < 1) {
        set_err(&s, "eglChooseConfig"); goto cleanup;
    }
    log_egl_config(display, config);

    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (context == EGL_NO_CONTEXT) { set_err(&s, "eglCreateContext"); goto cleanup; }

    const EGLint pbuf_attribs[] = { EGL_WIDTH, SIZE, EGL_HEIGHT, SIZE, EGL_NONE };
    surface = eglCreatePbufferSurface(display, config, pbuf_attribs);
    if (surface == EGL_NO_SURFACE) { set_err(&s, "eglCreatePbufferSurface"); goto cleanup; }

    if (!eglMakeCurrent(display, surface, surface, context)) {
        set_err(&s, "eglMakeCurrent"); goto cleanup;
    }

    LOGI("GL_VENDOR='%s' GL_RENDERER='%s' GL_VERSION='%s'",
         (const char*)glGetString(GL_VENDOR),
         (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VERSION));

    if (!configure_default_framebuffer(&s, "clear default framebuffer")) goto cleanup;
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!check_gl(&s, "transparent clear")) goto cleanup;

    if (!draw_red_quad(&s)) goto cleanup;
    glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "glReadPixels")) goto cleanup;
    if (!validate_red(&s, pixels)) goto cleanup;

    s.success = 1;

cleanup:
    if (surface != EGL_NO_SURFACE) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
    }
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
    return s;
}
