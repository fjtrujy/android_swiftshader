// The actual SwiftShader repro — minimal slice.
//
// Two entry points are invoked from Kotlin, in order:
//   `repro_variant10_offthread_gl`              — Phase 1 (preamble).
//   `repro_variant19_shared_context_upload_and_sample` — Phase 2 (test).
//
// See ../README.md for the full call trace + hypotheses.

#include "repro.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#define LOG_TAG "swsrepro"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

// PBO constants — not exposed by the NDK GLES headers we link against.
#define V_GL_PIXEL_UNPACK_BUFFER       0x88EC
#define V_GL_MAP_WRITE_BIT             0x0002
#define V_GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define V_GL_STREAM_DRAW               0x88E0

// ---------------------------------------------------------------------------
// Local helpers.
// ---------------------------------------------------------------------------

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

static GLuint compile_shader(GLenum type, const char* src, ReproStatus* s) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = {0};
        glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
        char buf[256];
        snprintf(buf, sizeof(buf), "shader compile (type=0x%04X) failed: %s", type, log);
        set_err(s, buf);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

// ---------------------------------------------------------------------------
// Shaders (used by Phase 2).
// ---------------------------------------------------------------------------

static const char* VS_TEXTURED_QUAD_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char* FS_TEXTURED_QUAD_SRC =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

// ---------------------------------------------------------------------------
// Phase 1: variant10 — preamble.
// Bisect step: drop the pbuffer surface — just create + destroy an EGL
// context. If the bug still reproduces, the trigger needs only a transient
// EGL context (no surface, no makeCurrent, no GL work) before Phase 2.
// ---------------------------------------------------------------------------

static ReproStatus v10_bare_egl(void) {
    ReproStatus s = {0};

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) { set_err(&s, "v10 bare eglGetDisplay"); return s; }
    if (!eglInitialize(display, NULL, NULL)) { set_err(&s, "v10 bare eglInitialize"); return s; }

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs = 0;
    if (!eglChooseConfig(display, cfg_attribs, &config, 1, &num_configs) || num_configs < 1) {
        set_err(&s, "v10 bare eglChooseConfig"); eglTerminate(display); return s;
    }
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (context == EGL_NO_CONTEXT) { set_err(&s, "v10 bare eglCreateContext"); eglTerminate(display); return s; }

    LOGI("variant10 bare: created+destroying ctx %p on display %p (no surface, no makeCurrent)",
         (void*)context, (void*)display);

    eglDestroyContext(display, context);
    eglTerminate(display);
    s.success = 1;
    return s;
}

ReproStatus repro_variant10_offthread_gl(uint8_t* pixels, int width, int height) {
    LOGI("variant10 (preamble): main-thread bare-EGL init+teardown (%dx%d)", width, height);
    // Sentinel-fill the pixel buffer so the Kotlin side can still parse a
    // valid summary. The preamble does no GL work.
    memset(pixels, 0, (size_t)width * (size_t)height * 4);
    return v10_bare_egl();
}

// ---------------------------------------------------------------------------
// Phase 2: variant19 — the test.
// Main thread EGL context A; worker thread creates context B with share_context
// = A; uploads a 256x256 RGBA8 texture via PBO + glTexSubImage2D, fences, flushes,
// releases. Main thread glWaitSync's the fence, samples the texture, reads back.
// ---------------------------------------------------------------------------

#define V19_SOURCE_SIZE  256
#define V19_TARGET_W     512
#define V19_TARGET_H     512

struct V19Shared {
    EGLDisplay display;
    EGLConfig  config;
    EGLContext main_context;
    GLuint     texture;     // created by worker, sampled by main.
    GLsync     fence;       // signalled by worker, waited by main.
    int        worker_ok;
    char       worker_err[128];
};

