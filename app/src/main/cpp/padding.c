// padding.c — historical variants 2-9, 11-18 from the bisection that found the
// SwiftShader Android-emulator render bug. None of these are invoked from
// Kotlin in this repro (MainActivity calls only variant 10 and variant 19,
// defined in repro.c). They live here purely because aggressive stripping
// makes the bug stop reproducing — the trigger is sensitive to having this
// code compiled into libswsrepro.so even when never called.
//
// For the actual repro see ../README.md and `repro.c`.

#include "repro_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#define LOG_TAG "swsrepro"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

// runs `do_work`, then tears everything down.



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


ReproStatus repro_variant7_offset_quad_copy_blend(uint8_t* pixels, int width, int height) {
    LOGI("variant7: offset red quad with .copy blend on transparent-black bg (%dx%d)", width, height);
    return run_with_gl(2, pixels, width, height, offset_quad_copy_blend);
}

// ---------------------------------------------------------------------------
// Variant 8: texture sampling. Source texture pre-filled with red is sampled
// in the fragment shader to fill a target FBO.
// ---------------------------------------------------------------------------


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

// ---------------------------------------------------------------------------
// Variant 13: GLES3 instanced textured draw + normal alpha blend.
// ---------------------------------------------------------------------------

// GLES3 vertex shader: gl_InstanceID positions 16 quads in a 4x4 grid covering NDC.
static const char* VS_INSTANCED_GRID_SRC =
    "#version 300 es\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  float col = float(gl_InstanceID % 4);\n"
    "  float row = float(gl_InstanceID / 4);\n"
    "  vec2 tileMin = vec2(-1.0 + col * 0.5, -1.0 + row * 0.5);\n"
    "  vec2 ndc = tileMin + 0.5 * (a_pos * 0.5 + 0.5);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

static const char* FS_TEXTURE_SAMPLE_ES3_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 o_color;\n"
    "void main() { o_color = texture(u_tex, v_uv); }\n";

#define SWSREPRO_V13_SOURCE_SIZE 256
#define SWSREPRO_V13_TARGET_W    512
#define SWSREPRO_V13_TARGET_H    512

static ReproStatus instanced_textured_blend(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    ReproStatus s = {0};
    GLuint src_tex = 0, dst_tex = 0, fbo = 0, vs = 0, fs = 0, program = 0, vbo = 0;

    // 1. Source texture: 256x256 RGBA8 pre-filled with opaque red.
    size_t src_bytes = SWSREPRO_V13_SOURCE_SIZE * SWSREPRO_V13_SOURCE_SIZE * 4;
    uint8_t* src_pixels = malloc(src_bytes);
    if (!src_pixels) { set_err(&s, "variant13 malloc"); goto cleanup; }
    for (size_t i = 0; i < src_bytes; i += 4) {
        src_pixels[i+0] = 255; src_pixels[i+1] = 0; src_pixels[i+2] = 0; src_pixels[i+3] = 255;
    }
    glGenTextures(1, &src_tex);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SWSREPRO_V13_SOURCE_SIZE, SWSREPRO_V13_SOURCE_SIZE,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, src_pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!check_gl(&s, "v13 source upload")) goto cleanup;

    // 2. Target FBO 512x512.
    glGenTextures(1, &dst_tex);
    glBindTexture(GL_TEXTURE_2D, dst_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SWSREPRO_V13_TARGET_W, SWSREPRO_V13_TARGET_H,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        char buf[128]; snprintf(buf, sizeof(buf), "v13 FBO incomplete 0x%04X", st);
        set_err(&s, buf); goto cleanup;
    }

    // 3. Program.
    vs = compile_shader(GL_VERTEX_SHADER, VS_INSTANCED_GRID_SRC, &s); if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, FS_TEXTURE_SAMPLE_ES3_SRC, &s); if (!fs) goto cleanup;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_uv");
    glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = {0}; glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        char buf[256]; snprintf(buf, sizeof(buf), "v13 link failed: %s", log);
        set_err(&s, buf); goto cleanup;
    }
    glUseProgram(program);

    // Fullscreen quad with UVs covering the full source texture.
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
    glBindTexture(GL_TEXTURE_2D, src_tex);
    GLint u_tex = glGetUniformLocation(program, "u_tex");
    glUniform1i(u_tex, 0);

    glViewport(0, 0, SWSREPRO_V13_TARGET_W, SWSREPRO_V13_TARGET_H);

    // Normal alpha blend — mirrors the renderer's drawTexturedRectangles(blend=normal).
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // The smoking-gun call: instanced draw with texture sampling + alpha blend.
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 16);
    glFinish();

    glReadPixels(0, 0, SWSREPRO_V13_TARGET_W, SWSREPRO_V13_TARGET_H,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "v13 readback")) goto cleanup;

    s.success = 1;

