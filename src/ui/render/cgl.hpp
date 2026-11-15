#pragma once
// ---------------------------------------------------------------------------
// cgl.hpp - a tiny, self-contained OpenGL 3.3 core loader for CardiacVisualizer.
//
// Why hand-rolled: the ImGui OpenGL3 backend ships its own minimal loader
// (imgui_impl_opengl3_loader.h) that covers shaders/VAOs/textures but NOT
// framebuffers/renderbuffers (needed for the bloom pipeline), and it keeps its
// function pointers private to the backend. Vendoring full GLAD would add a
// generated ~5k-line dependency. Instead we declare exactly the entry points
// the advanced renderer uses, load them once after the context is current, and
// expose them under cg_gl* names.
//
// ISOLATION RULE: this header must ONLY be included by src/ui/render/*.cpp
// translation units. It defines its own GL types/constants and remaps the
// standard gl* names to cg_gl* pointers, so it must never be mixed in the same
// TU with <GL/gl.h>, GLEW/GLAD, or imgui_impl_opengl3*.h. The renderer's public
// header (trajectory_renderer.hpp) exposes no GL types, keeping that boundary.
// ---------------------------------------------------------------------------

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
// windows.h leaks these legacy macros; drop them so camera code can use zNear etc.
#  ifdef near
#    undef near
#  endif
#  ifdef far
#    undef far
#  endif
#endif

#ifndef APIENTRY
#  define APIENTRY
#endif
#ifndef APIENTRYP
#  define APIENTRYP APIENTRY *
#endif

// --- GL base types (self-contained; do NOT also include <GL/gl.h>) ---------
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef double         GLclampd;
typedef char           GLchar;
typedef long long      GLintptr;
typedef long long      GLsizeiptr;
typedef unsigned short GLhalf;

// --- GL constants (core subset used by the renderer) -----------------------
#define GL_FALSE                     0
#define GL_TRUE                      1
#define GL_NONE                      0
#define GL_NO_ERROR                  0

#define GL_POINTS                    0x0000
#define GL_LINES                     0x0001
#define GL_LINE_STRIP                0x0003
#define GL_TRIANGLES                 0x0004
#define GL_TRIANGLE_STRIP            0x0005
#define GL_TRIANGLE_FAN              0x0006

#define GL_DEPTH_BUFFER_BIT          0x00000100
#define GL_COLOR_BUFFER_BIT          0x00004000

#define GL_CW                        0x0900
#define GL_CCW                       0x0901
#define GL_FRONT                     0x0404
#define GL_BACK                      0x0405
#define GL_LESS                      0x0201
#define GL_LEQUAL                    0x0203

#define GL_ZERO                      0
#define GL_ONE                       1
#define GL_SRC_ALPHA                 0x0302
#define GL_ONE_MINUS_SRC_ALPHA       0x0303
#define GL_DST_ALPHA                 0x0304
#define GL_ONE_MINUS_DST_ALPHA       0x0305

#define GL_DEPTH_TEST                0x0B71
#define GL_BLEND                     0x0BE2
#define GL_CULL_FACE                 0x0B44
#define GL_SCISSOR_TEST              0x0C11
#define GL_PROGRAM_POINT_SIZE        0x8642
#define GL_MULTISAMPLE               0x809D

#define GL_UNSIGNED_BYTE             0x1401
#define GL_UNSIGNED_SHORT            0x1403
#define GL_UNSIGNED_INT              0x1405
#define GL_HALF_FLOAT                0x140B
#define GL_FLOAT                     0x1406

#define GL_VENDOR                    0x1F00
#define GL_RENDERER                  0x1F01
#define GL_VERSION                   0x1F02
#define GL_MAJOR_VERSION             0x821B
#define GL_MINOR_VERSION             0x821C

#define GL_UNPACK_ALIGNMENT          0x0CF5

