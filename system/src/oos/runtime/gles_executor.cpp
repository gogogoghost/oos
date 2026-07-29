#include "oos/runtime/gles_executor.h"

#include <GLES2/gl2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace oos::runtime {
namespace {

struct TextureFormatInfo {
  GLenum format;
  GLenum type;
  uint32_t bytes_per_pixel;
};

bool textureFormatInfo(uint32_t format, TextureFormatInfo &result) {
  switch (format) {
  case OOS_TEXTURE_A8:
    result = {GL_ALPHA, GL_UNSIGNED_BYTE, 1};
    return true;
  case OOS_TEXTURE_RGB565:
    result = {GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2};
    return true;
  case OOS_TEXTURE_RGBA4444:
    result = {GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2};
    return true;
  case OOS_TEXTURE_RGBA8888:
    result = {GL_RGBA, GL_UNSIGNED_BYTE, 4};
    return true;
  default:
    return false;
  }
}

GLenum bufferUsage(uint32_t usage) {
  switch (usage) {
  case OOS_GLES_STATIC_DRAW:
    return GL_STATIC_DRAW;
  case OOS_GLES_DYNAMIC_DRAW:
    return GL_DYNAMIC_DRAW;
  case OOS_GLES_STREAM_DRAW:
    return GL_STREAM_DRAW;
  default:
    return 0;
  }
}

bool primitiveType(uint32_t primitive, GLenum &result) {
  switch (primitive) {
  case OOS_GLES_POINTS:
    result = GL_POINTS;
    return true;
  case OOS_GLES_LINES:
    result = GL_LINES;
    return true;
  case OOS_GLES_LINE_STRIP:
    result = GL_LINE_STRIP;
    return true;
  case OOS_GLES_TRIANGLES:
    result = GL_TRIANGLES;
    return true;
  case OOS_GLES_TRIANGLE_STRIP:
    result = GL_TRIANGLE_STRIP;
    return true;
  case OOS_GLES_TRIANGLE_FAN:
    result = GL_TRIANGLE_FAN;
    return true;
  default:
    return false;
  }
}

GLenum vertexType(uint32_t type) {
  switch (type) {
  case OOS_GLES_F32:
    return GL_FLOAT;
  case OOS_GLES_U8:
    return GL_UNSIGNED_BYTE;
  case OOS_GLES_I8:
    return GL_BYTE;
  case OOS_GLES_U16:
    return GL_UNSIGNED_SHORT;
  case OOS_GLES_I16:
    return GL_SHORT;
  default:
    return 0;
  }
}

uint32_t vertexTypeBytes(GLenum type) {
  switch (type) {
  case GL_FLOAT:
    return 4;
  case GL_UNSIGNED_SHORT:
  case GL_SHORT:
    return 2;
  case GL_UNSIGNED_BYTE:
  case GL_BYTE:
    return 1;
  default:
    return 0;
  }
}

bool blendFactor(uint32_t factor, GLenum &result) {
  constexpr std::array<GLenum, 15> kFactors = {
      GL_ZERO,
      GL_ONE,
      GL_SRC_COLOR,
      GL_ONE_MINUS_SRC_COLOR,
      GL_DST_COLOR,
      GL_ONE_MINUS_DST_COLOR,
      GL_SRC_ALPHA,
      GL_ONE_MINUS_SRC_ALPHA,
      GL_DST_ALPHA,
      GL_ONE_MINUS_DST_ALPHA,
      GL_CONSTANT_COLOR,
      GL_ONE_MINUS_CONSTANT_COLOR,
      GL_CONSTANT_ALPHA,
      GL_ONE_MINUS_CONSTANT_ALPHA,
      GL_SRC_ALPHA_SATURATE,
  };
  if (factor >= kFactors.size())
    return false;
  result = kFactors[factor];
  return true;
}

GLenum blendEquation(uint32_t equation) {
  switch (equation) {
  case OOS_GLES_BLEND_ADD:
    return GL_FUNC_ADD;
  case OOS_GLES_BLEND_SUBTRACT:
    return GL_FUNC_SUBTRACT;
  case OOS_GLES_BLEND_REVERSE_SUBTRACT:
    return GL_FUNC_REVERSE_SUBTRACT;
  default:
    return 0;
  }
}

GLenum compareFunction(uint32_t function) {
  constexpr std::array<GLenum, 8> kFunctions = {
      GL_NEVER,   GL_LESS,     GL_EQUAL,  GL_LEQUAL,
      GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS,
  };
  return function < kFunctions.size() ? kFunctions[function] : 0;
}

GLenum cullFace(uint32_t face) {
  switch (face) {
  case OOS_GLES_CULL_FRONT:
    return GL_FRONT;
  case OOS_GLES_CULL_BACK:
    return GL_BACK;
  case OOS_GLES_CULL_FRONT_AND_BACK:
    return GL_FRONT_AND_BACK;
  default:
    return 0;
  }
}

bool stencilOperation(uint32_t operation, GLenum &result) {
  constexpr std::array<GLenum, 8> kOperations = {
      GL_KEEP, GL_ZERO,   GL_REPLACE,   GL_INCR,
      GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP,
  };
  if (operation >= kOperations.size())
    return false;
  result = kOperations[operation];
  return true;
}

float floatBits(uint32_t bits) {
  float result;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

void clearGlErrors() {
  while (glGetError() != GL_NO_ERROR) {
  }
}

template <typename Map> GLuint objectName(const Map &objects, uint32_t handle) {
  const auto found = objects.find(handle);
  return found == objects.end() ? 0 : found->second;
}

} // namespace

class GlesExecutor::Impl {
public:
  struct Texture {
    GLuint name = 0;
    uint32_t format = OOS_TEXTURE_RGBA8888;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  struct Buffer {
    GLuint name = 0;
    uint32_t size = 0;
  };