cleanup:
    free(src_pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (program) glDeleteProgram(program);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (src_tex) glDeleteTextures(1, &src_tex);
    if (dst_tex) glDeleteTextures(1, &dst_tex);
    return s;
}

ReproStatus repro_variant13_instanced_textured_blend(uint8_t* pixels, int width, int height) {
    LOGI("variant13: drawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 16) sampling red atlas, normal blend, 4x4 grid -> 512x512 FBO");
    return run_with_gl(3, pixels, width, height, instanced_textured_blend);
}

// ---------------------------------------------------------------------------
// Variant 14: negative-Y viewport (extends past the framebuffer).
// ---------------------------------------------------------------------------

static ReproStatus negative_y_viewport(uint8_t* pixels, int width, int height) {
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
        char buf[128]; snprintf(buf, sizeof(buf), "v14 FBO incomplete 0x%04X", st);
        set_err(&s, buf); goto cleanup;
    }

    vs = compile_shader(GL_VERTEX_SHADER, VS_OFFSET_QUAD_SRC, &s); if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, FS_UNIFORM_COLOR_SRC, &s); if (!fs) goto cleanup;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { set_err(&s, "v14 link failed"); goto cleanup; }
    glUseProgram(program);
    GLint u_color = glGetUniformLocation(program, "u_color");

    // Full-NDC quad.
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

    // KEY CALL: viewport with NEGATIVE Y origin, extending past the framebuffer.
    // Mirrors the renderer's `glViewport(0, -2048, 4096, 4096)` on a 2048x2048 FB.
    // Here: 256x256 FB, viewport (0, -256, 512, 512) — half above (Y < 0), half below.
    glViewport(0, -height, width * 2, height * 2);

    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUniform4f(u_color, 1.0f, 0.0f, 0.0f, 1.0f); // opaque red
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "v14 readback")) goto cleanup;

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

ReproStatus repro_variant14_negative_y_viewport(uint8_t* pixels, int width, int height) {
    LOGI("variant14: glViewport(0, -%d, %d, %d) on %dx%d FB + clear + red quad",
         height, width * 2, height * 2, width, height);
    return run_with_gl(2, pixels, width, height, negative_y_viewport);
}

// ---------------------------------------------------------------------------
// Variant 15: drawArraysInstanced with per-instance vertex attribute (divisor=1).
// ---------------------------------------------------------------------------

// Vertex shader: a_pos is per-vertex (divisor 0), a_instanceOffset is per-instance
// (divisor 1). Positions a 0.5×0.5-NDC tile per instance, tiling 16 instances 4x4.
static const char* VS_INSTANCE_ATTRIBUTE_SRC =
    "#version 300 es\n"
    "in vec2 a_pos;            // per-vertex, divisor=0\n"
    "in vec2 a_instanceOffset; // per-instance, divisor=1 (NDC offset for this tile)\n"
    "in vec2 a_uv;             // per-vertex, divisor=0\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 ndc = a_instanceOffset + 0.5 * (a_pos * 0.5 + 0.5);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

