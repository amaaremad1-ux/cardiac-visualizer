// gl_shader.cpp - ShaderProgram implementation.
#include "gl_shader.hpp"

#include "cgl.hpp"

#include <cstdio>
#include <vector>

namespace cardiac::gl {
namespace {

bool compile_stage(unsigned int type, const char* src, unsigned int& out, std::string& log) {
  GLuint s = glCreateShader(type);
  if (s == 0u) {
    log += "glCreateShader returned 0\n";
    return false;
  }
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = GL_FALSE;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    GLint len = 0;
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
    GLsizei cap = len > 0 ? static_cast<GLsizei>(len) + 1 : 1024;
    std::vector<char> buf(static_cast<size_t>(cap), '\0');
    glGetShaderInfoLog(s, cap, nullptr, buf.data());
    char head[64];
    std::snprintf(head, sizeof(head), "%s compile failed:\n",
                  type == GL_VERTEX_SHADER ? "vertex" : "fragment");
    log += head;
    log += buf.data();
    log += "\n";
    glDeleteShader(s);
    return false;
  }
  out = s;
  return true;
}

} // namespace

ShaderProgram::~ShaderProgram() {
  if (program_ != 0u) glDeleteProgram(program_);
}

ShaderProgram::ShaderProgram(ShaderProgram&& o) noexcept
    : program_(o.program_), log_(std::move(o.log_)),
      uniform_cache_(std::move(o.uniform_cache_)) {
  o.program_ = 0u;
}

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& o) noexcept {
  if (this != &o) {
    if (program_ != 0u) glDeleteProgram(program_);
    program_ = o.program_;
    log_ = std::move(o.log_);
    uniform_cache_ = std::move(o.uniform_cache_);
    o.program_ = 0u;
  }
  return *this;
}

bool ShaderProgram::build(const char* vertex_src, const char* fragment_src) {
  log_.clear();
  uniform_cache_.clear();
  unsigned int vs = 0u, fs = 0u;
  if (!compile_stage(GL_VERTEX_SHADER, vertex_src, vs, log_)) return false;
  if (!compile_stage(GL_FRAGMENT_SHADER, fragment_src, fs, log_)) {
    glDeleteShader(vs);
    return false;
  }
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  // Shaders are flagged for deletion on detach; keep them until program dies.
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = GL_FALSE;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (ok != GL_TRUE) {
    GLint len = 0;
    glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
    GLsizei cap = len > 0 ? static_cast<GLsizei>(len) + 1 : 1024;
    std::vector<char> buf(static_cast<size_t>(cap), '\0');
    glGetProgramInfoLog(p, cap, nullptr, buf.data());
    log_ += "link failed:\n";
    log_ += buf.data();
    glDeleteProgram(p);
    return false;
  }
  program_ = p;
  return true;
}

void ShaderProgram::use() const {
  if (program_ != 0u) glUseProgram(program_);
}

int ShaderProgram::uniform(const char* name) {
  auto it = uniform_cache_.find(name);
  if (it != uniform_cache_.end()) return it->second;
  GLint loc = program_ != 0u ? glGetUniformLocation(program_, name) : -1;
  uniform_cache_.emplace(name, static_cast<int>(loc));
  return static_cast<int>(loc);
}

void ShaderProgram::set_int(const char* name, int v) { glUniform1i(uniform(name), v); }
void ShaderProgram::set_float(const char* name, float v) { glUniform1f(uniform(name), v); }
void ShaderProgram::set_vec2(const char* name, float x, float y) {
  glUniform2f(uniform(name), x, y);
}
void ShaderProgram::set_vec3(const char* name, float x, float y, float z) {
  glUniform3f(uniform(name), x, y, z);
}
void ShaderProgram::set_vec4(const char* name, float x, float y, float z, float w) {
  glUniform4f(uniform(name), x, y, z, w);
}
void ShaderProgram::set_mat4(const char* name, const float* m) {
  glUniformMatrix4fv(uniform(name), 1, GL_FALSE, m);
}

} // namespace cardiac::gl