  struct VertexAttribute {
    bool enabled = false;
    uint32_t buffer_size = 0;
    uint32_t element_bytes = 0;
    uint32_t stride = 0;
    uint32_t offset = 0;
  };

  explicit Impl(GlesFrameTarget &target) : target(target) {}

  bool current() { return target.makeGlesContextCurrent(); }

  bool capabilities(OosGlesCapabilities &result) {
    if (!current())
      return false;
    GLint texture_size = 0;
    GLint texture_units = 0;
    GLint attributes = 0;
    GLint varyings = 0;
    GLint vertex_uniforms = 0;
    GLint fragment_uniforms = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &texture_size);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &texture_units);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
    glGetIntegerv(GL_MAX_VARYING_VECTORS, &varyings);
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &vertex_uniforms);
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &fragment_uniforms);
    result = {
        2,
        0,
        static_cast<uint32_t>(std::max(texture_size, 0)),
        static_cast<uint32_t>(std::max(texture_units, 0)),
        static_cast<uint32_t>(std::max(attributes, 0)),
        static_cast<uint32_t>(std::max(varyings, 0)),
        static_cast<uint32_t>(std::max(vertex_uniforms, 0)),
        static_cast<uint32_t>(std::max(fragment_uniforms, 0)),
        target.glesDepthBits(),
        target.glesStencilBits(),
        OOS_GLES_MAX_BUFFER_BYTES,
        OOS_GLES_MAX_COMMANDS,
        OOS_GLES_MAX_COMMAND_DATA_WORDS,
    };
    return glGetError() == GL_NO_ERROR;
  }

  bool setTexture(uint32_t handle, uint32_t format, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t row_stride,
                  uint32_t flags, const uint8_t *pixels, size_t pixel_bytes) {
    TextureFormatInfo info{};
    const bool invalid_flags =
        ((flags & OOS_TEXTURE_REPEAT_X) &&
         (flags & OOS_TEXTURE_MIRRORED_REPEAT_X)) ||
        ((flags & OOS_TEXTURE_REPEAT_Y) &&
         (flags & OOS_TEXTURE_MIRRORED_REPEAT_Y)) ||
        ((flags & OOS_TEXTURE_LINEAR_MIPMAPS) &&
         !(flags & OOS_TEXTURE_MIPMAPS));
    if (!current() || handle == 0 || !pixels || width == 0 || height == 0 ||
        invalid_flags ||
        (flags & ~OOS_TEXTURE_FLAGS_MASK) != 0 ||
        !textureFormatInfo(format, info) ||
        width > std::numeric_limits<uint32_t>::max() / info.bytes_per_pixel)
      return false;
    const uint32_t row_bytes = width * info.bytes_per_pixel;
    if (row_stride < row_bytes ||
        static_cast<uint64_t>(row_stride) * (height - 1) + row_bytes !=
            pixel_bytes)
      return false;

    auto found = textures.find(handle);
    const bool replace = (flags & OOS_TEXTURE_REPLACE) != 0;
    const bool inserted = found == textures.end();
    Texture previous;
    if (inserted) {
      if (x != 0 || y != 0)
        return false;
      Texture texture;
      glGenTextures(1, &texture.name);
      texture.format = format;
      texture.width = width;
      texture.height = height;
      found = textures.emplace(handle, texture).first;
    } else if (replace) {
      if (x != 0 || y != 0)
        return false;
      previous = found->second;
      found->second.format = format;
      found->second.width = width;
      found->second.height = height;
    } else if (found->second.format != format || x > found->second.width ||
               y > found->second.height || width > found->second.width - x ||
               height > found->second.height - y) {
      return false;
    }

    clearGlErrors();
    glBindTexture(GL_TEXTURE_2D, found->second.name);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (inserted || replace) {
      glTexImage2D(GL_TEXTURE_2D, 0, info.format, width, height, 0, info.format,
                   info.type, row_stride == row_bytes ? pixels : nullptr);
      if (row_stride != row_bytes) {
        for (uint32_t row = 0; row < height; ++row) {
          glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, width, 1, info.format,
                          info.type,
                          pixels + static_cast<size_t>(row) * row_stride);
        }
      }
    } else if (row_stride == row_bytes) {
      glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, info.format,
                      info.type, pixels);
    } else {
      for (uint32_t row = 0; row < height; ++row) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y + row, width, 1, info.format,
                        info.type,
                        pixels + static_cast<size_t>(row) * row_stride);
      }
    }
    const bool linear_minification =
        (flags & OOS_TEXTURE_LINEAR_MINIFICATION) != 0;
    const bool linear_magnification =
        (flags & OOS_TEXTURE_LINEAR_MAGNIFICATION) != 0;
    const bool mipmaps = (flags & OOS_TEXTURE_MIPMAPS) != 0;
    const bool linear_mipmaps =
        (flags & OOS_TEXTURE_LINEAR_MIPMAPS) != 0;
    const GLint min_filter = !mipmaps
                                 ? (linear_minification ? GL_LINEAR
                                                       : GL_NEAREST)
                             : linear_minification
                                 ? (linear_mipmaps ? GL_LINEAR_MIPMAP_LINEAR
                                                   : GL_LINEAR_MIPMAP_NEAREST)
                                 : (linear_mipmaps ? GL_NEAREST_MIPMAP_LINEAR
                                                   : GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    linear_magnification ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    (flags & OOS_TEXTURE_MIRRORED_REPEAT_X)
                        ? GL_MIRRORED_REPEAT
                    : (flags & OOS_TEXTURE_REPEAT_X) ? GL_REPEAT
                                                     : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    (flags & OOS_TEXTURE_MIRRORED_REPEAT_Y)
                        ? GL_MIRRORED_REPEAT
                    : (flags & OOS_TEXTURE_REPEAT_Y) ? GL_REPEAT
                                                     : GL_CLAMP_TO_EDGE);
    if (mipmaps)
      glGenerateMipmap(GL_TEXTURE_2D);
    const bool success = glGetError() == GL_NO_ERROR;
    if (!success && inserted) {
      glDeleteTextures(1, &found->second.name);
      textures.erase(found);
    } else if (!success && replace) {
      found->second = previous;
    }
    return success;
  }

  bool freeTexture(uint32_t handle) {
    if (!current())
      return false;
    const auto found = textures.find(handle);
    if (found == textures.end())
      return true;
    glDeleteTextures(1, &found->second.name);
    textures.erase(found);
    return glGetError() == GL_NO_ERROR;
  }

  bool setBuffer(uint32_t handle, uint32_t size, uint32_t usage,
                 const uint8_t *data, size_t data_size) {
    const GLenum gl_usage = bufferUsage(usage);
    if (!current() || handle == 0 || size == 0 ||
        size > OOS_GLES_MAX_BUFFER_BYTES || gl_usage == 0 ||
        (data_size != 0 && data_size != size) || (data_size && !data))
      return false;
    auto found = buffers.find(handle);
    const bool inserted = found == buffers.end();
    if (inserted) {
      Buffer buffer;
      glGenBuffers(1, &buffer.name);
      found = buffers.emplace(handle, buffer).first;
    }
    clearGlErrors();
    glBindBuffer(GL_ARRAY_BUFFER, found->second.name);
    glBufferData(GL_ARRAY_BUFFER, size, data_size ? data : nullptr, gl_usage);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    const bool success = glGetError() == GL_NO_ERROR;
    if (success) {
      found->second.size = size;
    } else if (inserted) {
      glDeleteBuffers(1, &found->second.name);
      buffers.erase(found);
    }
    return success;
  }

  bool writeBuffer(uint32_t handle, uint32_t offset, const uint8_t *data,
                   size_t data_size) {
    const auto found = buffers.find(handle);
    if (!current() || found == buffers.end() || !data || data_size == 0 ||
        offset > found->second.size || data_size > found->second.size - offset)
      return false;
    clearGlErrors();
    glBindBuffer(GL_ARRAY_BUFFER, found->second.name);
    glBufferSubData(GL_ARRAY_BUFFER, offset, data_size, data);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return glGetError() == GL_NO_ERROR;
  }

  bool freeBuffer(uint32_t handle) {
    if (!current())
      return false;
    const auto found = buffers.find(handle);
    if (found == buffers.end())
      return true;
    glDeleteBuffers(1, &found->second.name);
    buffers.erase(found);
    return glGetError() == GL_NO_ERROR;
  }

  bool setShader(uint32_t handle, uint32_t stage, const char *source,
                 size_t source_size) {
    if (!current() || handle == 0 || !source || source_size == 0 ||
        source_size > std::numeric_limits<GLint>::max())
      return false;
    const GLenum gl_stage = stage == OOS_GLES_VERTEX_SHADER ? GL_VERTEX_SHADER
                            : stage == OOS_GLES_FRAGMENT_SHADER
                                ? GL_FRAGMENT_SHADER
                                : 0;
    if (!gl_stage)
      return false;
    const GLuint name = glCreateShader(gl_stage);
    const GLint length = static_cast<GLint>(source_size);
    glShaderSource(name, 1, &source, &length);
    glCompileShader(name);
    GLint compiled = GL_FALSE;
    glGetShaderiv(name, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
      std::array<char, 1024> log{};
      glGetShaderInfoLog(name, log.size(), nullptr, log.data());
      std::fprintf(stderr, "OOS guest GLES shader failed: %s\n", log.data());
      glDeleteShader(name);
      return false;
    }
    const auto old = shaders.find(handle);
    if (old != shaders.end()) {
      glDeleteShader(old->second);
      old->second = name;
    } else {
      shaders.emplace(handle, name);
    }
    return glGetError() == GL_NO_ERROR;
  }

  bool freeShader(uint32_t handle) {
    if (!current())
      return false;
    const auto found = shaders.find(handle);
    if (found == shaders.end())
      return true;
    glDeleteShader(found->second);
    shaders.erase(found);
    return glGetError() == GL_NO_ERROR;
  }

  bool setProgram(uint32_t handle, uint32_t vertex_shader,
                  uint32_t fragment_shader) {
    const GLuint vertex = objectName(shaders, vertex_shader);
    const GLuint fragment = objectName(shaders, fragment_shader);
    if (!current() || handle == 0 || !vertex || !fragment)
      return false;
    const GLuint name = glCreateProgram();
    glAttachShader(name, vertex);
    glAttachShader(name, fragment);
    glLinkProgram(name);
    GLint linked = GL_FALSE;
    glGetProgramiv(name, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
      std::array<char, 1024> log{};
      glGetProgramInfoLog(name, log.size(), nullptr, log.data());
      std::fprintf(stderr, "OOS guest GLES program failed: %s\n", log.data());
      glDeleteProgram(name);
      return false;
    }
    const auto old = programs.find(handle);
    if (old != programs.end()) {
      glDeleteProgram(old->second);
      old->second = name;
    } else {
      programs.emplace(handle, name);
    }
    return glGetError() == GL_NO_ERROR;
  }

  bool freeProgram(uint32_t handle) {
    if (!current())
      return false;
    const auto found = programs.find(handle);
    if (found == programs.end())
      return true;
    glDeleteProgram(found->second);
    programs.erase(found);
    return glGetError() == GL_NO_ERROR;
  }

  int32_t location(uint32_t handle, const char *name, size_t name_size,
                   bool attribute) {
    const GLuint program = objectName(programs, handle);
    if (!current() || !program || !name || name_size == 0 || name_size > 255)
      return -1;
    const std::string terminated(name, name_size);
    return attribute ? glGetAttribLocation(program, terminated.c_str())
                     : glGetUniformLocation(program, terminated.c_str());
  }

  bool submit(const OosGlesCommand *commands, size_t command_count,
              const uint32_t *data, size_t data_words) {
    if (!current() || !commands || command_count < 2 ||
        command_count > OOS_GLES_MAX_COMMANDS ||
        data_words > OOS_GLES_MAX_COMMAND_DATA_WORDS || (data_words && !data) ||
        commands[0].opcode != OOS_GLES_BEGIN_FRAME ||
        commands[command_count - 1].opcode != OOS_GLES_END_FRAME)
      return false;

    bool require_depth = false;
    bool require_stencil = false;
    for (size_t index = 0; index < command_count; ++index) {
      const OosGlesCommand &command = commands[index];
      if (command.opcode == OOS_GLES_BEGIN_FRAME) {
        require_depth |= (command.args[0] & OOS_GLES_CLEAR_DEPTH) != 0;
        require_stencil |= (command.args[0] & OOS_GLES_CLEAR_STENCIL) != 0;
      } else if (command.opcode == OOS_GLES_DEPTH && command.args[0]) {
        require_depth = true;
      } else if (command.opcode == OOS_GLES_STENCIL && command.args[0]) {
        require_stencil = true;
      }
    }
    if (!target.bindGlesSurface(require_depth, require_stencil))
      return false;
    clearGlErrors();
    glViewport(0, 0, target.glesSurfaceWidth(), target.glesSurfaceHeight());
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    GLint max_attributes = 0;
    GLint max_texture_units = 0;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_attributes);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &max_texture_units);
    frame_attributes.clear();
    frame_attributes.resize(static_cast<size_t>(std::max(max_attributes, 0)));
    bool began = false;
    bool ended = false;
    const Buffer *vertex_buffer = nullptr;
    const Buffer *index_buffer = nullptr;

    for (size_t index = 0; index < command_count; ++index) {
      const OosGlesCommand &command = commands[index];
      const uint32_t *a = command.args;
      switch (command.opcode) {
      case OOS_GLES_BEGIN_FRAME: {
        if (index != 0 || began)
          return false;
        began = true;
        const uint32_t mask = a[0];
        const float clear_depth = floatBits(a[2]);
        if (mask & ~(OOS_GLES_CLEAR_COLOR | OOS_GLES_CLEAR_DEPTH |
                     OOS_GLES_CLEAR_STENCIL) ||
            !std::isfinite(clear_depth))
          return false;
        const std::array<uint8_t, 4> color = {
            static_cast<uint8_t>(a[1]), static_cast<uint8_t>(a[1] >> 8),
            static_cast<uint8_t>(a[1] >> 16), static_cast<uint8_t>(a[1] >> 24)};
        glClearColor(color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f,
                     color[3] / 255.0f);
        glClearDepthf(clear_depth);
        glClearStencil(static_cast<GLint>(a[3]));
        GLbitfield gl_mask = 0;
        if (mask & OOS_GLES_CLEAR_COLOR)
          gl_mask |= GL_COLOR_BUFFER_BIT;
        if (mask & OOS_GLES_CLEAR_DEPTH)
          gl_mask |= GL_DEPTH_BUFFER_BIT;
        if (mask & OOS_GLES_CLEAR_STENCIL)
          gl_mask |= GL_STENCIL_BUFFER_BIT;
        glClear(gl_mask);
        break;
      }
      case OOS_GLES_VIEWPORT:
        if (a[2] > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()) ||
            a[3] > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()))
          return false;
        glViewport(static_cast<GLint>(a[0]), static_cast<GLint>(a[1]), a[2],
                   a[3]);
        break;
      case OOS_GLES_SCISSOR:
        if (a[0] > 1)
          return false;
        if (a[0]) {
          if (a[3] >
                  static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()) ||
              a[4] > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()))
            return false;
          glEnable(GL_SCISSOR_TEST);
          glScissor(static_cast<GLint>(a[1]), static_cast<GLint>(a[2]), a[3],
                    a[4]);
        } else {
          glDisable(GL_SCISSOR_TEST);
        }
        break;
      case OOS_GLES_BLEND: {
        if (a[0] > 1)
          return false;
        if (!a[0]) {
          glDisable(GL_BLEND);
          break;
        }
        GLenum source_rgb;
        GLenum destination_rgb;
        GLenum source_alpha;
        GLenum destination_alpha;
        const GLenum rgb_equation = blendEquation(a[5]);
        const GLenum alpha_equation = blendEquation(a[6]);
        if (!blendFactor(a[1], source_rgb) ||
            !blendFactor(a[2], destination_rgb) ||
            !blendFactor(a[3], source_alpha) ||
            !blendFactor(a[4], destination_alpha) || !rgb_equation ||
            !alpha_equation)
          return false;
        glEnable(GL_BLEND);
        glBlendFuncSeparate(source_rgb, destination_rgb, source_alpha,
                            destination_alpha);
        glBlendEquationSeparate(rgb_equation, alpha_equation);
        const std::array<uint8_t, 4> constant = {
            static_cast<uint8_t>(a[7]), static_cast<uint8_t>(a[7] >> 8),
            static_cast<uint8_t>(a[7] >> 16), static_cast<uint8_t>(a[7] >> 24)};
        glBlendColor(constant[0] / 255.0f, constant[1] / 255.0f,
                     constant[2] / 255.0f, constant[3] / 255.0f);
        break;
      }
      case OOS_GLES_DEPTH: {
        if (a[0] > 1 || a[1] > 1)
          return false;
        if (!a[0]) {
          glDisable(GL_DEPTH_TEST);
        } else {
          const GLenum function = compareFunction(a[2]);
          if (!function)
            return false;
          glEnable(GL_DEPTH_TEST);
          glDepthFunc(function);
        }
        glDepthMask(a[1] ? GL_TRUE : GL_FALSE);
        break;
      }
      case OOS_GLES_COLOR_MASK:
        if (a[0] > 1 || a[1] > 1 || a[2] > 1 || a[3] > 1)
          return false;
        glColorMask(a[0] ? GL_TRUE : GL_FALSE, a[1] ? GL_TRUE : GL_FALSE,
                    a[2] ? GL_TRUE : GL_FALSE, a[3] ? GL_TRUE : GL_FALSE);
        break;
      case OOS_GLES_STENCIL:
        if (a[0] > 1)
          return false;
        if (a[0])
          glEnable(GL_STENCIL_TEST);
        else
          glDisable(GL_STENCIL_TEST);
        glStencilMaskSeparate(GL_FRONT, a[1]);
        glStencilMaskSeparate(GL_BACK, a[2]);
        break;
      case OOS_GLES_STENCIL_FUNCTION: {
        const GLenum face = cullFace(a[0]);
        const GLenum function = compareFunction(a[1]);
        if (!face || !function)
          return false;
        glStencilFuncSeparate(face, function, static_cast<GLint>(a[2]), a[3]);
        break;
      }
      case OOS_GLES_STENCIL_OPERATION: {
        const GLenum face = cullFace(a[0]);
        GLenum stencil_fail;
        GLenum depth_fail;
        GLenum pass;
        if (!face || !stencilOperation(a[1], stencil_fail) ||
            !stencilOperation(a[2], depth_fail) ||
            !stencilOperation(a[3], pass))
          return false;
        glStencilOpSeparate(face, stencil_fail, depth_fail, pass);
        break;
      }
      case OOS_GLES_RASTER: {
        const float line_width = floatBits(a[0]);
        const float polygon_factor = floatBits(a[2]);
        const float polygon_units = floatBits(a[3]);
        const float depth_near = floatBits(a[5]);
        const float depth_far = floatBits(a[6]);
        if (!std::isfinite(line_width) || line_width <= 0 ||
            !std::isfinite(polygon_factor) || !std::isfinite(polygon_units) ||
            !std::isfinite(depth_near) || !std::isfinite(depth_far) ||
            a[1] > 1 || a[4] > 1)
          return false;
        glLineWidth(line_width);
        if (a[1]) {
          glEnable(GL_POLYGON_OFFSET_FILL);
          glPolygonOffset(polygon_factor, polygon_units);
        } else {
          glDisable(GL_POLYGON_OFFSET_FILL);
        }
        if (a[4])
          glEnable(GL_DITHER);
        else
          glDisable(GL_DITHER);
        glDepthRangef(depth_near, depth_far);
        break;
      }
      case OOS_GLES_CULL: {
        if (a[0] > 1)
          return false;
        if (!a[0]) {
          glDisable(GL_CULL_FACE);
          break;
        }
        const GLenum face = cullFace(a[1]);
        if (!face || a[2] > OOS_GLES_FRONT_CW)
          return false;
        glEnable(GL_CULL_FACE);
        glCullFace(face);
        glFrontFace(a[2] == OOS_GLES_FRONT_CW ? GL_CW : GL_CCW);
        break;
      }
      case OOS_GLES_USE_PROGRAM: {
        const GLuint program = objectName(programs, a[0]);
        if (!program)
          return false;
        glUseProgram(program);
        break;
      }
      case OOS_GLES_BIND_TEXTURE: {
        const auto texture = textures.find(a[1]);
        if (a[0] >= static_cast<uint32_t>(std::max(max_texture_units, 0)) ||
            texture == textures.end())
          return false;
        glActiveTexture(GL_TEXTURE0 + a[0]);
        glBindTexture(GL_TEXTURE_2D, texture->second.name);
        break;
      }
      case OOS_GLES_BIND_VERTEX_BUFFER: {
        const auto found = buffers.find(a[0]);
        if (found == buffers.end())
          return false;
        vertex_buffer = &found->second;
        glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer->name);
        break;
      }
      case OOS_GLES_BIND_INDEX_BUFFER: {
        const auto found = buffers.find(a[0]);
        if (found == buffers.end())
          return false;
        index_buffer = &found->second;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer->name);
        break;
      }
      case OOS_GLES_VERTEX_ATTRIBUTE: {
        if (a[0] >= static_cast<uint32_t>(max_attributes))
          return false;
        if (!a[6]) {
          glDisableVertexAttribArray(a[0]);
          frame_attributes[a[0]] = {};
          break;
        }
        const GLenum type = vertexType(a[2]);
        const uint32_t type_bytes = vertexTypeBytes(type);
        if (!vertex_buffer || a[1] < 1 || a[1] > 4 || !type_bytes || a[3] > 1 ||
            a[6] > 1 ||
            a[4] > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()) ||
            a[1] > std::numeric_limits<uint32_t>::max() / type_bytes)
          return false;
        const uint32_t element_bytes = a[1] * type_bytes;
        const uint32_t stride = a[4] ? a[4] : element_bytes;
        if (a[5] > vertex_buffer->size ||
            element_bytes > vertex_buffer->size - a[5])
          return false;
        glEnableVertexAttribArray(a[0]);
        glVertexAttribPointer(
            a[0], a[1], type, a[3] ? GL_TRUE : GL_FALSE, a[4],
            reinterpret_cast<const void *>(static_cast<uintptr_t>(a[5])));
        frame_attributes[a[0]] = {true, vertex_buffer->size, element_bytes,
                                  stride, a[5]};
        break;
      }
      case OOS_GLES_UNIFORM: {
        const GLint location = static_cast<GLint>(a[0]);
        const uint32_t type = a[1];
        const uint32_t count = a[2];
        const uint32_t offset = a[3];
        constexpr std::array<uint32_t, 8> kElements = {1, 1, 2, 3, 4, 4, 9, 16};
        if (type >= kElements.size() || count == 0 ||
            count > std::numeric_limits<uint32_t>::max() / kElements[type])
          return false;
        const uint32_t words = count * kElements[type];
        if (offset > data_words || words > data_words - offset)
          return false;
        const void *values = data + offset;
        switch (type) {
        case OOS_GLES_UNIFORM_I1:
          glUniform1iv(location, count, static_cast<const GLint *>(values));
          break;
        case OOS_GLES_UNIFORM_F1:
          glUniform1fv(location, count, static_cast<const GLfloat *>(values));
          break;
        case OOS_GLES_UNIFORM_F2:
          glUniform2fv(location, count, static_cast<const GLfloat *>(values));
          break;
        case OOS_GLES_UNIFORM_F3:
          glUniform3fv(location, count, static_cast<const GLfloat *>(values));
          break;
        case OOS_GLES_UNIFORM_F4:
          glUniform4fv(location, count, static_cast<const GLfloat *>(values));
          break;
        case OOS_GLES_UNIFORM_MAT2:
          glUniformMatrix2fv(location, count, GL_FALSE,
                             static_cast<const GLfloat *>(values));
          break;
        case OOS_GLES_UNIFORM_MAT3:
          glUniformMatrix3fv(location, count, GL_FALSE,
                             static_cast<const GLfloat *>(values));
          break;
        case OOS_GLES_UNIFORM_MAT4:
          glUniformMatrix4fv(location, count, GL_FALSE,
                             static_cast<const GLfloat *>(values));
          break;
        }
        break;
      }
      case OOS_GLES_DRAW_ARRAYS: {
        GLenum primitive;
        if (!primitiveType(a[0], primitive) ||
            a[1] > static_cast<uint32_t>(std::numeric_limits<GLint>::max()) ||
            a[2] > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()))
          return false;
        if (a[2] != 0) {
          for (const VertexAttribute &attribute : frame_attributes) {
            if (!attribute.enabled)
              continue;
            const uint64_t last =
                static_cast<uint64_t>(attribute.offset) +
                (static_cast<uint64_t>(a[1]) + a[2] - 1) * attribute.stride +
                attribute.element_bytes;
            if (last > attribute.buffer_size)
              return false;
          }
        }
        glDrawArrays(primitive, static_cast<GLint>(a[1]), a[2]);
        break;
      }
      case OOS_GLES_DRAW_ELEMENTS: {
        GLenum primitive;
        const GLenum type = vertexType(a[2]);
        const uint32_t index_bytes = vertexTypeBytes(type);
        if (!primitiveType(a[0], primitive) || !index_buffer ||
            (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT))
          return false;
        if (a[1] > static_cast<uint32_t>(std::numeric_limits<GLsizei>::max()) ||
            a[3] > index_buffer->size ||
            static_cast<uint64_t>(a[1]) * index_bytes >
                index_buffer->size - a[3])
          return false;
        glDrawElements(
            primitive, a[1], type,
            reinterpret_cast<const void *>(static_cast<uintptr_t>(a[3])));
        break;
      }
      case OOS_GLES_END_FRAME:
        if (index != command_count - 1 || ended)
          return false;
        ended = true;
        break;
      default:
        return false;
      }
    }

    if (!began || !ended || glGetError() != GL_NO_ERROR)
      return false;
    for (GLint index = 0; index < max_attributes; ++index)
      glDisableVertexAttribArray(index);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    return target.presentGlesSurface();
  }

  void reset() {
    if (!current())
      return;
    for (const auto &entry : programs)
      glDeleteProgram(entry.second);
    for (const auto &entry : shaders)
      glDeleteShader(entry.second);
    for (const auto &entry : buffers)
      glDeleteBuffers(1, &entry.second.name);
    for (const auto &entry : textures)
      glDeleteTextures(1, &entry.second.name);
    programs.clear();
    shaders.clear();
    buffers.clear();
    textures.clear();
  }

  GlesFrameTarget &target;
  std::unordered_map<uint32_t, Texture> textures;
  std::unordered_map<uint32_t, Buffer> buffers;
  std::unordered_map<uint32_t, GLuint> shaders;
  std::unordered_map<uint32_t, GLuint> programs;
  std::vector<VertexAttribute> frame_attributes;
};