static ReproStatus instanced_attribute_divisor(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    ReproStatus s = {0};
    GLuint src_tex = 0, dst_tex = 0, fbo = 0, vs = 0, fs = 0, program = 0;
    GLuint vbo_quad = 0, vbo_instance = 0;

    const int W = SWSREPRO_V7_WIDTH;
    const int H = SWSREPRO_V7_HEIGHT;

    // 1. Source texture: 256x256 RGBA8 pre-filled with opaque red.
    size_t src_bytes = SWSREPRO_V13_SOURCE_SIZE * SWSREPRO_V13_SOURCE_SIZE * 4;
    uint8_t* src_pixels = malloc(src_bytes);
    if (!src_pixels) { set_err(&s, "v15 malloc"); goto cleanup; }
    for (size_t i = 0; i < src_bytes; i += 4) {
        src_pixels[i+0] = 255; src_pixels[i+1] = 0; src_pixels[i+2] = 0; src_pixels[i+3] = 255;
    }
    glGenTextures(1, &src_tex);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SWSREPRO_V13_SOURCE_SIZE, SWSREPRO_V13_SOURCE_SIZE,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, src_pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 2. Target FBO.
    glGenTextures(1, &dst_tex);
    glBindTexture(GL_TEXTURE_2D, dst_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) { set_err(&s, "v15 FBO incomplete"); goto cleanup; }

    // 3. Program.
    vs = compile_shader(GL_VERTEX_SHADER, VS_INSTANCE_ATTRIBUTE_SRC, &s); if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, FS_TEXTURE_SAMPLE_ES3_SRC, &s); if (!fs) goto cleanup;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_instanceOffset");
    glBindAttribLocation(program, 2, "a_uv");
    glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = {0}; glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        char buf[256]; snprintf(buf, sizeof(buf), "v15 link failed: %s", log);
        set_err(&s, buf); goto cleanup;
    }
    glUseProgram(program);

    // 4. Per-vertex VBO (4 verts, divisor=0): position + uv interleaved.
    static const GLfloat quadVerts[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    glGenBuffers(1, &vbo_quad);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), NULL);
    glVertexAttribDivisor(0, 0);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (const void*)(2 * sizeof(GLfloat)));
    glVertexAttribDivisor(2, 0);

    // 5. Per-instance VBO (16 entries, divisor=1): NDC offset for each tile.
    GLfloat instanceOffsets[32]; // 16 entries × vec2
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int i = row * 4 + col;
            instanceOffsets[i * 2 + 0] = -1.0f + (float)col * 0.5f;
            instanceOffsets[i * 2 + 1] = -1.0f + (float)row * 0.5f;
        }
    }
    glGenBuffers(1, &vbo_instance);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_instance);
    glBufferData(GL_ARRAY_BUFFER, sizeof(instanceOffsets), instanceOffsets, GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glVertexAttribDivisor(1, 1); // THE KEY CALL — advance once per instance, not vertex.

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    GLint u_tex = glGetUniformLocation(program, "u_tex");
    glUniform1i(u_tex, 0);

    glViewport(0, 0, W, H);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // The smoking-gun call.
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 16);
    glFinish();

    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "v15 readback")) goto cleanup;

    s.success = 1;

cleanup:
    free(src_pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (vbo_quad) glDeleteBuffers(1, &vbo_quad);
    if (vbo_instance) glDeleteBuffers(1, &vbo_instance);
    if (program) glDeleteProgram(program);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (src_tex) glDeleteTextures(1, &src_tex);
    if (dst_tex) glDeleteTextures(1, &dst_tex);
    return s;
}

ReproStatus repro_variant15_instanced_attribute_divisor(uint8_t* pixels, int width, int height) {
    LOGI("variant15: drawArraysInstanced(... 16) with glVertexAttribDivisor(1, 1) per-instance attr, %dx%d", width, height);
    return run_with_gl(3, pixels, width, height, instanced_attribute_divisor);
}

// ---------------------------------------------------------------------------
// Variant 16: state-pollution: previous negative-Y-viewport draw then a normal one.
// ---------------------------------------------------------------------------

