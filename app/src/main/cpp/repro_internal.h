// Internal header — declares helpers shared between repro.c (the actual test)
// and padding.c (the historical variants kept for the library-size dependency).
//
// Reviewer note: the public surface is in `repro.h`. This file exists only to
// share static-utility code with the padding module without text-duplicating
// it. Everything here is implementation detail.

#ifndef SWSREPRO_REPRO_INTERNAL_H
#define SWSREPRO_REPRO_INTERNAL_H

#include "repro.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

// PBO constants — not exposed by the NDK GLES headers we link against.
#define V_GL_PIXEL_UNPACK_BUFFER       0x88EC
#define V_GL_MAP_WRITE_BIT             0x0002
#define V_GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define V_GL_STREAM_DRAW               0x88E0

// Shared helpers (defined in repro.c).
void set_err(ReproStatus* s, const char* msg);
int  check_gl(ReproStatus* s, const char* where);
GLuint compile_shader(GLenum type, const char* src, ReproStatus* s);

// EGL bootstrap helper: brings up an offscreen GLES{2,3} context backed by a
// 1x1 pbuffer, runs `do_work`, then tears everything down. Used by the padding
// variants. Defined in repro.c.
typedef ReproStatus (*WorkFn)(uint8_t* pixels, int width, int height);
ReproStatus run_with_gl(int gles_version, uint8_t* pixels, int width, int height, WorkFn do_work);

// Shared shader strings. Defined in repro.c.
extern const char* VS_OFFSET_QUAD_SRC;
extern const char* FS_UNIFORM_COLOR_SRC;
extern const char* VS_TEXTURED_QUAD_SRC;
extern const char* FS_TEXTURED_QUAD_SRC;

// Variant 10's actual draw body — used by variant 10 in repro.c. Made
// non-static so padding.c can also reference it (some padding variants reuse
// the offset-quad pattern). Defined in repro.c.
ReproStatus offset_quad_copy_blend(uint8_t* pixels, int width, int height);

#endif