#define GL_TEXTURE_2D                0x0DE1
#define GL_TEXTURE0                  0x84C0
#define GL_TEXTURE_MAG_FILTER        0x2800
#define GL_TEXTURE_MIN_FILTER        0x2801
#define GL_TEXTURE_WRAP_S            0x2802
#define GL_TEXTURE_WRAP_T            0x2803
#define GL_NEAREST                   0x2600
#define GL_LINEAR                    0x2601
#define GL_NEAREST_MIPMAP_LINEAR     0x2702
#define GL_LINEAR_MIPMAP_LINEAR      0x2703
#define GL_CLAMP_TO_EDGE             0x812F
#define GL_REPEAT                    0x2901

#define GL_RED                       0x1903
#define GL_RGB                       0x1907
#define GL_RGBA                      0x1908
#define GL_R8                        0x8229
#define GL_RG8                       0x822B
#define GL_RGB8                      0x8051
#define GL_RGBA8                     0x8058
#define GL_R16F                      0x822D
#define GL_RG16F                     0x822F
#define GL_RGB16F                    0x881B
#define GL_RGBA16F                   0x881A
#define GL_DEPTH_COMPONENT           0x1902
#define GL_DEPTH_COMPONENT24         0x81A6
#define GL_DEPTH24_STENCIL8          0x88F0

#define GL_ARRAY_BUFFER              0x8892
#define GL_ELEMENT_ARRAY_BUFFER      0x8893
#define GL_STREAM_DRAW               0x88E0
#define GL_STATIC_DRAW               0x88E4
#define GL_DYNAMIC_DRAW              0x88E8
#define GL_MAP_READ_BIT              0x0001
#define GL_MAP_WRITE_BIT             0x0002
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008

#define GL_FRAMEBUFFER               0x8D40
#define GL_READ_FRAMEBUFFER          0x8CA8
#define GL_DRAW_FRAMEBUFFER          0x8CA9
#define GL_RENDERBUFFER              0x8D41
#define GL_COLOR_ATTACHMENT0         0x8CE0
#define GL_COLOR_ATTACHMENT1         0x8CE1
#define GL_COLOR_ATTACHMENT2         0x8CE2
#define GL_COLOR_ATTACHMENT3         0x8CE3
#define GL_DEPTH_ATTACHMENT          0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT  0x821A
#define GL_FRAMEBUFFER_COMPLETE      0x8CD5

#define GL_VERTEX_SHADER             0x8B31
#define GL_FRAGMENT_SHADER           0x8B30
#define GL_COMPILE_STATUS            0x8B81
#define GL_LINK_STATUS               0x8B82
#define GL_INFO_LOG_LENGTH           0x8B84