static ReproStatus state_pollution(uint8_t* pixels, int width, int height) {
    ReproStatus s = {0};
    GLuint src_tex = 0, atlas_tex = 0, atlas_fbo = 0, out_tex = 0, out_fbo = 0;
    GLuint vs_uniform = 0, fs_uniform = 0, program_uniform = 0;
    GLuint vs_textured = 0, fs_textured = 0, program_textured = 0;
    GLuint vbo_pos = 0, vbo_full = 0;

    // 1. Source texture for pass 2: opaque red.
    size_t src_bytes = (size_t)width * (size_t)height * 4;
    uint8_t* src_pixels = malloc(src_bytes);
    if (!src_pixels) { set_err(&s, "v16 malloc"); goto cleanup; }
    for (size_t i = 0; i < src_bytes; i += 4) {
        src_pixels[i+0] = 255; src_pixels[i+1] = 0; src_pixels[i+2] = 0; src_pixels[i+3] = 255;
    }
    glGenTextures(1, &src_tex);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, src_pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // 2. Atlas FBO (pass-1 target).
    glGenTextures(1, &atlas_tex);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &atlas_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, atlas_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, atlas_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        set_err(&s, "v16 atlas FBO incomplete"); goto cleanup;
    }

    // 3. Output FBO (pass-2 target).
    glGenTextures(1, &out_tex);
    glBindTexture(GL_TEXTURE_2D, out_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &out_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        set_err(&s, "v16 out FBO incomplete"); goto cleanup;
    }

    // 4. Programs.
    vs_uniform = compile_shader(GL_VERTEX_SHADER, VS_OFFSET_QUAD_SRC, &s); if (!vs_uniform) goto cleanup;
    fs_uniform = compile_shader(GL_FRAGMENT_SHADER, FS_UNIFORM_COLOR_SRC, &s); if (!fs_uniform) goto cleanup;
    program_uniform = glCreateProgram();
    glAttachShader(program_uniform, vs_uniform);
    glAttachShader(program_uniform, fs_uniform);
    glBindAttribLocation(program_uniform, 0, "a_pos");
    glLinkProgram(program_uniform);

    vs_textured = compile_shader(GL_VERTEX_SHADER, VS_INSTANCED_GRID_SRC, &s); if (!vs_textured) goto cleanup;
    fs_textured = compile_shader(GL_FRAGMENT_SHADER, FS_TEXTURE_SAMPLE_ES3_SRC, &s); if (!fs_textured) goto cleanup;
    program_textured = glCreateProgram();
    glAttachShader(program_textured, vs_textured);
    glAttachShader(program_textured, fs_textured);
    glBindAttribLocation(program_textured, 0, "a_pos");
    glBindAttribLocation(program_textured, 1, "a_uv");
    glLinkProgram(program_textured);

    static const GLfloat fullQuadPos[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    glGenBuffers(1, &vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fullQuadPos), fullQuadPos, GL_STATIC_DRAW);

    static const GLfloat fullQuadInter[] = {
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f,
    };
    glGenBuffers(1, &vbo_full);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_full);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fullQuadInter), fullQuadInter, GL_STATIC_DRAW);

    // === Pass 1: atlas FBO, negative-Y viewport, .copy blend, uniform-color draw ===
    glBindFramebuffer(GL_FRAMEBUFFER, atlas_fbo);
    glViewport(0, -height, width * 2, height * 2); // The renderer's atlas viewport pattern
    glUseProgram(program_uniform);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    glClearColor(1.f, 1.f, 1.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    GLint u_color = glGetUniformLocation(program_uniform, "u_color");
    glUniform4f(u_color, 0.f, 0.f, 0.f, 0.f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    // === Pass 2: output FBO, normal viewport, textured-instanced draw ===
    glBindFramebuffer(GL_FRAMEBUFFER, out_fbo);
    glViewport(0, 0, width, height);
    glUseProgram(program_textured);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_full);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (const void*)(2 * sizeof(GLfloat)));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    GLint u_tex = glGetUniformLocation(program_textured, "u_tex");
    glUniform1i(u_tex, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 16);
    glFinish();

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "v16 readback")) goto cleanup;

    s.success = 1;

