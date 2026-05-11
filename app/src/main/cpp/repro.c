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

static void set_err(ReproStatus* s, const char* msg) {
    s->success = 0;
    size_t n = strlen(msg);
    if (n >= sizeof(s->error)) n = sizeof(s->error) - 1;
    memcpy(s->error, msg, n);
    s->error[n] = '\0';
}

typedef ReproStatus (*WorkFn)(uint8_t* pixels, int width, int height);

// Brings up an offscreen GLES context (version 2 or 3) backed by a 1x1 pbuffer,
// runs `do_work`, then tears everything down.
static ReproStatus run_with_gl(int gles_version, uint8_t* pixels, int width, int height, WorkFn do_work) {
    ReproStatus s = {0};

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) { set_err(&s, "eglGetDisplay returned EGL_NO_DISPLAY"); return s; }

    if (!eglInitialize(display, NULL, NULL)) { set_err(&s, "eglInitialize failed"); return s; }

    const EGLint renderable = (gles_version >= 3) ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT;
    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, renderable,
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

    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, gles_version, EGL_NONE };
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

// ---------------------------------------------------------------------------
// Shared helper: check glGetError, set s.error if non-zero.
// ---------------------------------------------------------------------------

static int check_gl(ReproStatus* s, const char* where) {
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) return 1;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s: glGetError = 0x%04X", where, err);
    set_err(s, buf);
    return 0;
}

// ---------------------------------------------------------------------------
// Variant 2 / 3: RGBA8 texture-FBO + glClear + glReadPixels.
// ---------------------------------------------------------------------------

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
    if (!check_gl(&s, "variant2/3 readback")) goto cleanup;

    s.success = 1;

cleanup:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (texture) glDeleteTextures(1, &texture);
    return s;
}

ReproStatus repro_variant2_fbo_clear_readpixels(uint8_t* pixels, int width, int height) {
    LOGI("variant2: clear RGBA8 FBO to opaque-red, glReadPixels to malloc buffer (%dx%d)", width, height);
    return run_with_gl(2, pixels, width, height, fbo_clear_readpixels);
}

ReproStatus repro_variant3_fbo_clear_readpixels_into_bitmap(uint8_t* mapped_pixels, int width, int height) {
    LOGI("variant3: clear RGBA8 FBO to opaque-red, glReadPixels into AndroidBitmap-locked buffer (%dx%d)", width, height);
    return run_with_gl(2, mapped_pixels, width, height, fbo_clear_readpixels);
}

// ---------------------------------------------------------------------------
// Variant 4: shader-rasterized full-viewport quad, output opaque green.
// ---------------------------------------------------------------------------

static const char* VS_SRC =
    "attribute vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char* FS_SRC =
    "precision mediump float;\n"
    "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n"; // OPAQUE GREEN

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

static ReproStatus shader_fullscreen_quad(uint8_t* pixels, int width, int height) {
    ReproStatus s = {0};

    GLuint texture = 0, fbo = 0, vs = 0, fs = 0, program = 0, vbo = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        char buf[128]; snprintf(buf, sizeof(buf), "FBO incomplete, status=0x%04X", status);
        set_err(&s, buf); goto cleanup;
    }

    vs = compile_shader(GL_VERTEX_SHADER, VS_SRC, &s);
    if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, FS_SRC, &s);
    if (!fs) goto cleanup;

    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = {0};
        glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        char buf[256]; snprintf(buf, sizeof(buf), "program link failed: %s", log);
        set_err(&s, buf); goto cleanup;
    }

    glUseProgram(program);

    static const GLfloat quad[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);

    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // opaque black background
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "variant4 readback")) goto cleanup;

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

ReproStatus repro_variant4_shader_fullscreen_quad(uint8_t* pixels, int width, int height) {
    LOGI("variant4: shader-rasterized fullscreen quad (opaque green) to RGBA8 FBO (%dx%d)", width, height);
    return run_with_gl(2, pixels, width, height, shader_fullscreen_quad);
}

// ---------------------------------------------------------------------------
// Variant 5: MSAA renderbuffer + glBlitFramebuffer resolve.
// ---------------------------------------------------------------------------

