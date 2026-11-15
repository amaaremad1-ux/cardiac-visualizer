#pragma once
// gl_shader.hpp - RAII shader-program helper over the cgl loader.
#include <string>
#include <unordered_map>

namespace cardiac::gl {

// Compiles + links a GLSL 330 core program. On failure, log() holds the
// concatenated compile/link info log and valid() is false. Uniform locations
// are queried lazily and cached. Not copyable; movable.
class ShaderProgram {
public:
  ShaderProgram() = default;
  ~ShaderProgram();
  ShaderProgram(const ShaderProgram&) = delete;
  ShaderProgram& operator=(const ShaderProgram&) = delete;
  ShaderProgram(ShaderProgram&& o) noexcept;
  ShaderProgram& operator=(ShaderProgram&& o) noexcept;

  bool build(const char* vertex_src, const char* fragment_src);
  bool valid() const { return program_ != 0u; }
  void use() const;
  const std::string& log() const { return log_; }

  int uniform(const char* name);
  void set_int(const char* name, int v);
  void set_float(const char* name, float v);
  void set_vec2(const char* name, float x, float y);
  void set_vec3(const char* name, float x, float y, float z);
  void set_vec4(const char* name, float x, float y, float z, float w);
  void set_mat4(const char* name, const float* col_major_16);

private:
  unsigned int program_ = 0u;
  std::string log_;
  std::unordered_map<std::string, int> uniform_cache_;
};

} // namespace cardiac::gl