cleanup:
    free(src_pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (vbo_full) glDeleteBuffers(1, &vbo_full);
    if (vbo_pos) glDeleteBuffers(1, &vbo_pos);
    if (program_uniform) glDeleteProgram(program_uniform);
    if (vs_uniform) glDeleteShader(vs_uniform);
    if (fs_uniform) glDeleteShader(fs_uniform);
    if (program_textured) glDeleteProgram(program_textured);
    if (vs_textured) glDeleteShader(vs_textured);
    if (fs_textured) glDeleteShader(fs_textured);
    if (out_fbo) glDeleteFramebuffers(1, &out_fbo);
    if (out_tex) glDeleteTextures(1, &out_tex);
    if (atlas_fbo) glDeleteFramebuffers(1, &atlas_fbo);
    if (atlas_tex) glDeleteTextures(1, &atlas_tex);
    if (src_tex) glDeleteTextures(1, &src_tex);
    return s;
}

ReproStatus repro_variant16_state_pollution(uint8_t* pixels, int width, int height) {
    LOGI("variant16: pass1 negative-Y viewport draw then pass2 normal instanced draw (%dx%d)", width, height);
    return run_with_gl(3, pixels, width, height, state_pollution);
}

// ---------------------------------------------------------------------------
// Variant 17: drawArraysInstanced with sampler2D array indexed by per-instance attribute.
// ---------------------------------------------------------------------------

// Vertex shader: passes per-instance `a_tex_index` through to fragment as flat-int.
// Position via gl_InstanceID grid offset for simplicity.
static const char* VS_SAMPLER_ARRAY_SRC =
    "#version 300 es\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in int a_tex_index;\n"
    "out vec2 v_uv;\n"
    "flat out int v_tex_index;\n"
    "void main() {\n"
    "  float col = float(gl_InstanceID % 4);\n"
    "  float row = float(gl_InstanceID / 4);\n"
    "  vec2 tileMin = vec2(-1.0 + col * 0.5, -1.0 + row * 0.5);\n"
    "  vec2 ndc = tileMin + 0.5 * (a_pos * 0.5 + 0.5);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "  v_tex_index = a_tex_index;\n"
    "}\n";

// Fragment shader: indexes into sampler2D[2] by the per-instance index.
static const char* FS_SAMPLER_ARRAY_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "flat in int v_tex_index;\n"
    "uniform sampler2D u_textures[2];\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "  o_color = texture(u_textures[v_tex_index], v_uv);\n"
    "}\n";

static ReproStatus sampler_array_dynamic_index(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    ReproStatus s = {0};
    GLuint src_tex_0 = 0, src_tex_1 = 0, dst_tex = 0, fbo = 0;
    GLuint vs = 0, fs = 0, program = 0, vbo_pv = 0, vbo_inst = 0;

    const int W = SWSREPRO_V7_WIDTH;
    const int H = SWSREPRO_V7_HEIGHT;
    const int SRC = SWSREPRO_V13_SOURCE_SIZE;

    size_t src_bytes = (size_t)SRC * (size_t)SRC * 4;
    uint8_t* src_pixels = malloc(src_bytes);
    if (!src_pixels) { set_err(&s, "v17 malloc"); goto cleanup; }
    for (size_t i = 0; i < src_bytes; i += 4) {
        src_pixels[i+0] = 255; src_pixels[i+1] = 0; src_pixels[i+2] = 0; src_pixels[i+3] = 255;
    }

    glGenTextures(1, &src_tex_0);
    glBindTexture(GL_TEXTURE_2D, src_tex_0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SRC, SRC, 0, GL_RGBA, GL_UNSIGNED_BYTE, src_pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &src_tex_1);
    glBindTexture(GL_TEXTURE_2D, src_tex_1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SRC, SRC, 0, GL_RGBA, GL_UNSIGNED_BYTE, src_pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &dst_tex);
    glBindTexture(GL_TEXTURE_2D, dst_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        set_err(&s, "v17 FBO incomplete"); goto cleanup;
    }

    vs = compile_shader(GL_VERTEX_SHADER, VS_SAMPLER_ARRAY_SRC, &s); if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, FS_SAMPLER_ARRAY_SRC, &s); if (!fs) goto cleanup;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_uv");
    glBindAttribLocation(program, 2, "a_tex_index");
    glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = {0}; glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        char buf[256]; snprintf(buf, sizeof(buf), "v17 link failed: %s", log);
        set_err(&s, buf); goto cleanup;
    }
    glUseProgram(program);

    // Per-vertex VBO (pos + uv interleaved).
    static const GLfloat quadVerts[] = {
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f,
    };
    glGenBuffers(1, &vbo_pv);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pv);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (const void*)(2 * sizeof(GLfloat)));

    // Per-instance VBO: 16 ints, alternating 0 and 1.
    GLint texIndices[16];
    for (int i = 0; i < 16; i++) texIndices[i] = i & 1;
    glGenBuffers(1, &vbo_inst);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_inst);
    glBufferData(GL_ARRAY_BUFFER, sizeof(texIndices), texIndices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_INT, 0, NULL);
    glVertexAttribDivisor(2, 1);

    // Bind textures to TEXTURE0 and TEXTURE1, and set the sampler array uniforms.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex_0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, src_tex_1);

    GLint u_textures_loc = glGetUniformLocation(program, "u_textures[0]");
    GLint sampler_units[2] = { 0, 1 };
    glUniform1iv(u_textures_loc, 2, sampler_units);

    glViewport(0, 0, W, H);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    // The smoking-gun call.
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 16);
    glFinish();

    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "v17 readback")) goto cleanup;

    s.success = 1;