// --- function-pointer table ------------------------------------------------
// X(<GL suffix>, <return type>, <signature>) ; symbol becomes cg_gl<suffix>.
#define CGL_FUNC_LIST(X)                                                     \
  X(Enable, void, GLenum cap)                                                \
  X(Disable, void, GLenum cap)                                               \
  X(BlendFunc, void, GLenum sfactor, GLenum dfactor)                         \
  X(BlendFuncSeparate, void, GLenum srcRGB, GLenum dstRGB, GLenum srcA, GLenum dstA) \
  X(DepthFunc, void, GLenum func)                                            \
  X(DepthMask, void, GLboolean flag)                                         \
  X(ColorMask, void, GLboolean r, GLboolean g, GLboolean b, GLboolean a)     \
  X(CullFace, void, GLenum mode)                                             \
  X(FrontFace, void, GLenum mode)                                            \
  X(LineWidth, void, GLfloat width)                                          \
  X(PointSize, void, GLfloat size)                                           \
  X(PixelStorei, void, GLenum pname, GLint param)                            \
  X(Viewport, void, GLint x, GLint y, GLsizei w, GLsizei h)                  \
  X(Scissor, void, GLint x, GLint y, GLsizei w, GLsizei h)                   \
  X(Clear, void, GLbitfield mask)                                            \
  X(ClearColor, void, GLfloat r, GLfloat g, GLfloat b, GLfloat a)            \
  X(ClearDepth, void, GLdouble d)                                            \
  X(Finish, void, void)                                                      \
  X(Flush, void, void)                                                       \
  X(GetError, GLenum, void)                                                  \
  X(GetString, const GLubyte *, GLenum name)                                 \
  X(GetIntegerv, void, GLenum pname, GLint *data)                            \
  X(ReadPixels, void, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *px) \
  X(DrawBuffers, void, GLsizei n, const GLenum *bufs)                        \
  X(ActiveTexture, void, GLenum tex)                                         \
  X(GenTextures, void, GLsizei n, GLuint *t)                                 \
  X(DeleteTextures, void, GLsizei n, const GLuint *t)                        \
  X(BindTexture, void, GLenum target, GLuint t)                              \
  X(TexImage2D, void, GLenum target, GLint lvl, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *px) \
  X(TexSubImage2D, void, GLenum target, GLint lvl, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *px) \
  X(TexParameteri, void, GLenum target, GLenum pname, GLint p)               \
  X(TexParameterf, void, GLenum target, GLenum pname, GLfloat p)             \
  X(GenerateMipmap, void, GLenum target)                                     \
  X(GenBuffers, void, GLsizei n, GLuint *b)                                  \
  X(DeleteBuffers, void, GLsizei n, const GLuint *b)                         \
  X(BindBuffer, void, GLenum target, GLuint b)                               \
  X(BufferData, void, GLenum target, GLsizeiptr size, const void *data, GLenum usage) \
  X(BufferSubData, void, GLenum target, GLintptr off, GLsizeiptr size, const void *data) \
  X(GenVertexArrays, void, GLsizei n, GLuint *a)                             \
  X(DeleteVertexArrays, void, GLsizei n, const GLuint *a)                    \
  X(BindVertexArray, void, GLuint a)                                         \
  X(VertexAttribPointer, void, GLuint idx, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr) \
  X(EnableVertexAttribArray, void, GLuint idx)                               \
  X(DisableVertexAttribArray, void, GLuint idx)                              \
  X(VertexAttribDivisor, void, GLuint idx, GLuint div)                       \
  X(MapBufferRange, void *, GLenum target, GLintptr off, GLsizeiptr len, GLenum acc) \
  X(UnmapBuffer, GLboolean, GLenum target)                                   \
  X(DrawArrays, void, GLenum mode, GLint first, GLsizei count)               \
  X(DrawElements, void, GLenum mode, GLsizei count, GLenum type, const void *idx) \
  X(DrawArraysInstanced, void, GLenum mode, GLint first, GLsizei count, GLsizei prim) \
  X(CreateShader, GLuint, GLenum type)                                       \
  X(ShaderSource, void, GLuint s, GLsizei n, const GLchar *const *src, const GLint *len) \
  X(CompileShader, void, GLuint s)                                           \
  X(GetShaderiv, void, GLuint s, GLenum pname, GLint *p)                     \
  X(GetShaderInfoLog, void, GLuint s, GLsizei max, GLsizei *len, GLchar *log) \
  X(DeleteShader, void, GLuint s)                                            \
  X(CreateProgram, GLuint, void)                                             \
  X(AttachShader, void, GLuint p, GLuint s)                                  \
  X(LinkProgram, void, GLuint p)                                             \
  X(GetProgramiv, void, GLuint p, GLenum pname, GLint *v)                    \
  X(GetProgramInfoLog, void, GLuint p, GLsizei max, GLsizei *len, GLchar *log) \
  X(DeleteProgram, void, GLuint p)                                           \
  X(UseProgram, void, GLuint p)                                              \
  X(GetUniformLocation, GLint, GLuint p, const GLchar *name)                 \
  X(GetAttribLocation, GLint, GLuint p, const GLchar *name)                  \
  X(Uniform1i, void, GLint loc, GLint v)                                     \
  X(Uniform1f, void, GLint loc, GLfloat v)                                   \
  X(Uniform2f, void, GLint loc, GLfloat a, GLfloat b)                        \
  X(Uniform3f, void, GLint loc, GLfloat a, GLfloat b, GLfloat c)             \
  X(Uniform4f, void, GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d)  \
  X(Uniform1fv, void, GLint loc, GLsizei n, const GLfloat *v)                \
  X(Uniform2fv, void, GLint loc, GLsizei n, const GLfloat *v)                \
  X(Uniform3fv, void, GLint loc, GLsizei n, const GLfloat *v)                \
  X(Uniform4fv, void, GLint loc, GLsizei n, const GLfloat *v)                \
  X(UniformMatrix3fv, void, GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) \
  X(UniformMatrix4fv, void, GLint loc, GLsizei n, GLboolean tr, const GLfloat *v) \
  X(GenFramebuffers, void, GLsizei n, GLuint *f)                             \
  X(DeleteFramebuffers, void, GLsizei n, const GLuint *f)                    \
  X(BindFramebuffer, void, GLenum target, GLuint f)                          \
  X(FramebufferTexture2D, void, GLenum target, GLenum att, GLenum tt, GLuint tex, GLint lvl) \
  X(CheckFramebufferStatus, GLenum, GLenum target)                           \
  X(GenRenderbuffers, void, GLsizei n, GLuint *r)                            \
  X(DeleteRenderbuffers, void, GLsizei n, const GLuint *r)                   \
  X(BindRenderbuffer, void, GLenum target, GLuint r)                         \
  X(RenderbufferStorage, void, GLenum target, GLenum ifmt, GLsizei w, GLsizei h) \
  X(FramebufferRenderbuffer, void, GLenum target, GLenum att, GLenum rt, GLuint rb) \
  X(BlitFramebuffer, void, GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)

