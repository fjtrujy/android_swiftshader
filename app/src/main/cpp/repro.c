// SwiftShader Android-emulator render bug — Goodnotes-style UBO draw probe.
//
// This is intentionally tiny, but it mirrors the simplest RendererV5 path
// closely:
//
//   1. Create a GLES3 pbuffer context.
//   2. Allocate a single-level immutable RGBA8 render target with
//      glTexStorage2D and set a non-mipmap min filter.
//   3. Clear the target to transparent black.
//   4. Draw a fullscreen red rectangle using the same ingredients as the
//      RendererV5 colored-rectangle shader:
//        - layout(std140) uniform DrawingUniforms
//        - glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo)
//        - gl_VertexID-generated triangle strip
//        - no vertex attributes
//   5. Read back pixels through the same flipped FBO blit shape used by the
//      Android snapshot tests.
//
// If direct `-gpu swiftshader` returns transparent black while swangle returns
// red, this is a small repro for the empty Goodnotes Android recordings.

#include "repro.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include <android/log.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "swsrepro"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define SIZE 256
#define DRAWING_UNIFORMS_BINDING 0

typedef struct {
    uint8_t r, g, b, a;
} Pixel;

// Mirrors RendererV5's DrawingUniforms std140 payload:
// vec2 page size, vec2 camera pixel size, vec2 camera center, float scale,
// float rotation. Total: 32 bytes.
typedef struct {
    float pageModelSize[2];
    float cameraPixelSize[2];
    float cameraModelCenter[2];
    float cameraPixelFromModelScale;
    float cameraRotation;
} DrawingUniforms;

static const char* VERTEX_SHADER =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision lowp int;\n"
    "layout(std140) uniform DrawingUniforms {\n"
    "  vec2 uPageModelSize;\n"
    "  vec2 uCameraPixelSize;\n"
    "  vec2 uCameraModelCenter;\n"
    "  float uCameraPixelFromModelScale;\n"
    "  float uCameraRotation;\n"
    "};\n"
    "uniform vec4 uModelFrame;\n"
    "const ivec2 coordinatesIndices[4] = ivec2[4](\n"
    "  ivec2(0, 1), ivec2(2, 1), ivec2(0, 3), ivec2(2, 3)\n"
    ");\n"
    "vec2 pixelFromModel(vec2 modelPosition,\n"
    "                    vec2 pageModelSize,\n"
    "                    vec2 cameraModelCenter,\n"
    "                    float cameraPixelFromModelScale,\n"
    "                    vec2 cameraPixelSize,\n"
    "                    int cameraRotation) {\n"
    "  vec2 pixelPositionOffsetFromCameraCenter =\n"
    "      (modelPosition - cameraModelCenter) * cameraPixelFromModelScale;\n"
    "  vec2 rotatedPixelPositionOffsetFromCenter;\n"
    "  if (cameraRotation == 0) {\n"
    "    rotatedPixelPositionOffsetFromCenter = pixelPositionOffsetFromCameraCenter;\n"
    "  } else if (cameraRotation == 1) {\n"
    "    rotatedPixelPositionOffsetFromCenter = vec2(\n"
    "        pixelPositionOffsetFromCameraCenter.y,\n"
    "        -pixelPositionOffsetFromCameraCenter.x);\n"
    "  } else if (cameraRotation == 2) {\n"
    "    rotatedPixelPositionOffsetFromCenter = -pixelPositionOffsetFromCameraCenter;\n"
    "  } else {\n"
    "    rotatedPixelPositionOffsetFromCenter = vec2(\n"
    "        -pixelPositionOffsetFromCameraCenter.y,\n"
    "        pixelPositionOffsetFromCameraCenter.x);\n"
    "  }\n"
    "  return rotatedPixelPositionOffsetFromCenter + cameraPixelSize / 2.0;\n"
    "}\n"
    "vec2 normalizedFromPixel(vec2 pixelPosition, vec2 cameraPixelSize) {\n"
    "  vec2 normalizedPosition =\n"
    "      (pixelPosition / cameraPixelSize - vec2(0.5)) * vec2(2.0);\n"
    "  return vec2(normalizedPosition.x, -normalizedPosition.y);\n"
    "}\n"
    "void main() {\n"
    "  ivec2 idx = coordinatesIndices[gl_VertexID];\n"
    "  vec2 modelPosition = vec2(uModelFrame[idx.x], uModelFrame[idx.y]);\n"
    "  vec2 pixelPosition = pixelFromModel(\n"
    "      modelPosition,\n"
    "      uPageModelSize,\n"
    "      uCameraModelCenter,\n"
    "      uCameraPixelFromModelScale,\n"
    "      uCameraPixelSize,\n"
    "      int(uCameraRotation));\n"
    "  vec2 normalizedPosition = normalizedFromPixel(pixelPosition, uCameraPixelSize);\n"
    "  gl_Position = vec4(normalizedPosition, 0.0, 1.0);\n"
    "}\n";

static const char* FRAGMENT_SHADER =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform vec4 uColor;\n"
    "out vec4 fColor;\n"
    "void main() { fColor = uColor; }\n";

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
    Pixel first = pixel_at(pixels, width, 0, 0);
    Pixel center = pixel_at(pixels, width, width / 2, height / 2);
    Pixel last = pixel_at(pixels, width, width - 1, height - 1);
    LOGI("%s: first=(%u,%u,%u,%u) center=(%u,%u,%u,%u) last=(%u,%u,%u,%u)",
         label,
         first.r, first.g, first.b, first.a,
         center.r, center.g, center.b, center.a,
         last.r, last.g, last.b, last.a);
}