cleanup:
    free(src_pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (vbo_pv) glDeleteBuffers(1, &vbo_pv);
    if (vbo_inst) glDeleteBuffers(1, &vbo_inst);
    if (program) glDeleteProgram(program);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (src_tex_0) glDeleteTextures(1, &src_tex_0);
    if (src_tex_1) glDeleteTextures(1, &src_tex_1);
    if (dst_tex) glDeleteTextures(1, &dst_tex);
    return s;
}

ReproStatus repro_variant17_sampler_array_dynamic_index(uint8_t* pixels, int width, int height) {
    LOGI("variant17: drawArraysInstanced + sampler2D[2] array indexed by per-instance int attr (%dx%d)", width, height);
    return run_with_gl(3, pixels, width, height, sampler_array_dynamic_index);
}

// ---------------------------------------------------------------------------
// Variant 18: drawArraysInstanced + sampler2DArray with per-instance layer index.
// ---------------------------------------------------------------------------

static const char* VS_2D_ARRAY_SRC =
    "#version 300 es\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in int a_layer;\n"
    "out vec2 v_uv;\n"
    "flat out int v_layer;\n"
    "void main() {\n"
    "  float col = float(gl_InstanceID % 4);\n"
    "  float row = float(gl_InstanceID / 4);\n"
    "  vec2 tileMin = vec2(-1.0 + col * 0.5, -1.0 + row * 0.5);\n"
    "  vec2 ndc = tileMin + 0.5 * (a_pos * 0.5 + 0.5);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "  v_layer = a_layer;\n"
    "}\n";

static const char* FS_2D_ARRAY_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "precision mediump sampler2DArray;\n"
    "in vec2 v_uv;\n"
    "flat in int v_layer;\n"
    "uniform sampler2DArray u_layers;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "  o_color = texture(u_layers, vec3(v_uv, float(v_layer)));\n"
    "}\n";