static void* v19_worker(void* arg) {
    struct V19Shared* shared = (struct V19Shared*)arg;
    shared->worker_ok = 0;
    shared->worker_err[0] = '\0';

    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext worker_context = eglCreateContext(shared->display, shared->config,
                                                  shared->main_context, ctx_attribs);
    if (worker_context == EGL_NO_CONTEXT) {
        snprintf(shared->worker_err, sizeof(shared->worker_err),
                 "v19 worker eglCreateContext (shared) failed, eglError=0x%04X", eglGetError());
        return NULL;
    }
    const EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface worker_surface = eglCreatePbufferSurface(shared->display, shared->config, pbuf_attribs);
    if (worker_surface == EGL_NO_SURFACE) {
        snprintf(shared->worker_err, sizeof(shared->worker_err), "v19 worker pbuffer failed");
        eglDestroyContext(shared->display, worker_context); return NULL;
    }
    if (!eglMakeCurrent(shared->display, worker_surface, worker_surface, worker_context)) {
        snprintf(shared->worker_err, sizeof(shared->worker_err), "v19 worker makeCurrent failed");
        eglDestroySurface(shared->display, worker_surface);
        eglDestroyContext(shared->display, worker_context); return NULL;
    }

    // Allocate the shared texture in the share group.
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, V19_SOURCE_SIZE, V19_SOURCE_SIZE);

    // PBO-backed upload: map/memcpy red/unmap, then glTexSubImage2D from PBO offset 0.
    GLuint pbo = 0;
    glGenBuffers(1, &pbo);
    glBindBuffer(V_GL_PIXEL_UNPACK_BUFFER, pbo);
    size_t byteSize = (size_t)V19_SOURCE_SIZE * (size_t)V19_SOURCE_SIZE * 4;
    glBufferData(V_GL_PIXEL_UNPACK_BUFFER, byteSize, NULL, V_GL_STREAM_DRAW);
    void* mapped = glMapBufferRange(V_GL_PIXEL_UNPACK_BUFFER, 0, byteSize,
                                     V_GL_MAP_WRITE_BIT | V_GL_MAP_INVALIDATE_BUFFER_BIT);
    if (!mapped) {
        snprintf(shared->worker_err, sizeof(shared->worker_err), "v19 worker glMapBufferRange returned NULL");
        glBindBuffer(V_GL_PIXEL_UNPACK_BUFFER, 0);
        glDeleteBuffers(1, &pbo); glDeleteTextures(1, &tex);
        eglMakeCurrent(shared->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(shared->display, worker_surface);
        eglDestroyContext(shared->display, worker_context);
        return NULL;
    }
    uint8_t* m = (uint8_t*)mapped;
    for (size_t i = 0; i < byteSize; i += 4) {
        m[i+0] = 255; m[i+1] = 0; m[i+2] = 0; m[i+3] = 255;
    }
    glUnmapBuffer(V_GL_PIXEL_UNPACK_BUFFER);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, V19_SOURCE_SIZE, V19_SOURCE_SIZE,
                    GL_RGBA, GL_UNSIGNED_BYTE, (const void*)(uintptr_t)0);
    glBindBuffer(V_GL_PIXEL_UNPACK_BUFFER, 0);
    glDeleteBuffers(1, &pbo);

    // Fence and flush, hand off to main.
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    shared->texture = tex;
    shared->fence   = fence;
    shared->worker_ok = 1;

    // Release the worker's hold on its context — only one thread can hold an
    // EGL context current at once.
    eglMakeCurrent(shared->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(shared->display, worker_surface);
    eglDestroyContext(shared->display, worker_context);
    return NULL;
}

static ReproStatus v19_main_path(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    ReproStatus s = {0};
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext main_context = EGL_NO_CONTEXT;
    EGLSurface main_surface = EGL_NO_SURFACE;
    GLuint dst_tex = 0, fbo = 0, vs = 0, fs = 0, program = 0, vbo = 0;
    pthread_t worker_thread = 0;
    struct V19Shared shared = {0};

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) { set_err(&s, "v19 main eglGetDisplay"); goto cleanup; }
    if (!eglInitialize(display, NULL, NULL)) { set_err(&s, "v19 main eglInitialize"); goto cleanup; }

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs = 0;
    if (!eglChooseConfig(display, cfg_attribs, &config, 1, &num_configs) || num_configs < 1) {
        set_err(&s, "v19 main eglChooseConfig"); goto cleanup;
    }
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    main_context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (main_context == EGL_NO_CONTEXT) { set_err(&s, "v19 main eglCreateContext"); goto cleanup; }
    const EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    main_surface = eglCreatePbufferSurface(display, config, pbuf_attribs);
    if (main_surface == EGL_NO_SURFACE) { set_err(&s, "v19 main pbuffer"); goto cleanup; }
    if (!eglMakeCurrent(display, main_surface, main_surface, main_context)) {
        set_err(&s, "v19 main makeCurrent"); goto cleanup;
    }

    shared.display      = display;
    shared.config       = config;
    shared.main_context = main_context;
    if (pthread_create(&worker_thread, NULL, v19_worker, &shared) != 0) {
        set_err(&s, "v19 pthread_create failed"); goto cleanup;
    }
    pthread_join(worker_thread, NULL);
    if (!shared.worker_ok) {
        char buf[200];
        snprintf(buf, sizeof(buf), "v19 worker failed: %s", shared.worker_err);
        set_err(&s, buf); goto cleanup;
    }

    // Wait on the worker's fence (GPU-side), then sample the shared texture.
    glWaitSync(shared.fence, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(shared.fence);
    shared.fence = NULL;

    glGenTextures(1, &dst_tex);
    glBindTexture(GL_TEXTURE_2D, dst_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, V19_TARGET_W, V19_TARGET_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        set_err(&s, "v19 main FBO incomplete"); goto cleanup;
    }

    vs = compile_shader(GL_VERTEX_SHADER, VS_TEXTURED_QUAD_SRC, &s); if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, FS_TEXTURED_QUAD_SRC, &s); if (!fs) goto cleanup;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_uv");
    glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { set_err(&s, "v19 link failed"); goto cleanup; }
    glUseProgram(program);

    static const GLfloat quad[] = {
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f,
    };
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (const void*)(2 * sizeof(GLfloat)));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, shared.texture);
    GLint u_tex = glGetUniformLocation(program, "u_tex");
    glUniform1i(u_tex, 0);

    glViewport(0, 0, V19_TARGET_W, V19_TARGET_H);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    glReadPixels(0, 0, V19_TARGET_W, V19_TARGET_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "v19 readback")) goto cleanup;
    s.success = 1;

cleanup:
    if (shared.texture) glDeleteTextures(1, &shared.texture);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (program) glDeleteProgram(program);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (dst_tex) glDeleteTextures(1, &dst_tex);
    if (main_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, main_surface);
    }
    if (main_context != EGL_NO_CONTEXT) eglDestroyContext(display, main_context);
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
    return s;
}

ReproStatus repro_variant19_shared_context_upload_and_sample(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    LOGI("variant19 (test): shared-context worker uploads via PBO+fence; main glWaitSync + samples");
    return v19_main_path(pixels, V19_TARGET_W, V19_TARGET_H);
}