static ReproStatus msaa_resolve(uint8_t* pixels, int width, int height) {
    ReproStatus s = {0};

    GLuint rb_msaa = 0, fbo_msaa = 0, tex_resolve = 0, fbo_resolve = 0;

    // MSAA FBO with renderbuffer color attachment.
    glGenRenderbuffers(1, &rb_msaa);
    glBindRenderbuffer(GL_RENDERBUFFER, rb_msaa);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, width, height);
    if (!check_gl(&s, "variant5 glRenderbufferStorageMultisample")) goto cleanup;

    glGenFramebuffers(1, &fbo_msaa);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_msaa);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb_msaa);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char buf[128]; snprintf(buf, sizeof(buf), "variant5 MSAA FBO incomplete, status=0x%04X", st);
        set_err(&s, buf); goto cleanup;
    }

    // Single-sample resolve target.
    glGenTextures(1, &tex_resolve);
    glBindTexture(GL_TEXTURE_2D, tex_resolve);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &fbo_resolve);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_resolve);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_resolve, 0);
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char buf[128]; snprintf(buf, sizeof(buf), "variant5 resolve FBO incomplete, status=0x%04X", st);
        set_err(&s, buf); goto cleanup;
    }

    // Clear MSAA FBO to opaque red.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_msaa);
    glViewport(0, 0, width, height);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f); // OPAQUE RED
    glClear(GL_COLOR_BUFFER_BIT);

    // Blit-resolve from MSAA → single-sample.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_msaa);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_resolve);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (!check_gl(&s, "variant5 glBlitFramebuffer")) goto cleanup;

    // Readback from resolve target.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_resolve);
    glFinish();
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "variant5 readback")) goto cleanup;

    s.success = 1;

cleanup:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo_resolve) glDeleteFramebuffers(1, &fbo_resolve);
    if (tex_resolve) glDeleteTextures(1, &tex_resolve);
    if (fbo_msaa) glDeleteFramebuffers(1, &fbo_msaa);
    if (rb_msaa) glDeleteRenderbuffers(1, &rb_msaa);
    return s;
}

ReproStatus repro_variant5_msaa_resolve(uint8_t* pixels, int width, int height) {
    LOGI("variant5: MSAA RGBA8 (4 samples) → blit-resolve → read (%dx%d)", width, height);
    return run_with_gl(3, pixels, width, height, msaa_resolve);
}

// ---------------------------------------------------------------------------
// Variant 6: sRGB color attachment.
// ---------------------------------------------------------------------------

static ReproStatus srgb_framebuffer(uint8_t* pixels, int width, int height) {
    ReproStatus s = {0};
    GLuint texture = 0, fbo = 0;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    if (!check_gl(&s, "variant6 glTexImage2D GL_SRGB8_ALPHA8")) goto cleanup;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char buf[128]; snprintf(buf, sizeof(buf), "variant6 FBO incomplete, status=0x%04X", st);
        set_err(&s, buf); goto cleanup;
    }

    glViewport(0, 0, width, height);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f); // OPAQUE RED (linear); sRGB encoded write
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "variant6 readback")) goto cleanup;

    s.success = 1;

cleanup:
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (texture) glDeleteTextures(1, &texture);
    return s;
}

ReproStatus repro_variant6_srgb_framebuffer(uint8_t* pixels, int width, int height) {
    LOGI("variant6: SRGB8_ALPHA8 FBO + glClear(opaque-red) + readback (%dx%d)", width, height);
    return run_with_gl(3, pixels, width, height, srgb_framebuffer);
}

// ---------------------------------------------------------------------------
// Variant 7: offset partial-quad with .copy blend on a transparent-black background.
// Mirrors the GoodNotes Renderer's `drawColoredRectangle` call against the
// canonical failing scene (512x512, 256x256 red rect at (128, 128)).
// ---------------------------------------------------------------------------

// Vertex shader: pass an NDC [-1, 1] position straight through.
static const char* VS_OFFSET_QUAD_SRC =
    "attribute vec2 a_pos;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

// Fragment shader: solid color from a uniform.
static const char* FS_UNIFORM_COLOR_SRC =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main() { gl_FragColor = u_color; }\n";

static ReproStatus offset_quad_copy_blend(uint8_t* pixels, int width, int height) {
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
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char buf[128]; snprintf(buf, sizeof(buf), "variant7 FBO incomplete, status=0x%04X", st);
        set_err(&s, buf); goto cleanup;
    }

    vs = compile_shader(GL_VERTEX_SHADER, VS_OFFSET_QUAD_SRC, &s); if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, FS_UNIFORM_COLOR_SRC, &s); if (!fs) goto cleanup;

    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = {0}; glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        char buf[256]; snprintf(buf, sizeof(buf), "variant7 link failed: %s", log);
        set_err(&s, buf); goto cleanup;
    }
    glUseProgram(program);
    GLint u_color = glGetUniformLocation(program, "u_color");

    // Quad covering the middle half of NDC ([-0.5, 0.5]) = pixel (128, 128) → (384, 384)
    // in a 512x512 viewport.
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

    // .copy blendMode = source replaces destination, no alpha blending.
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);

    // Clear to transparent black (this is the corner color we expect to survive).
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw the centered red quad.
    glUniform4f(u_color, 1.0f, 0.0f, 0.0f, 1.0f); // opaque red
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "variant7 readback")) goto cleanup;

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