static ReproStatus sampler_2d_array(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    ReproStatus s = {0};
    GLuint src_array = 0, dst_tex = 0, fbo = 0;
    GLuint vs = 0, fs = 0, program = 0, vbo_pv = 0, vbo_inst = 0;

    const int W = SWSREPRO_V7_WIDTH;
    const int H = SWSREPRO_V7_HEIGHT;
    const int SRC = SWSREPRO_V13_SOURCE_SIZE;
    const int LAYERS = 2;

    // Build red pixel data, then glTexImage3D it into a sampler2DArray with 2 layers.
    size_t layer_bytes = (size_t)SRC * (size_t)SRC * 4;
    size_t total_bytes = layer_bytes * LAYERS;
    uint8_t* layer_pixels = malloc(total_bytes);
    if (!layer_pixels) { set_err(&s, "v18 malloc"); goto cleanup; }
    for (size_t i = 0; i < total_bytes; i += 4) {
        layer_pixels[i+0] = 255; layer_pixels[i+1] = 0; layer_pixels[i+2] = 0; layer_pixels[i+3] = 255;
    }

    glGenTextures(1, &src_array);
    glBindTexture(GL_TEXTURE_2D_ARRAY, src_array);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, SRC, SRC, LAYERS, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, layer_pixels);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &dst_tex);
    glBindTexture(GL_TEXTURE_2D, dst_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        set_err(&s, "v18 FBO incomplete"); goto cleanup;
    }

    vs = compile_shader(GL_VERTEX_SHADER, VS_2D_ARRAY_SRC, &s); if (!vs) goto cleanup;
    fs = compile_shader(GL_FRAGMENT_SHADER, FS_2D_ARRAY_SRC, &s); if (!fs) goto cleanup;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_uv");
    glBindAttribLocation(program, 2, "a_layer");
    glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = {0}; glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        char buf[256]; snprintf(buf, sizeof(buf), "v18 link failed: %s", log);
        set_err(&s, buf); goto cleanup;
    }
    glUseProgram(program);

    static const GLfloat quadVerts[] = {
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f,
    };
    glGenBuffers(1, &vbo_pv);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pv);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (const void*)(2 * sizeof(GLfloat)));

    GLint layers[16];
    for (int i = 0; i < 16; i++) layers[i] = i & 1;
    glGenBuffers(1, &vbo_inst);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_inst);
    glBufferData(GL_ARRAY_BUFFER, sizeof(layers), layers, GL_STATIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_INT, 0, NULL);
    glVertexAttribDivisor(2, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, src_array);
    GLint u_layers_loc = glGetUniformLocation(program, "u_layers");
    glUniform1i(u_layers_loc, 0);

    glViewport(0, 0, W, H);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, 16);
    glFinish();

    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(&s, "v18 readback")) goto cleanup;

    s.success = 1;

cleanup:
    free(layer_pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (vbo_pv) glDeleteBuffers(1, &vbo_pv);
    if (vbo_inst) glDeleteBuffers(1, &vbo_inst);
    if (program) glDeleteProgram(program);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (src_array) glDeleteTextures(1, &src_array);
    if (dst_tex) glDeleteTextures(1, &dst_tex);
    return s;
}

ReproStatus repro_variant18_sampler_2d_array(uint8_t* pixels, int width, int height) {
    LOGI("variant18: drawArraysInstanced + sampler2DArray with per-instance layer (%dx%d)", width, height);
    return run_with_gl(3, pixels, width, height, sampler_2d_array);
}


// ---------------------------------------------------------------------------
// Dead-but-linked JNI exports. None are called from Kotlin; they exist as
// exported `JNIEXPORT` symbols (visibility=default) so the linker preserves
// the variant function bodies above. Removing them lets `-Wl,--gc-sections`
// strip the variants, which makes the bug stop reproducing.
// ---------------------------------------------------------------------------

#include <jni.h>
#include <android/bitmap.h>

static jstring padding_make_summary(JNIEnv* env, const ReproStatus* status,
                                     const uint8_t* pixels, int width, int height) {
    int cr = 0, cg = 0, cb = 0, ca = 0;
    int kr = 0, kg = 0, kb = 0, ka = 0;
    if (status->success && pixels) {
        size_t center = ((size_t)height / 2 * width + width / 2) * 4;
        cr = pixels[center];     cg = pixels[center + 1]; cb = pixels[center + 2]; ca = pixels[center + 3];
        kr = pixels[0];          kg = pixels[1];          kb = pixels[2];          ka = pixels[3];
    }
    char buf[512];
    snprintf(buf, sizeof(buf), "%d|%s|%d,%d,%d,%d|%d,%d,%d,%d",
             status->success ? 1 : 0, status->error,
             cr, cg, cb, ca, kr, kg, kb, ka);
    return (*env)->NewStringUTF(env, buf);
}

typedef ReproStatus (*PaddingVariantFn)(uint8_t* pixels, int width, int height);

