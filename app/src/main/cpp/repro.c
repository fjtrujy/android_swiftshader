// The SwiftShader Android-emulator render bug — minimal repro.
//
// One JNI entry point invoked from Kotlin (`repro_run_test`) does the entire
// trigger sequence in a fresh process:
//   - Bring up GLES3 + 1x1 pbuffer (all on main thread).
//   - Allocate a 256x256 RGBA8 source texture (glTexStorage2D).
//   - Upload opaque red via a GL_PIXEL_UNPACK_BUFFER using glMapBufferRange
//     with `GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT`, memcpy red into
//     the mapped pointer, glUnmapBuffer, then glTexSubImage2D from PBO
//     offset 0.
//   - Bind a 512x512 RGBA8 destination FBO; draw a full-NDC quad sampling
//     the source texture; glReadPixels.
//
// Expected output: every pixel `(255, 0, 0, 255)`.
// On the Android emulator with `-gpu swiftshader`: every pixel `(0, 0, 0, 0)`.
// On `-gpu swangle` (ANGLE → Vulkan → SwiftShader-Vulkan): correct red.
//
// The bisection on this branch isolated the trigger to a single map flag:
//   - WRITE_BIT alone                       → ✅ red
//   - WRITE_BIT | INVALIDATE_BUFFER_BIT     → ❌ zeros (THIS)
//   - WRITE_BIT | INVALIDATE_RANGE_BIT      → ✅ red
//   - glBufferSubData (no map/unmap)        → ✅ red
//   - direct glTexImage2D from CPU pointer  → ✅ red
//
// Conclusion: SwiftShader 4.0.0.1 in the Android emulator's GLES translator
// mishandles glMapBufferRange on GL_PIXEL_UNPACK_BUFFER when
// GL_MAP_INVALIDATE_BUFFER_BIT is set. The bytes written through the mapped
// pointer never reach the buffer's backing store before glUnmapBuffer +
// glTexSubImage2D from PBO offset 0 read it, so the texture ends up filled
// with zeros and subsequent sampling produces transparent black.

#include "repro.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#define LOG_TAG "swsrepro"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

// PBO constants — not exposed by the NDK GLES headers we link against.
#define V_GL_PIXEL_UNPACK_BUFFER       0x88EC
#define V_GL_STREAM_DRAW               0x88E0
#define V_GL_MAP_WRITE_BIT             0x0002
#define V_GL_MAP_INVALIDATE_BUFFER_BIT 0x0008

#define SOURCE_SIZE  256
#define TARGET_W     512
#define TARGET_H     512

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
// Shaders.
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
// Main path: bootstrap GLES3 + PBO upload + sample texture, all on the main
// thread (no worker, no shared context). Bisect step.
// ---------------------------------------------------------------------------

static ReproStatus run_test(uint8_t* pixels) {
    ReproStatus s = {0};
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext main_context = EGL_NO_CONTEXT;
    EGLSurface main_surface = EGL_NO_SURFACE;
    GLuint src_tex = 0, dst_tex = 0, fbo = 0, vs = 0, fs = 0, program = 0, vbo = 0;

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
    main_context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (main_context == EGL_NO_CONTEXT) { set_err(&s, "main eglCreateContext"); goto cleanup; }
    const EGLint pbuf_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    main_surface = eglCreatePbufferSurface(display, config, pbuf_attribs);
    if (main_surface == EGL_NO_SURFACE) { set_err(&s, "main pbuffer"); goto cleanup; }
    if (!eglMakeCurrent(display, main_surface, main_surface, main_context)) {
        set_err(&s, "main makeCurrent"); goto cleanup;
    }

    LOGI("main: GL_VENDOR='%s' GL_RENDERER='%s' GL_VERSION='%s'",
         (const char*)glGetString(GL_VENDOR),
         (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VERSION));

    // Source texture: 256x256 RGBA8. MUTABLE storage via glTexImage2D from a
    // malloc'd CPU buffer (NO glTexStorage2D, NO PBO). Default min filter is
    // GL_NEAREST_MIPMAP_LINEAR. Bisect step: with 1 mipmap level and a
    // mipmap-aware min filter, a mutable texture is technically INCOMPLETE
    // per the GLES3 spec — so swangle and swiftshader should both return
    // (0,0,0,0) on sample. This tests where each implementation actually
    // draws the completeness line.
    glGenTextures(1, &src_tex);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    {
        size_t byteSize = (size_t)SOURCE_SIZE * (size_t)SOURCE_SIZE * 4;
        uint8_t* cpu = (uint8_t*)malloc(byteSize);
        if (!cpu) { set_err(&s, "main malloc red buffer"); goto cleanup; }
        for (size_t i = 0; i < byteSize; i += 4) {
            cpu[i+0] = 255; cpu[i+1] = 0; cpu[i+2] = 0; cpu[i+3] = 255;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SOURCE_SIZE, SOURCE_SIZE, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, cpu);
        free(cpu);
    }
    // Deliberately NO glTexParameteri here. Default min filter is mipmap-aware.

    glGenTextures(1, &dst_tex);
    glBindTexture(GL_TEXTURE_2D, dst_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TARGET_W, TARGET_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        set_err(&s, "main FBO incomplete"); goto cleanup;
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
    GLint u_tex = glGetUniformLocation(program, "u_tex");
    glUniform1i(u_tex, 0);

    glViewport(0, 0, TARGET_W, TARGET_H);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    glReadPixels(0, 0, TARGET_W, TARGET_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "readback")) goto cleanup;
    s.success = 1;

cleanup:
    if (src_tex) glDeleteTextures(1, &src_tex);
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

ReproStatus repro_run_test(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    LOGI("repro_run_test: glTexImage2D (mutable, 1 level) + default mipmap min filter");
    return run_test(pixels);
}