ReproStatus repro_variant7_offset_quad_copy_blend(uint8_t* pixels, int width, int height) {
    LOGI("variant7: offset red quad with .copy blend on transparent-black bg (%dx%d)", width, height);
    return run_with_gl(2, pixels, width, height, offset_quad_copy_blend);
}

// ---------------------------------------------------------------------------
// Variant 8: texture sampling. Source texture pre-filled with red is sampled
// in the fragment shader to fill a target FBO.
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

static ReproStatus texture_sampling(uint8_t* pixels, int width, int height) {
    ReproStatus s = {0};

    GLuint src_texture = 0, dst_texture = 0, fbo = 0;
    GLuint vs = 0, fs = 0, program = 0, vbo = 0;

    // Source texture: CPU-fill 256x256 RGBA with opaque red.
    size_t src_bytes = (size_t)width * (size_t)height * 4;
    uint8_t* src_pixels = malloc(src_bytes);
    if (!src_pixels) { set_err(&s, "variant8 malloc failed"); goto cleanup; }
    for (size_t i = 0; i < src_bytes; i += 4) {
        src_pixels[i + 0] = 255; src_pixels[i + 1] = 0;
        src_pixels[i + 2] = 0;   src_pixels[i + 3] = 255;
    }

    glGenTextures(1, &src_texture);
    glBindTexture(GL_TEXTURE_2D, src_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, src_pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!check_gl(&s, "variant8 source glTexImage2D")) goto cleanup;

    // Destination texture + FBO.
    glGenTextures(1, &dst_texture);
    glBindTexture(GL_TEXTURE_2D, dst_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, dst_texture, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char buf[128]; snprintf(buf, sizeof(buf), "variant8 FBO incomplete, status=0x%04X", st);
        set_err(&s, buf); goto cleanup;
    }

    // Program.
    vs = compile_shader(GL_VERTEX_SHADER, VS_TEXTURED_QUAD_SRC, &s); if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, FS_TEXTURED_QUAD_SRC, &s); if (!fs) goto cleanup;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_uv");
    glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = {0}; glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        char buf[256]; snprintf(buf, sizeof(buf), "variant8 link failed: %s", log);
        set_err(&s, buf); goto cleanup;
    }
    glUseProgram(program);

    // Fullscreen quad with UVs covering the full source texture.
    static const GLfloat quad[] = {
        // pos      // uv
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
    glBindTexture(GL_TEXTURE_2D, src_texture);
    GLint u_tex = glGetUniformLocation(program, "u_tex");
    glUniform1i(u_tex, 0);

    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "variant8 readback")) goto cleanup;

    s.success = 1;

cleanup:
    free(src_pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (program) glDeleteProgram(program);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (src_texture) glDeleteTextures(1, &src_texture);
    if (dst_texture) glDeleteTextures(1, &dst_texture);
    return s;
}

ReproStatus repro_variant8_texture_sampling(uint8_t* pixels, int width, int height) {
    LOGI("variant8: CPU-filled red source texture sampled to target FBO (%dx%d)", width, height);
    return run_with_gl(2, pixels, width, height, texture_sampling);
}

// ---------------------------------------------------------------------------
// Variant 9: glTexSubImage2D upload from a CPU buffer, then readback.
// ---------------------------------------------------------------------------

static ReproStatus texsubimage_upload(uint8_t* pixels, int width, int height) {
    ReproStatus s = {0};
    GLuint texture = 0, fbo = 0;

    // CPU buffer pre-filled with opaque red.
    size_t bytes = (size_t)width * (size_t)height * 4;
    uint8_t* cpu_pixels = malloc(bytes);
    if (!cpu_pixels) { set_err(&s, "variant9 malloc failed"); goto cleanup; }
    for (size_t i = 0; i < bytes; i += 4) {
        cpu_pixels[i + 0] = 255; cpu_pixels[i + 1] = 0;
        cpu_pixels[i + 2] = 0;   cpu_pixels[i + 3] = 255;
    }

    // Empty texture allocated via glTexImage2D (no initial data) and then
    // filled via glTexSubImage2D — the renderer's CPU-buffer upload path.
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_RGBA, GL_UNSIGNED_BYTE, cpu_pixels);
    if (!check_gl(&s, "variant9 glTexSubImage2D")) goto cleanup;

    // Bind as FBO, glReadPixels.
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char buf[128]; snprintf(buf, sizeof(buf), "variant9 FBO incomplete, status=0x%04X", st);
        set_err(&s, buf); goto cleanup;
    }

    glFinish();
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "variant9 readback")) goto cleanup;

    s.success = 1;

