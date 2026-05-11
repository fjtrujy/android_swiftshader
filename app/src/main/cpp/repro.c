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

// PBO constants — not exposed by the NDK GLES2 headers.
#define V_GL_PIXEL_UNPACK_BUFFER       0x88EC
#define V_GL_MAP_WRITE_BIT             0x0002
#define V_GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define V_GL_STREAM_DRAW               0x88E0

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

// =============================================================================
// PREAMBLE: variant 10's body — off-thread fresh-context GLES2 draw.
// =============================================================================

static const char* PREAMBLE_VS_SRC =
    "attribute vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char* PREAMBLE_FS_SRC =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main() { gl_FragColor = u_color; }\n";

// Allocates a 512x512 RGBA8 FBO, compiles+links a uniform-color shader, draws a
// centered quad in NDC [-0.5, 0.5] with `.copy` blend, reads back. Runs inside
// the worker thread's freshly-created GLES2 context.
static ReproStatus preamble_gl_work(uint8_t* pixels, int width, int height) {
    ReproStatus s = {0};
    GLuint texture = 0, fbo = 0, vs = 0, fs = 0, program = 0, vbo = 0;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        set_err(&s, "preamble FBO incomplete"); goto cleanup;
    }

    vs = compile_shader(GL_VERTEX_SHADER, PREAMBLE_VS_SRC, &s); if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, PREAMBLE_FS_SRC, &s); if (!fs) goto cleanup;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { set_err(&s, "preamble link failed"); goto cleanup; }
    glUseProgram(program);
    GLint u_color = glGetUniformLocation(program, "u_color");

    // Quad covering NDC [-0.5, 0.5] (centered).
    static const GLfloat quad[] = {
        -0.5f, -0.5f,
         0.5f, -0.5f,
        -0.5f,  0.5f,
         0.5f,  0.5f,
    };
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);

    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO); // `.copy` blend
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform4f(u_color, 1.0f, 0.0f, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    s.success = 1;

cleanup:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (program) glDeleteProgram(program);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (texture) glDeleteTextures(1, &texture);
    return s;
}

struct PreambleArgs {
    ReproStatus result;
};

static void* preamble_worker(void* arg) {
    struct PreambleArgs* p = (struct PreambleArgs*)arg;

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        set_err(&p->result, "preamble eglGetDisplay returned EGL_NO_DISPLAY");
        return NULL;
    }
    if (!eglInitialize(display, NULL, NULL)) {
        set_err(&p->result, "preamble eglInitialize failed");
        return NULL;
    }
    // GLES2 (CLIENT_VERSION=2). This matters: variant 10 used GLES2.
    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    EGLConfig config; EGLint n = 0;
    if (!eglChooseConfig(display, cfg_attribs, &config, 1, &n) || n < 1) {
        set_err(&p->result, "preamble eglChooseConfig failed");
        eglTerminate(display); return NULL;
    }
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    // FRESH context (NOT shared with anything).
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (context == EGL_NO_CONTEXT) {
        set_err(&p->result, "preamble eglCreateContext failed");
        eglTerminate(display); return NULL;
    }
    const EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbuf_attribs);
    if (surface == EGL_NO_SURFACE) {
        set_err(&p->result, "preamble eglCreatePbufferSurface failed");
        eglDestroyContext(display, context); eglTerminate(display); return NULL;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        set_err(&p->result, "preamble eglMakeCurrent failed");
        eglDestroySurface(display, surface); eglDestroyContext(display, context);
        eglTerminate(display); return NULL;
    }

    LOGI("preamble worker: GL_VENDOR='%s' GL_RENDERER='%s'",
         (const char*)glGetString(GL_VENDOR),
         (const char*)glGetString(GL_RENDERER));

    // Heap-allocated (don't put 1 MB on the worker pthread's stack).
    uint8_t* scratch = malloc(512 * 512 * 4);
    if (!scratch) {
        set_err(&p->result, "preamble worker: malloc(512*512*4) failed");
    } else {
        p->result = preamble_gl_work(scratch, 512, 512);
        free(scratch);
    }

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return NULL;
}

ReproStatus repro_preamble_offthread_fresh_context(void) {
    LOGI("preamble: spawning worker thread for fresh-context GLES2 draw");
    struct PreambleArgs args = {0};
    pthread_t thread;
    if (pthread_create(&thread, NULL, preamble_worker, &args) != 0) {
        ReproStatus s; set_err(&s, "preamble pthread_create failed"); return s;
    }
    pthread_join(thread, NULL);
    return args.result;
}

// =============================================================================
// TEST: shared-EGL-context cross-thread upload + main-thread sample.
// =============================================================================

static const char* TEST_VS_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char* TEST_FS_SRC =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

#define TEST_SRC_SIZE 256

struct TestShared {
    EGLDisplay display;
    EGLConfig  config;
    EGLContext main_context;

    GLuint    texture;
    GLsync    fence;
    int       ok;
    char      err[128];
};

static void test_worker_set_err(struct TestShared* w, const char* msg) {
    w->ok = 0;
    size_t n = strlen(msg);
    if (n >= sizeof(w->err)) n = sizeof(w->err) - 1;
    memcpy(w->err, msg, n);
    w->err[n] = '\0';
}

