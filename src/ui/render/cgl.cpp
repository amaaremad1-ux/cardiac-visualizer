// cgl.cpp - implementation of the tiny OpenGL 3.3 core loader (see cgl.hpp).
// Defines the function pointers, resolves them against the current context, and
// records diagnostics. Isolated TU: nothing here includes ImGui or <GL/gl.h>.
#include "cgl.hpp"

#include <cstdio>

// Define every function pointer as null; load() fills them in.
#define CGL_DEFINE(name, ret, ...) PFN_gl##name cg_gl##name = nullptr;
CGL_FUNC_LIST(CGL_DEFINE)

namespace cardiac::gl {
namespace {

bool g_loaded = false;
const char* g_missing = nullptr;
char g_error[512] = {0};
int g_major = 0;
int g_minor = 0;
const char* g_renderer = "";

// Resolve one GL entry point. wglGetProcAddress returns small sentinel values
// (1/2/3/-1) on failure; 1.1 functions live in opengl32.dll and must be fetched
// with GetProcAddress there. Both paths are tried.
void* proc_address(const char* name) {
#ifdef _WIN32
  void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
  auto bad = [](void* q) {
    return q == nullptr || q == reinterpret_cast<void*>(0x1) ||
           q == reinterpret_cast<void*>(0x2) || q == reinterpret_cast<void*>(0x3) ||
           q == reinterpret_cast<void*>(-1);
  };
  if (bad(p)) {
    static HMODULE gl_dll = LoadLibraryA("opengl32.dll");
    if (gl_dll) p = reinterpret_cast<void*>(GetProcAddress(gl_dll, name));
  }
  return bad(p) ? nullptr : p;
#else
  (void)name;
  return nullptr;
#endif
}

} // namespace

bool load() {
  if (g_loaded) return true;
  g_missing = nullptr;
  g_error[0] = '\0';

#define CGL_LOADONE(name, ret, ...)                                          \
  cg_gl##name = reinterpret_cast<PFN_gl##name>(proc_address("gl" #name));     \
  if (!cg_gl##name && !g_missing) g_missing = "gl" #name;
  CGL_FUNC_LIST(CGL_LOADONE)
#undef CGL_LOADONE

  if (g_missing) {
    std::snprintf(g_error, sizeof(g_error),
                  "OpenGL entry point unresolved: %s (need a 3.3 core context)",
                  g_missing);
    return false;
  }

  // Query the context version so callers can degrade gracefully.
  if (cg_glGetIntegerv) {
    cg_glGetIntegerv(GL_MAJOR_VERSION, &g_major);
    cg_glGetIntegerv(GL_MINOR_VERSION, &g_minor);
  }
  if (cg_glGetString) {
    const GLubyte* r = cg_glGetString(GL_RENDERER);
    if (r) g_renderer = reinterpret_cast<const char*>(r);
  }
  if (cg_glGetError) { (void)cg_glGetError(); } // clear any stale error

  g_loaded = true;
  return true;
}

bool loaded() { return g_loaded; }
const char* missing_symbol() { return g_missing ? g_missing : ""; }
const char* error() { return g_error; }
int major_version() { return g_major; }
int minor_version() { return g_minor; }
const char* renderer_string() { return g_renderer; }

} // namespace cardiac::gl
