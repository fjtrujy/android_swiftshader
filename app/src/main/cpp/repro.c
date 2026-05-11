#include "repro.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#define LOG_TAG "swsrepro"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define SRC_SIZE 256

static void set_err(ReproStatus* s, const char* msg) {
    s->success = 0;
    size_t n = strlen(msg);
    if (n >= sizeof(s->error)) n = sizeof(s->error) - 1;
    memcpy(s->error, msg, n);
    s->error[n] = '\0';
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

// Pass-through textured-quad shaders. Render thread uses these to sample the
// worker-uploaded texture and write to the output FBO.
static const char* VS_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char* FS_SRC =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

// PBO constants — not exposed by the NDK GLES headers we link against.
#define V_GL_PIXEL_UNPACK_BUFFER       0x88EC
#define V_GL_MAP_WRITE_BIT             0x0002
#define V_GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define V_GL_STREAM_DRAW               0x88E0

// --- Worker thread -----------------------------------------------------------
//
// Mirrors `BackgroundGLUploader` in GoodNotes Renderer: opens a shared EGL
// context, uploads a texture via PBO + `glTexSubImage2D`, fences and flushes,
// then releases the context.

struct WorkerShared {
    EGLDisplay display;
    EGLConfig  config;
    EGLContext main_context;

    // Outputs (set by the worker, read by main after pthread_join):
    GLuint    texture;
    GLsync    fence;
    int       ok;
    char      err[128];
};

static void worker_set_err(struct WorkerShared* w, const char* msg) {
    w->ok = 0;
    size_t n = strlen(msg);
    if (n >= sizeof(w->err)) n = sizeof(w->err) - 1;
    memcpy(w->err, msg, n);
    w->err[n] = '\0';
}

static void* worker_main(void* arg) {
    struct WorkerShared* w = (struct WorkerShared*)arg;
    w->ok = 0;
    w->err[0] = '\0';

    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(w->display, w->config, w->main_context, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) { worker_set_err(w, "worker eglCreateContext (shared) failed"); return NULL; }

    const EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(w->display, w->config, pbuf_attribs);
    if (surf == EGL_NO_SURFACE) {
        worker_set_err(w, "worker eglCreatePbufferSurface failed");
        eglDestroyContext(w->display, ctx);
        return NULL;
    }

    if (!eglMakeCurrent(w->display, surf, surf, ctx)) {
        worker_set_err(w, "worker eglMakeCurrent failed");
        eglDestroySurface(w->display, surf);
        eglDestroyContext(w->display, ctx);
        return NULL;
    }

    LOGI("worker: GL_VENDOR='%s' GL_RENDERER='%s'",
         (const char*)glGetString(GL_VENDOR),
         (const char*)glGetString(GL_RENDERER));

    // Allocate a 256x256 RGBA8 texture in the shared object space.
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, SRC_SIZE, SRC_SIZE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // PBO upload: glBufferData → glMapBufferRange → memcpy red → glUnmapBuffer →
    // glTexSubImage2D from PBO offset 0.
    GLuint pbo = 0;
    glGenBuffers(1, &pbo);
    glBindBuffer(V_GL_PIXEL_UNPACK_BUFFER, pbo);
    size_t byteSize = (size_t)SRC_SIZE * (size_t)SRC_SIZE * 4;
    glBufferData(V_GL_PIXEL_UNPACK_BUFFER, byteSize, NULL, V_GL_STREAM_DRAW);
    void* mapped = glMapBufferRange(
        V_GL_PIXEL_UNPACK_BUFFER, 0, byteSize,
        V_GL_MAP_WRITE_BIT | V_GL_MAP_INVALIDATE_BUFFER_BIT
    );
    if (!mapped) {
        worker_set_err(w, "worker glMapBufferRange returned NULL");
        glBindBuffer(V_GL_PIXEL_UNPACK_BUFFER, 0);
        glDeleteBuffers(1, &pbo);
        glDeleteTextures(1, &tex);
        eglMakeCurrent(w->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(w->display, surf);
        eglDestroyContext(w->display, ctx);
        return NULL;
    }
    uint8_t* m = (uint8_t*)mapped;
    for (size_t i = 0; i < byteSize; i += 4) {
        m[i + 0] = 255; m[i + 1] = 0; m[i + 2] = 0; m[i + 3] = 255;
    }
    glUnmapBuffer(V_GL_PIXEL_UNPACK_BUFFER);

    glTexSubImage2D(
        GL_TEXTURE_2D, 0, 0, 0,
        SRC_SIZE, SRC_SIZE,
        GL_RGBA, GL_UNSIGNED_BYTE,
        (const void*)(uintptr_t)0 // offset 0 into bound PBO
    );
    glBindBuffer(V_GL_PIXEL_UNPACK_BUFFER, 0);
    glDeleteBuffers(1, &pbo);

    // Fence the upload so the render thread can `glWaitSync` on it.
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    w->texture = tex;
    w->fence   = fence;
    w->ok = 1;

    // Release the worker's context — a single EGL context can't be current on
    // multiple threads concurrently. Without this the next eglMakeCurrent on
    // the main thread can hit EGL_BAD_ACCESS.
    eglMakeCurrent(w->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(w->display, surf);
    eglDestroyContext(w->display, ctx);
    return NULL;
}

// --- Preamble: create+destroy N EGL contexts -----------------------------
//
// The bug only manifested after the process had created+destroyed many EGL
// contexts in earlier test variants. This preamble approximates that pattern.

struct ChurnArgs { int iterations; int from_worker_thread; };

// Each "cycle" creates a full EGL display+context+pbuffer surface, allocates a
// texture+FBO, compiles a trivial shader, does one draw + readback, then tears
// everything down. Mirrors the per-variant `run_with_gl` pattern on the `main`
// branch. Set `from_worker_thread = 1` to run from a freshly-spawned pthread.
static void* churn_body(void* arg) {
    struct ChurnArgs* a = (struct ChurnArgs*)arg;
    for (int i = 0; i < a->iterations; i++) {
        EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (d == EGL_NO_DISPLAY) continue;
        eglInitialize(d, NULL, NULL);
        const EGLint cfg_attribs[] = {
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        EGLConfig cfg; EGLint n = 0;
        if (!eglChooseConfig(d, cfg_attribs, &cfg, 1, &n) || n < 1) { eglTerminate(d); continue; }
        const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        const EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attribs);
        EGLSurface s = eglCreatePbufferSurface(d, cfg, pbuf_attribs);
        eglMakeCurrent(d, s, s, c);

        // Allocate a texture-FBO, compile a trivial shader, draw, read.
        GLuint tex = 0, fbo = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glViewport(0, 0, 64, 64);
        glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        uint8_t tmp[64 * 64 * 4];
        glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);

        eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(d, s);
        eglDestroyContext(d, c);
        eglTerminate(d);
    }
    return NULL;
}

static void egl_context_churn(int iterations) {
    // Run the churn from a worker thread, matching the renderer's BackgroundGLUploader
    // pattern (and our variant 10's fresh-context pthread). The main thread will pick
    // up after this and run the actual cross-context test.
    struct ChurnArgs a = { iterations, 1 };
    pthread_t t = 0;
    if (pthread_create(&t, NULL, churn_body, &a) != 0) {
        // Fall back to running synchronously on the main thread.
        a.from_worker_thread = 0;
        churn_body(&a);
        return;
    }
    pthread_join(t, NULL);
}

// --- Main thread -------------------------------------------------------------

ReproStatus repro_run(uint8_t* pixels) {
    ReproStatus s = {0};

    // Prime the process state to approximate what 18 prior test variants did.
    // On the `main` branch with those variants in place, this kind of activity
    // had already happened by the time the cross-context test ran — and the bug
    // manifested. Without it, the cross-context test alone passes cleanly on
    // both backends. Establish the minimum preamble needed.
    LOGI("preamble: 18 eglInit/Terminate cycles");
    egl_context_churn(18);

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext main_ctx = EGL_NO_CONTEXT;
    EGLSurface main_surf = EGL_NO_SURFACE;
    GLuint dst_tex = 0, fbo = 0;
    GLuint vs = 0, fs = 0, program = 0, vbo = 0;
    struct WorkerShared shared = {0};
    pthread_t worker = 0;

    // -- Main EGL setup --
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) { set_err(&s, "main eglGetDisplay"); goto cleanup; }
    if (!eglInitialize(display, NULL, NULL)) { set_err(&s, "main eglInitialize"); goto cleanup; }

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs = 0;
    if (!eglChooseConfig(display, cfg_attribs, &config, 1, &num_configs) || num_configs < 1) {
        set_err(&s, "main eglChooseConfig"); goto cleanup;
    }
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    main_ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (main_ctx == EGL_NO_CONTEXT) { set_err(&s, "main eglCreateContext"); goto cleanup; }

    const EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    main_surf = eglCreatePbufferSurface(display, config, pbuf_attribs);
    if (main_surf == EGL_NO_SURFACE) { set_err(&s, "main eglCreatePbufferSurface"); goto cleanup; }

    if (!eglMakeCurrent(display, main_surf, main_surf, main_ctx)) {
        set_err(&s, "main eglMakeCurrent"); goto cleanup;
    }
    LOGI("main: GL_VENDOR='%s' GL_RENDERER='%s' GL_VERSION='%s'",
         (const char*)glGetString(GL_VENDOR),
         (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VERSION));

    // -- Spawn worker thread that uploads the texture and fences --
    shared.display = display;
    shared.config = config;
    shared.main_context = main_ctx;
    if (pthread_create(&worker, NULL, worker_main, &shared) != 0) {
        set_err(&s, "pthread_create failed"); goto cleanup;
    }
    pthread_join(worker, NULL);
    if (!shared.ok) {
        char buf[200];
        snprintf(buf, sizeof(buf), "worker reported error: %s", shared.err);
        set_err(&s, buf); goto cleanup;
    }

    // -- Render thread: wait for the worker's fence, then sample the shared texture --
    glWaitSync(shared.fence, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(shared.fence);
    shared.fence = NULL;

    glGenTextures(1, &dst_tex);
    glBindTexture(GL_TEXTURE_2D, dst_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SWSREPRO_OUTPUT_W, SWSREPRO_OUTPUT_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char buf[64]; snprintf(buf, sizeof(buf), "main FBO incomplete 0x%04X", st);
        set_err(&s, buf); goto cleanup;
    }

    vs = compile_shader(GL_VERTEX_SHADER, VS_SRC, &s); if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, FS_SRC, &s); if (!fs) goto cleanup;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_uv");
    glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = {0}; glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        char buf[256]; snprintf(buf, sizeof(buf), "program link failed: %s", log);
        set_err(&s, buf); goto cleanup;
    }
    glUseProgram(program);

    // Fullscreen-quad VBO (pos + uv interleaved).
    static const GLfloat quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (const void*)(2 * sizeof(GLfloat)));

    // Bind the worker-uploaded texture for sampling.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, shared.texture);
    GLint u_tex = glGetUniformLocation(program, "u_tex");
    glUniform1i(u_tex, 0);

    glViewport(0, 0, SWSREPRO_OUTPUT_W, SWSREPRO_OUTPUT_H);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    glReadPixels(0, 0, SWSREPRO_OUTPUT_W, SWSREPRO_OUTPUT_H,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    GLenum gl_err = glGetError();
    if (gl_err != GL_NO_ERROR) {
        char buf[64]; snprintf(buf, sizeof(buf), "main glGetError after readback 0x%04X", gl_err);
        set_err(&s, buf); goto cleanup;
    }

    s.success = 1;

cleanup:
    if (shared.texture) glDeleteTextures(1, &shared.texture);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (program) glDeleteProgram(program);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (dst_tex) glDeleteTextures(1, &dst_tex);
    if (main_surf != EGL_NO_SURFACE) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, main_surf);
    }
    if (main_ctx != EGL_NO_CONTEXT) eglDestroyContext(display, main_ctx);
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
    return s;
}