GlesExecutor::GlesExecutor(GlesFrameTarget &target)
    : impl_(std::make_unique<Impl>(target)) {}
GlesExecutor::~GlesExecutor() = default;

bool GlesExecutor::capabilities(OosGlesCapabilities &result) {
  return impl_->capabilities(result);
}

bool GlesExecutor::setTexture(uint32_t texture, uint32_t format, uint32_t x,
                              uint32_t y, uint32_t width, uint32_t height,
                              uint32_t row_stride, uint32_t flags,
                              const uint8_t *pixels, size_t pixel_bytes) {
  return impl_->setTexture(texture, format, x, y, width, height, row_stride,
                           flags, pixels, pixel_bytes);
}

bool GlesExecutor::freeTexture(uint32_t texture) {
  return impl_->freeTexture(texture);
}

uint32_t GlesExecutor::textureName(uint32_t texture) const {
  const auto found = impl_->textures.find(texture);
  return found == impl_->textures.end() ? 0 : found->second.name;
}

uint32_t GlesExecutor::textureFormat(uint32_t texture) const {
  const auto found = impl_->textures.find(texture);
  return found == impl_->textures.end() ? std::numeric_limits<uint32_t>::max()
                                        : found->second.format;
}

bool GlesExecutor::setBuffer(uint32_t buffer, uint32_t size, uint32_t usage,
                             const uint8_t *data, size_t data_size) {
  return impl_->setBuffer(buffer, size, usage, data, data_size);
}