cleanup:
    free(cpu_pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (texture) glDeleteTextures(1, &texture);
    return s;
}

ReproStatus repro_variant9_texsubimage_upload(uint8_t* pixels, int width, int height) {
    LOGI("variant9: CPU buffer -> glTexSubImage2D -> FBO -> glReadPixels (%dx%d)", width, height);
    return run_with_gl(2, pixels, width, height, texsubimage_upload);
}

// ---------------------------------------------------------------------------
// Variant 10: off-thread GL — run variant 7 (offset red quad) on a worker pthread.
// ---------------------------------------------------------------------------

struct ThreadArgs {
    uint8_t* pixels;
    int width;
    int height;
    ReproStatus result;
};

static void* offthread_worker(void* arg) {
    struct ThreadArgs* t = (struct ThreadArgs*)arg;
    t->result = run_with_gl(2, t->pixels, t->width, t->height, offset_quad_copy_blend);
    return NULL;
}

ReproStatus repro_variant10_offthread_gl(uint8_t* pixels, int width, int height) {
    LOGI("variant10: variant7 on a worker pthread (%dx%d)", width, height);
    struct ThreadArgs args = { .pixels = pixels, .width = width, .height = height };
    pthread_t thread;
    if (pthread_create(&thread, NULL, offthread_worker, &args) != 0) {
        ReproStatus s; set_err(&s, "pthread_create failed"); return s;
    }
    pthread_join(thread, NULL);
    return args.result;
}

// ---------------------------------------------------------------------------
// Variants 11 / 12: chained frame mimicking the Renderer's full per-frame pipeline.
// ---------------------------------------------------------------------------

#define SWSREPRO_V11_SOURCE_SIZE 256
#define SWSREPRO_V11_TARGET_W    512
#define SWSREPRO_V11_TARGET_H    512

// Reusable resources owned by one frame's draw. Survives across iterations in variant 12.
struct ChainedFrame {
    GLuint src_texture;
    GLuint dst_texture;
    GLuint fbo;
    GLuint program;
    GLuint vbo;
    GLuint vs;
    GLuint fs;
    GLint  u_tex;
};