static jstring padding_run_256(JNIEnv* env, jbyteArray out_pixels, PaddingVariantFn fn) {
    jsize len = (*env)->GetArrayLength(env, out_pixels);
    if (len < SWSREPRO_WIDTH * SWSREPRO_HEIGHT * 4) {
        ReproStatus s = {0, "out_pixels too small"};
        return padding_make_summary(env, &s, NULL, 0, 0);
    }
    uint8_t* pixels = malloc(SWSREPRO_WIDTH * SWSREPRO_HEIGHT * 4);
    if (!pixels) { ReproStatus s = {0, "malloc failed"}; return padding_make_summary(env, &s, NULL, 0, 0); }
    ReproStatus s = fn(pixels, SWSREPRO_WIDTH, SWSREPRO_HEIGHT);
    if (s.success) {
        (*env)->SetByteArrayRegion(env, out_pixels, 0,
                                    SWSREPRO_WIDTH * SWSREPRO_HEIGHT * 4,
                                    (const jbyte*)pixels);
    }
    jstring out = padding_make_summary(env, &s, pixels, SWSREPRO_WIDTH, SWSREPRO_HEIGHT);
    free(pixels);
    return out;
}

static jstring padding_run_512(JNIEnv* env, jbyteArray out_pixels, PaddingVariantFn fn) {
    const int W = SWSREPRO_V7_WIDTH, H = SWSREPRO_V7_HEIGHT;
    jsize len = (*env)->GetArrayLength(env, out_pixels);
    if (len < W * H * 4) {
        ReproStatus s = {0, "padding out_pixels too small (need 512*512*4)"};
        return padding_make_summary(env, &s, NULL, 0, 0);
    }
    uint8_t* pixels = malloc(W * H * 4);
    if (!pixels) { ReproStatus s = {0, "malloc failed"}; return padding_make_summary(env, &s, NULL, 0, 0); }
    ReproStatus s = fn(pixels, W, H);
    if (s.success) {
        (*env)->SetByteArrayRegion(env, out_pixels, 0, W * H * 4, (const jbyte*)pixels);
    }
    jstring out = padding_make_summary(env, &s, pixels, W, H);
    free(pixels);
    return out;
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant2(JNIEnv* env, jclass clazz, jbyteArray out_pixels) {
    (void)clazz;
    return padding_run_256(env, out_pixels, repro_variant2_fbo_clear_readpixels);
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant3(JNIEnv* env, jclass clazz, jobject bitmap) {
    (void)clazz;
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) {
        ReproStatus s = {0, "AndroidBitmap_getInfo failed"};
        return padding_make_summary(env, &s, NULL, 0, 0);
    }
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        ReproStatus s = {0, "bitmap format must be RGBA_8888"};
        return padding_make_summary(env, &s, NULL, 0, 0);
    }
    void* mapped = NULL;
    if (AndroidBitmap_lockPixels(env, bitmap, &mapped) != ANDROID_BITMAP_RESULT_SUCCESS || !mapped) {
        ReproStatus s = {0, "AndroidBitmap_lockPixels failed"};
        return padding_make_summary(env, &s, NULL, 0, 0);
    }
    ReproStatus s = repro_variant3_fbo_clear_readpixels_into_bitmap(
        (uint8_t*)mapped, (int)info.width, (int)info.height);
    jstring out = padding_make_summary(env, &s, (const uint8_t*)mapped, (int)info.width, (int)info.height);
    AndroidBitmap_unlockPixels(env, bitmap);
    return out;
}

JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant4(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant4_shader_fullscreen_quad);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant5(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant5_msaa_resolve);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant6(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant6_srgb_framebuffer);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant7(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant7_offset_quad_copy_blend);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant8(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant8_texture_sampling);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant9(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant9_texsubimage_upload);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant11(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant11_chained_frame);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant12(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant12_multiframe_loop);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant13(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant13_instanced_textured_blend);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant14(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant14_negative_y_viewport);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant15(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant15_instanced_attribute_divisor);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant16(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_256(env, p, repro_variant16_state_pollution);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant17(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant17_sampler_array_dynamic_index);
}
JNIEXPORT jstring JNICALL
Java_com_example_swsrepro_ReproNative_runVariant18(JNIEnv* env, jclass clazz, jbyteArray p) {
    (void)clazz; return padding_run_512(env, p, repro_variant18_sampler_2d_array);
}