#define CGL_TYPEDEF(name, ret, ...) typedef ret (APIENTRYP PFN_gl##name)(__VA_ARGS__);
CGL_FUNC_LIST(CGL_TYPEDEF)

#define CGL_EXTERN(name, ret, ...) extern PFN_gl##name cg_gl##name;
CGL_FUNC_LIST(CGL_EXTERN)

namespace cardiac::gl {

// Loads every entry point above against the CURRENT GL context. Returns true
// when all required functions resolved. On failure, missing_symbol() names the
// first unresolved entry point and error() holds a human-readable summary.
bool load();
bool loaded();
const char* missing_symbol();
const char* error();
// Context-reported version/renderer strings (valid after load()).
int major_version();
int minor_version();
const char* renderer_string();

} // namespace cardiac::gl

// Remap the familiar gl* names to our pointers. Safe ONLY because render TUs
// never include another GL header (see ISOLATION RULE at top).
#define glEnable                     cg_glEnable
#define glDisable                    cg_glDisable
#define glBlendFunc                  cg_glBlendFunc
#define glBlendFuncSeparate          cg_glBlendFuncSeparate
#define glDepthFunc                  cg_glDepthFunc
#define glDepthMask                  cg_glDepthMask
#define glColorMask                  cg_glColorMask
#define glCullFace                   cg_glCullFace
#define glFrontFace                  cg_glFrontFace
#define glLineWidth                  cg_glLineWidth
#define glPointSize                  cg_glPointSize
#define glPixelStorei                cg_glPixelStorei
#define glViewport                   cg_glViewport
#define glScissor                    cg_glScissor
#define glClear                      cg_glClear
#define glClearColor                 cg_glClearColor
#define glClearDepth                 cg_glClearDepth
#define glFinish                     cg_glFinish
#define glFlush                      cg_glFlush
#define glGetError                   cg_glGetError
#define glGetString                  cg_glGetString
#define glGetIntegerv                cg_glGetIntegerv
#define glReadPixels                 cg_glReadPixels
#define glDrawBuffers                cg_glDrawBuffers
#define glActiveTexture              cg_glActiveTexture
#define glGenTextures                cg_glGenTextures
#define glDeleteTextures             cg_glDeleteTextures
#define glBindTexture                cg_glBindTexture
#define glTexImage2D                 cg_glTexImage2D
#define glTexSubImage2D              cg_glTexSubImage2D
#define glTexParameteri              cg_glTexParameteri
#define glTexParameterf              cg_glTexParameterf
#define glGenerateMipmap             cg_glGenerateMipmap
#define glGenBuffers                 cg_glGenBuffers
#define glDeleteBuffers              cg_glDeleteBuffers
#define glBindBuffer                 cg_glBindBuffer
#define glBufferData                 cg_glBufferData
#define glBufferSubData              cg_glBufferSubData
#define glGenVertexArrays            cg_glGenVertexArrays
#define glDeleteVertexArrays         cg_glDeleteVertexArrays
#define glBindVertexArray            cg_glBindVertexArray
#define glVertexAttribPointer        cg_glVertexAttribPointer
#define glEnableVertexAttribArray    cg_glEnableVertexAttribArray
#define glDisableVertexAttribArray   cg_glDisableVertexAttribArray
#define glVertexAttribDivisor        cg_glVertexAttribDivisor
#define glMapBufferRange             cg_glMapBufferRange
#define glUnmapBuffer                cg_glUnmapBuffer
#define glDrawArrays                 cg_glDrawArrays
#define glDrawElements               cg_glDrawElements
#define glDrawArraysInstanced        cg_glDrawArraysInstanced
#define glCreateShader               cg_glCreateShader
#define glShaderSource               cg_glShaderSource
#define glCompileShader              cg_glCompileShader
#define glGetShaderiv                cg_glGetShaderiv
#define glGetShaderInfoLog           cg_glGetShaderInfoLog
#define glDeleteShader               cg_glDeleteShader
#define glCreateProgram              cg_glCreateProgram
#define glAttachShader               cg_glAttachShader
#define glLinkProgram                cg_glLinkProgram
#define glGetProgramiv               cg_glGetProgramiv
#define glGetProgramInfoLog          cg_glGetProgramInfoLog
#define glDeleteProgram              cg_glDeleteProgram
#define glUseProgram                 cg_glUseProgram
#define glGetUniformLocation         cg_glGetUniformLocation
#define glGetAttribLocation          cg_glGetAttribLocation
#define glUniform1i                  cg_glUniform1i
#define glUniform1f                  cg_glUniform1f
#define glUniform2f                  cg_glUniform2f
#define glUniform3f                  cg_glUniform3f
#define glUniform4f                  cg_glUniform4f
#define glUniform1fv                 cg_glUniform1fv
#define glUniform2fv                 cg_glUniform2fv
#define glUniform3fv                 cg_glUniform3fv
#define glUniform4fv                 cg_glUniform4fv
#define glUniformMatrix3fv           cg_glUniformMatrix3fv
#define glUniformMatrix4fv           cg_glUniformMatrix4fv
#define glGenFramebuffers            cg_glGenFramebuffers
#define glDeleteFramebuffers         cg_glDeleteFramebuffers
#define glBindFramebuffer            cg_glBindFramebuffer
#define glFramebufferTexture2D       cg_glFramebufferTexture2D
#define glCheckFramebufferStatus     cg_glCheckFramebufferStatus
#define glGenRenderbuffers           cg_glGenRenderbuffers
#define glDeleteRenderbuffers        cg_glDeleteRenderbuffers
#define glBindRenderbuffer           cg_glBindRenderbuffer
#define glRenderbufferStorage        cg_glRenderbufferStorage
#define glFramebufferRenderbuffer    cg_glFramebufferRenderbuffer
#define glBlitFramebuffer            cg_glBlitFramebuffer