static void* test_worker(void* arg) {
    struct TestShared* w = (struct TestShared*)arg;
    w->ok = 0; w->err[0] = '\0';

    // SHARED context: passes the main thread's context as the share_context.
    // Server-side GL objects (textures, fence sync) are visible across the share group.
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(w->display, w->config, w->main_context, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) { test_worker_set_err(w, "test worker eglCreateContext (shared) failed"); return NULL; }

    const EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(w->display, w->config, pbuf_attribs);
    if (surf == EGL_NO_SURFACE) {
        test_worker_set_err(w, "test worker eglCreatePbufferSurface failed");
        eglDestroyContext(w->display, ctx); return NULL;
    }
    if (!eglMakeCurrent(w->display, surf, surf, ctx)) {
        test_worker_set_err(w, "test worker eglMakeCurrent failed");
        eglDestroySurface(w->display, surf); eglDestroyContext(w->display, ctx);
        return NULL;
    }

    LOGI("test worker: GL_VENDOR='%s' GL_RENDERER='%s'",
         (const char*)glGetString(GL_VENDOR),
         (const char*)glGetString(GL_RENDERER));

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, TEST_SRC_SIZE, TEST_SRC_SIZE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // PBO upload: glBufferData → glMapBufferRange → memcpy red → glUnmapBuffer →
    // glTexSubImage2D from PBO offset 0.
    GLuint pbo = 0;
    glGenBuffers(1, &pbo);
    glBindBuffer(V_GL_PIXEL_UNPACK_BUFFER, pbo);
    size_t byteSize = (size_t)TEST_SRC_SIZE * (size_t)TEST_SRC_SIZE * 4;
    glBufferData(V_GL_PIXEL_UNPACK_BUFFER, byteSize, NULL, V_GL_STREAM_DRAW);
    void* mapped = glMapBufferRange(
        V_GL_PIXEL_UNPACK_BUFFER, 0, byteSize,
        V_GL_MAP_WRITE_BIT | V_GL_MAP_INVALIDATE_BUFFER_BIT
    );
    if (!mapped) {
        test_worker_set_err(w, "test worker glMapBufferRange returned NULL");
        glBindBuffer(V_GL_PIXEL_UNPACK_BUFFER, 0);
        glDeleteBuffers(1, &pbo); glDeleteTextures(1, &tex);
        eglMakeCurrent(w->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(w->display, surf); eglDestroyContext(w->display, ctx);
        return NULL;
    }
    uint8_t* m = (uint8_t*)mapped;
    for (size_t i = 0; i < byteSize; i += 4) {
        m[i+0] = 255; m[i+1] = 0; m[i+2] = 0; m[i+3] = 255;
    }
    glUnmapBuffer(V_GL_PIXEL_UNPACK_BUFFER);

    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    TEST_SRC_SIZE, TEST_SRC_SIZE,
                    GL_RGBA, GL_UNSIGNED_BYTE,
                    (const void*)(uintptr_t)0); // offset 0 into bound PBO
    glBindBuffer(V_GL_PIXEL_UNPACK_BUFFER, 0);
    glDeleteBuffers(1, &pbo);

    // Fence the upload, flush so it's submitted to the GPU command queue.
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    w->texture = tex;
    w->fence   = fence;
    w->ok      = 1;

    // Release this thread's hold on the context.
    eglMakeCurrent(w->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(w->display, surf);
    eglDestroyContext(w->display, ctx);
    return NULL;
}

ReproStatus repro_test_shared_context_upload(uint8_t* pixels) {
    ReproStatus s = {0};

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext main_ctx = EGL_NO_CONTEXT;
    EGLSurface main_surf = EGL_NO_SURFACE;
    GLuint dst_tex = 0, fbo = 0, vs = 0, fs = 0, program = 0, vbo = 0;
    struct TestShared shared = {0};
    pthread_t worker = 0;

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) { set_err(&s, "test main eglGetDisplay"); goto cleanup; }
    if (!eglInitialize(display, NULL, NULL)) { set_err(&s, "test main eglInitialize"); goto cleanup; }

    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config; EGLint num_configs = 0;
    if (!eglChooseConfig(display, cfg_attribs, &config, 1, &num_configs) || num_configs < 1) {
        set_err(&s, "test main eglChooseConfig"); goto cleanup;
    }

    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    main_ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (main_ctx == EGL_NO_CONTEXT) { set_err(&s, "test main eglCreateContext"); goto cleanup; }

    const EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    main_surf = eglCreatePbufferSurface(display, config, pbuf_attribs);
    if (main_surf == EGL_NO_SURFACE) { set_err(&s, "test main eglCreatePbufferSurface"); goto cleanup; }

    if (!eglMakeCurrent(display, main_surf, main_surf, main_ctx)) {
        set_err(&s, "test main eglMakeCurrent"); goto cleanup;
    }
    LOGI("test main: GL_VENDOR='%s' GL_RENDERER='%s' GL_VERSION='%s'",
         (const char*)glGetString(GL_VENDOR),
         (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VERSION));

    shared.display = display;
    shared.config = config;
    shared.main_context = main_ctx;
    if (pthread_create(&worker, NULL, test_worker, &shared) != 0) {
        set_err(&s, "test pthread_create failed"); goto cleanup;
    }
    pthread_join(worker, NULL);
    if (!shared.ok) {
        char buf[200];
        snprintf(buf, sizeof(buf), "test worker reported error: %s", shared.err);
        set_err(&s, buf); goto cleanup;
    }

    // Wait on the worker's fence (GPU-side), then sample the shared texture.
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
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        set_err(&s, "test main FBO incomplete"); goto cleanup;
    }

    vs = compile_shader(GL_VERTEX_SHADER, TEST_VS_SRC, &s); if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, TEST_FS_SRC, &s); if (!fs) goto cleanup;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_uv");
    glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { set_err(&s, "test main link failed"); goto cleanup; }
    glUseProgram(program);

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
        char buf[64]; snprintf(buf, sizeof(buf), "test main glGetError 0x%04X", gl_err);
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