static int validate_red(ReproStatus* s, const char* label, const uint8_t* pixels) {
    Pixel first = pixel_at(pixels, SIZE, 0, 0);
    Pixel center = pixel_at(pixels, SIZE, SIZE / 2, SIZE / 2);
    Pixel last = pixel_at(pixels, SIZE, SIZE - 1, SIZE - 1);
    if (first.r == 255 && first.g == 0 && first.b == 0 && first.a == 255 &&
        center.r == 255 && center.g == 0 && center.b == 0 && center.a == 255 &&
        last.r == 255 && last.g == 0 && last.b == 0 && last.a == 255) {
        return 1;
    }

    char buf[256];
    snprintf(
        buf, sizeof(buf),
        "%s mismatch: first=(%u,%u,%u,%u), center=(%u,%u,%u,%u), last=(%u,%u,%u,%u)",
        label,
        first.r, first.g, first.b, first.a,
        center.r, center.g, center.b, center.a,
        last.r, last.g, last.b, last.a
    );
    set_err(s, buf);
    return 0;
}

static int read_direct(ReproStatus* s, GLuint source_fbo) {
    uint8_t* direct = (uint8_t*)malloc((size_t)SIZE * (size_t)SIZE * 4);
    if (!direct) {
        set_err(s, "malloc direct buffer");
        return 0;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, source_fbo);
    if (!check_fbo(s, GL_READ_FRAMEBUFFER, "direct read FBO")) {
        free(direct);
        return 0;
    }
    glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, direct);
    if (!check_gl(s, "direct glReadPixels")) {
        free(direct);
        return 0;
    }

    log_samples("direct readback", direct, SIZE, SIZE);
    int ok = validate_red(s, "direct readback", direct);
    free(direct);
    return ok;
}

static int read_flipped(ReproStatus* s, GLuint source_fbo, uint8_t* pixels) {
    GLuint dst_rbo = 0;
    GLuint dst_fbo = 0;

    glGenRenderbuffers(1, &dst_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, dst_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, SIZE, SIZE);

    glGenFramebuffers(1, &dst_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, dst_rbo);
    if (!check_fbo(s, GL_DRAW_FRAMEBUFFER, "flipped blit draw FBO")) goto fail;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, source_fbo);
    if (!check_fbo(s, GL_READ_FRAMEBUFFER, "flipped blit read FBO")) goto fail;

    glBlitFramebuffer(
        0, SIZE, SIZE, 0,
        0, 0, SIZE, SIZE,
        GL_COLOR_BUFFER_BIT,
        GL_NEAREST
    );
    if (!check_gl(s, "flipped glBlitFramebuffer")) goto fail;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, dst_fbo);
    if (!check_fbo(s, GL_READ_FRAMEBUFFER, "flipped readback FBO")) goto fail;
    glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (!check_gl(s, "flipped glReadPixels")) goto fail;

    log_samples("flipped readback", pixels, SIZE, SIZE);
    if (!validate_red(s, "flipped readback", pixels)) goto fail;

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

static int draw_renderer_style_rectangle(ReproStatus* s, GLuint target_fbo) {
    GLuint program = 0;
    GLuint ubo = 0;

    program = create_program(s);
    if (!program) return 0;
    glUseProgram(program);

    GLuint block_index = glGetUniformBlockIndex(program, "DrawingUniforms");
    if (block_index == GL_INVALID_INDEX) {
        set_err(s, "DrawingUniforms block not found");
        goto fail;
    }
    glUniformBlockBinding(program, block_index, DRAWING_UNIFORMS_BINDING);

    DrawingUniforms uniforms = {
        {SIZE, SIZE},
        {SIZE, SIZE},
        {SIZE / 2.0f, SIZE / 2.0f},
        1.0f,
        0.0f,
    };
    glGenBuffers(1, &ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(uniforms), &uniforms, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, DRAWING_UNIFORMS_BINDING, ubo);

    GLint model_frame = glGetUniformLocation(program, "uModelFrame");
    GLint color = glGetUniformLocation(program, "uColor");
    if (model_frame < 0 || color < 0) {
        set_err(s, "uniform lookup failed");
        goto fail;
    }

    glUniform4f(model_frame, 0.0f, 0.0f, (float)SIZE, (float)SIZE);
    glUniform4f(color, 1.0f, 0.0f, 0.0f, 1.0f);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);
    glViewport(0, 0, SIZE, SIZE);
    glDisable(GL_BLEND);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (!check_gl(s, "renderer-style glDrawArrays")) goto fail;

    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glUseProgram(0);
    glDeleteBuffers(1, &ubo);
    glDeleteProgram(program);
    return 1;

fail:
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glUseProgram(0);
    if (ubo) glDeleteBuffers(1, &ubo);
    if (program) glDeleteProgram(program);
    return 0;
}

ReproStatus repro_run_test(uint8_t* pixels, int width, int height) {
    (void)width; (void)height;
    LOGI("repro_run_test: Goodnotes-style std140 UBO + gl_VertexID rectangle into immutable render target");

    ReproStatus s = {0};
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint render_tex = 0, fbo = 0;

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

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, render_tex, 0);
    if (!check_fbo(&s, GL_FRAMEBUFFER, "render target FBO")) goto cleanup;

    glViewport(0, 0, SIZE, SIZE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!check_gl(&s, "transparent clear")) goto cleanup;

    if (!draw_renderer_style_rectangle(&s, fbo)) goto cleanup;
    glFinish();

    if (!read_direct(&s, fbo)) goto cleanup;
    if (!read_flipped(&s, fbo, pixels)) goto cleanup;

    s.success = 1;

cleanup:
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (render_tex) glDeleteTextures(1, &render_tex);
    if (surface != EGL_NO_SURFACE) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display, surface);
    }
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
    return s;
}