static int chained_frame_init(struct ChainedFrame* cf, ReproStatus* s) {
    memset(cf, 0, sizeof(*cf));

    // 1. Source texture (256x256), filled via glTexSubImage2D — the renderer's CPU upload path.
    size_t src_bytes = SWSREPRO_V11_SOURCE_SIZE * SWSREPRO_V11_SOURCE_SIZE * 4;
    uint8_t* cpu = malloc(src_bytes);
    if (!cpu) { set_err(s, "v11 init: malloc failed"); return 0; }
    for (size_t i = 0; i < src_bytes; i += 4) {
        cpu[i + 0] = 255; cpu[i + 1] = 0; cpu[i + 2] = 0; cpu[i + 3] = 255;
    }

    glGenTextures(1, &cf->src_texture);
    glBindTexture(GL_TEXTURE_2D, cf->src_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 SWSREPRO_V11_SOURCE_SIZE, SWSREPRO_V11_SOURCE_SIZE, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    SWSREPRO_V11_SOURCE_SIZE, SWSREPRO_V11_SOURCE_SIZE,
                    GL_RGBA, GL_UNSIGNED_BYTE, cpu);
    free(cpu);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!check_gl(s, "v11 init: source upload")) return 0;

    // 2. Target texture (512x512) + FBO.
    glGenTextures(1, &cf->dst_texture);
    glBindTexture(GL_TEXTURE_2D, cf->dst_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 SWSREPRO_V11_TARGET_W, SWSREPRO_V11_TARGET_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenFramebuffers(1, &cf->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, cf->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, cf->dst_texture, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char buf[128]; snprintf(buf, sizeof(buf), "v11 FBO incomplete, status=0x%04X", st);
        set_err(s, buf); return 0;
    }

    // 3. Program — textured quad sampling from src_texture.
    cf->vs = compile_shader(GL_VERTEX_SHADER, VS_TEXTURED_QUAD_SRC, s); if (!cf->vs) return 0;
    cf->fs = compile_shader(GL_FRAGMENT_SHADER, FS_TEXTURED_QUAD_SRC, s); if (!cf->fs) return 0;
    cf->program = glCreateProgram();
    glAttachShader(cf->program, cf->vs);
    glAttachShader(cf->program, cf->fs);
    glBindAttribLocation(cf->program, 0, "a_pos");
    glBindAttribLocation(cf->program, 1, "a_uv");
    glLinkProgram(cf->program);
    GLint linked = 0; glGetProgramiv(cf->program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = {0}; glGetProgramInfoLog(cf->program, sizeof(log) - 1, NULL, log);
        char buf[256]; snprintf(buf, sizeof(buf), "v11 link failed: %s", log);
        set_err(s, buf); return 0;
    }
    cf->u_tex = glGetUniformLocation(cf->program, "u_tex");

    // 4. VBO: centered quad in NDC [-0.5, 0.5] × [-0.5, 0.5] (= pixel offset (128, 128)
    // on a 512x512 viewport). UVs cover the full source texture.
    static const GLfloat quad[] = {
        -0.5f, -0.5f, 0.0f, 0.0f,
         0.5f, -0.5f, 1.0f, 0.0f,
        -0.5f,  0.5f, 0.0f, 1.0f,
         0.5f,  0.5f, 1.0f, 1.0f,
    };
    glGenBuffers(1, &cf->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, cf->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    return 1;
}

static void chained_frame_destroy(struct ChainedFrame* cf) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (cf->vbo) glDeleteBuffers(1, &cf->vbo);
    if (cf->program) glDeleteProgram(cf->program);
    if (cf->vs) glDeleteShader(cf->vs);
    if (cf->fs) glDeleteShader(cf->fs);
    if (cf->fbo) glDeleteFramebuffers(1, &cf->fbo);
    if (cf->src_texture) glDeleteTextures(1, &cf->src_texture);
    if (cf->dst_texture) glDeleteTextures(1, &cf->dst_texture);
}

// One render-pass: target FBO is cleared, source texture is sampled, quad is drawn.
// Does not read back — caller does that.
static void chained_frame_draw(struct ChainedFrame* cf) {
    glBindFramebuffer(GL_FRAMEBUFFER, cf->fbo);
    glViewport(0, 0, SWSREPRO_V11_TARGET_W, SWSREPRO_V11_TARGET_H);
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(cf->program);
    glBindBuffer(GL_ARRAY_BUFFER, cf->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (const void*)(2 * sizeof(GLfloat)));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cf->src_texture);
    glUniform1i(cf->u_tex, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static ReproStatus chained_frame_single(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    ReproStatus s = {0};
    struct ChainedFrame cf;
    if (!chained_frame_init(&cf, &s)) { chained_frame_destroy(&cf); return s; }

    chained_frame_draw(&cf);
    glFinish();
    glReadPixels(0, 0, SWSREPRO_V11_TARGET_W, SWSREPRO_V11_TARGET_H,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (check_gl(&s, "v11 readback")) s.success = 1;

    chained_frame_destroy(&cf);
    return s;
}

ReproStatus repro_variant11_chained_frame(uint8_t* pixels, int width, int height) {
    LOGI("variant11: CPU-upload + texture-sample + .copy blend, single chained frame (%dx%d)", width, height);
    return run_with_gl(2, pixels, width, height, chained_frame_single);
}

#define SWSREPRO_V12_ITERATIONS 50

static ReproStatus chained_frame_loop(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    ReproStatus s = {0};
    struct ChainedFrame cf;
    if (!chained_frame_init(&cf, &s)) { chained_frame_destroy(&cf); return s; }

    for (int i = 0; i < SWSREPRO_V12_ITERATIONS; i++) {
        chained_frame_draw(&cf);
        glFinish();
    }

    glReadPixels(0, 0, SWSREPRO_V11_TARGET_W, SWSREPRO_V11_TARGET_H,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (check_gl(&s, "v12 readback")) s.success = 1;

    chained_frame_destroy(&cf);
    return s;
}

ReproStatus repro_variant12_multiframe_loop(uint8_t* pixels, int width, int height) {
    LOGI("variant12: chained-frame loop x%d (%dx%d)", SWSREPRO_V12_ITERATIONS, width, height);
    return run_with_gl(2, pixels, width, height, chained_frame_loop);
}