bool GlesExecutor::writeBuffer(uint32_t buffer, uint32_t offset,
                               const uint8_t *data, size_t data_size) {
  return impl_->writeBuffer(buffer, offset, data, data_size);
}

bool GlesExecutor::freeBuffer(uint32_t buffer) {
  return impl_->freeBuffer(buffer);
}

bool GlesExecutor::setShader(uint32_t shader, uint32_t stage,
                             const char *source, size_t source_size) {
  return impl_->setShader(shader, stage, source, source_size);
}

bool GlesExecutor::freeShader(uint32_t shader) {
  return impl_->freeShader(shader);
}

bool GlesExecutor::setProgram(uint32_t program, uint32_t vertex_shader,
                              uint32_t fragment_shader) {
  return impl_->setProgram(program, vertex_shader, fragment_shader);
}

bool GlesExecutor::freeProgram(uint32_t program) {
  return impl_->freeProgram(program);
}

int32_t GlesExecutor::attributeLocation(uint32_t program, const char *name,
                                        size_t name_size) {
  return impl_->location(program, name, name_size, true);
}

int32_t GlesExecutor::uniformLocation(uint32_t program, const char *name,
                                      size_t name_size) {
  return impl_->location(program, name, name_size, false);
}

bool GlesExecutor::submit(const OosGlesCommand *commands, size_t command_count,
                          const uint32_t *data, size_t data_words) {
  return impl_->submit(commands, command_count, data, data_words);
}

void GlesExecutor::reset() { impl_->reset(); }

} // namespace oos::runtime
