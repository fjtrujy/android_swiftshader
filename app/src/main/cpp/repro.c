// SwiftShader Android-emulator render bug — minimal repro.
//
// Sequence (entirely on the main thread):
//   1. Bring up GLES3 + 1x1 pbuffer surface.
//   2. Create an IMMUTABLE 256x256 RGBA8 source texture via glTexStorage2D
//      with 1 mipmap level. Upload opaque red via glTexSubImage2D from a
//      CPU buffer. Do NOT call glTexParameteri — leave the default
//      min filter (GL_NEAREST_MIPMAP_LINEAR).
//   3. Create a 256x256 RGBA8 destination texture + FBO. Clear to a
//      sentinel BLUE so "draw didn't run" is distinguishable from
//      "sample returned zero".
//   4. Compile + link a textured-quad shader; draw a fullscreen quad
//      sampling the source texture into the FBO.
//   5. glReadPixels back.
//
// Expected output per GLES3 spec section 8.17 (immutable textures are
// always complete): every pixel `(255, 0, 0, 255)`.
//
// Actual:
//   - `-gpu swangle` (ANGLE → Vulkan → SwiftShader-Vulkan): red ✅.
//   - `-gpu swiftshader`: `(0, 0, 0, 0)` ❌. Two spec violations stacked:
//     a) SwiftShader treats the 1-level immutable texture as incomplete
//        because the default min filter is mipmap-aware. GLES3 §8.17
//        says immutable textures are always complete.
//     b) Sampling an incomplete texture returns (0, 0, 0, 0). GLES3
//        §8.17 requires (0, 0, 0, 1) for float texture types.
//
// Client workaround: call
//   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
// (or any non-mipmap-aware filter) after creating the source texture.

#include "repro.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#define LOG_TAG "swsrepro"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define SIZE 256

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

static const char* VS_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char* FS_SRC =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

ReproStatus repro_run_test(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    LOGI("repro_run_test: glTexStorage2D (immutable, 1 level) + glTexSubImage2D; default mipmap min filter");

    ReproStatus s = {0};
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint src_tex = 0, dst_tex = 0, fbo = 0, vs = 0, fs = 0, program = 0, vbo = 0;

    // EGL bring-up: GLES3 context + 1x1 pbuffer (drawing goes to an FBO).
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

    // *** THE BUG ***
    // Immutable, single-level RGBA8 texture filled with opaque red.
    // No glTexParameteri — default min filter is GL_NEAREST_MIPMAP_LINEAR.
    // GLES3 §8.17 says this is complete; SwiftShader treats it as incomplete.
    glGenTextures(1, &src_tex);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, SIZE, SIZE);
    {
        size_t byteSize = (size_t)SIZE * (size_t)SIZE * 4;
        uint8_t* cpu = (uint8_t*)malloc(byteSize);
        if (!cpu) { set_err(&s, "malloc red buffer"); goto cleanup; }
        for (size_t i = 0; i < byteSize; i += 4) {
            cpu[i+0] = 255; cpu[i+1] = 0; cpu[i+2] = 0; cpu[i+3] = 255;
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SIZE, SIZE,
                        GL_RGBA, GL_UNSIGNED_BYTE, cpu);
        free(cpu);
    }
    if (!check_gl(&s, "src texture setup")) goto cleanup;

    // Destination FBO. Clear to BLUE so "draw didn't run" is visually distinct
    // from "fragment shader wrote zeros".
    glGenTextures(1, &dst_tex);
    glBindTexture(GL_TEXTURE_2D, dst_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIZE, SIZE, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        set_err(&s, "FBO incomplete"); goto cleanup;
    }

    vs = compile_shader(GL_VERTEX_SHADER, VS_SRC, &s); if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, FS_SRC, &s); if (!fs) goto cleanup;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_uv");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { set_err(&s, "link failed"); goto cleanup; }
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
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(glGetUniformLocation(program, "u_tex"), 0);

    glViewport(0, 0, SIZE, SIZE);
    glClearColor(0.f, 0.f, 1.f, 1.f); // BLUE sentinel
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "readback")) goto cleanup;
    s.success = 1;

cleanup:
    if (vbo) glDeleteBuffers(1, &vbo);
    if (program) glDeleteProgram(program);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (dst_tex) glDeleteTextures(1, &dst_tex);
    if (src_tex) glDeleteTextures(1, &src_tex);
    if (surface != EGL_NO_SURFACE) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
    }
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
    return s;
}

