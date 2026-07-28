#include "oos/local/local_display.h"

#include <SDL3/SDL.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <unistd.h>
#include <unordered_map>

namespace oos::local {
namespace {

GLuint compileShader(GLenum type, const char *source) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE)
    return shader;
  std::array<char, 512> log{};
  glGetShaderInfoLog(shader, log.size(), nullptr, log.data());
  std::fprintf(stderr, "OOS local shader compilation failed: %s\n", log.data());
  glDeleteShader(shader);
  return 0;
}

bool finiteRect(const OosGfxDrawCommand &command) {
  return std::isfinite(command.clip_min[0]) &&
         std::isfinite(command.clip_min[1]) &&
         std::isfinite(command.clip_max[0]) &&
         std::isfinite(command.clip_max[1]);
}

} // namespace

class LocalDisplay::Impl {
public:
  struct Texture {
    GLuint name = 0;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  ~Impl() { shutdown(); }

  bool initialize() {
    if (initialized_)
      return true;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
      std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
      return false;
    }
    sdl_started_ = true;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    window_ = SDL_CreateWindow("Orange OS - local", LocalDisplay::kWidth,
                               LocalDisplay::kHeight, SDL_WINDOW_OPENGL);
    if (!window_) {
      std::fprintf(stderr, "create local window failed: %s\n", SDL_GetError());
      return false;
    }
    SDL_SetWindowMinimumSize(window_, LocalDisplay::kWidth,
                             LocalDisplay::kHeight);
    SDL_SetWindowMaximumSize(window_, LocalDisplay::kWidth,
                             LocalDisplay::kHeight);
    context_ = SDL_GL_CreateContext(window_);
    if (!context_ || !SDL_GL_MakeCurrent(window_, context_)) {
      std::fprintf(stderr, "create local GLES context failed: %s\n",
                   SDL_GetError());
      return false;
    }
    SDL_GL_SetSwapInterval(1);
    if (!initializeProgram())
      return false;
    const char *extensions =
        reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
    bgra_supported_ =
        extensions && std::strstr(extensions, "GL_EXT_texture_format_BGRA8888");
    glViewport(0, 0, LocalDisplay::kWidth, LocalDisplay::kHeight);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window_);
    initialized_ = true;
    std::fprintf(stderr,
                 "OOS local display initialized: %ux%u, renderer=%s, GLES=%s\n",
                 LocalDisplay::kWidth, LocalDisplay::kHeight,
                 glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return true;
  }

  bool initializeProgram() {
    constexpr char kVertexShader[] =
        "attribute vec2 aPosition;\n"
        "attribute vec2 aTexcoord;\n"
        "attribute vec4 aColor;\n"
        "uniform vec2 uScreenSize;\n"
        "varying vec2 vTexcoord;\n"
        "varying vec4 vColor;\n"
        "void main() {\n"
        "  vec2 p = aPosition / uScreenSize * 2.0 - 1.0;\n"
        "  gl_Position = vec4(p.x, -p.y, 0.0, 1.0);\n"
        "  vTexcoord = aTexcoord;\n"
        "  vColor = aColor;\n"
        "}\n";
    constexpr char kFragmentShader[] =
        "precision mediump float;\n"
        "uniform sampler2D uTexture;\n"
        "varying vec2 vTexcoord;\n"
        "varying vec4 vColor;\n"
        "void main() { gl_FragColor = vColor * texture2D(uTexture, "
        "vTexcoord); }\n";
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertex || !fragment)
      return false;
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glBindAttribLocation(program_, 0, "aPosition");
    glBindAttribLocation(program_, 1, "aTexcoord");
    glBindAttribLocation(program_, 2, "aColor");
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    screen_size_uniform_ = glGetUniformLocation(program_, "uScreenSize");
    texture_uniform_ = glGetUniformLocation(program_, "uTexture");
    return linked == GL_TRUE && screen_size_uniform_ >= 0 &&
           texture_uniform_ >= 0;
  }

  bool makeCurrent() {
    return initialized_ && SDL_GL_MakeCurrent(window_, context_);
  }

  bool setTexture(uint32_t handle, uint32_t x, uint32_t y, uint32_t width,
                  uint32_t height, uint32_t flags, const uint8_t *rgba,
                  size_t rgba_size) {
    if (!makeCurrent() || handle == 0 || !rgba || width == 0 || height == 0 ||
        rgba_size != static_cast<size_t>(width) * height * 4)
      return false;
    auto existing = textures_.find(handle);
    const bool replace = (flags & OOS_TEXTURE_REPLACE) != 0;
    if (existing == textures_.end()) {
      if (x != 0 || y != 0)
        return false;
      Texture texture;
      glGenTextures(1, &texture.name);
      texture.width = width;
      texture.height = height;
      existing = textures_.emplace(handle, texture).first;
      glBindTexture(GL_TEXTURE_2D, texture.name);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, rgba);
    } else if (replace) {
      if (x != 0 || y != 0)
        return false;
      existing->second.width = width;
      existing->second.height = height;
      glBindTexture(GL_TEXTURE_2D, existing->second.name);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, rgba);
    } else {
      Texture &texture = existing->second;
      if (x > texture.width || y > texture.height ||
          width > texture.width - x || height > texture.height - y)
        return false;
      glBindTexture(GL_TEXTURE_2D, texture.name);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA,
                      GL_UNSIGNED_BYTE, rgba);
    }
    const GLint filter = (flags & OOS_TEXTURE_LINEAR) ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return glGetError() == GL_NO_ERROR;
  }

  bool freeTexture(uint32_t handle) {
    if (!makeCurrent())
      return false;
    const auto texture = textures_.find(handle);
    if (texture == textures_.end())
      return true;
    glDeleteTextures(1, &texture->second.name);
    textures_.erase(texture);
    return glGetError() == GL_NO_ERROR;
  }

  void prepareDraw(const OosGfxVertex *vertices) {
    glUseProgram(program_);
    glUniform2f(screen_size_uniform_, LocalDisplay::kWidth,
                LocalDisplay::kHeight);
    glUniform1i(texture_uniform_, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OosGfxVertex),
                          &vertices[0].position);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(OosGfxVertex),
                          &vertices[0].uv);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(OosGfxVertex),
                          &vertices[0].color);
  }

  void finishDraw() {
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
  }

  bool submit(const OosGfxVertex *vertices, size_t vertex_count,
              const uint16_t *indices, size_t index_count,
              const OosGfxDrawCommand *commands, size_t command_count,
              uint32_t clear_rgba) {
    if (!makeCurrent() || (vertex_count && !vertices) ||
        (index_count && !indices) || (command_count && !commands))
      return false;
    for (size_t i = 0; i < command_count; ++i) {
      const OosGfxDrawCommand &command = commands[i];
      if (!finiteRect(command) || command.first_index > index_count ||
          command.index_count > index_count - command.first_index ||
          textures_.find(command.texture) == textures_.end())
        return false;
    }
    for (size_t i = 0; i < index_count; ++i) {
      if (indices[i] >= vertex_count)
        return false;
    }
    const std::array<uint8_t, 4> clear = {
        static_cast<uint8_t>(clear_rgba),
        static_cast<uint8_t>(clear_rgba >> 8),
        static_cast<uint8_t>(clear_rgba >> 16),
        static_cast<uint8_t>(clear_rgba >> 24),
    };
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, LocalDisplay::kWidth, LocalDisplay::kHeight);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(clear[0] / 255.0f, clear[1] / 255.0f, clear[2] / 255.0f,
                 clear[3] / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    if (vertex_count)
      prepareDraw(vertices);
    for (size_t i = 0; i < command_count; ++i) {
      const OosGfxDrawCommand &command = commands[i];
      const int min_x = std::clamp(static_cast<int>(command.clip_min[0]), 0,
                                   static_cast<int>(LocalDisplay::kWidth));
      const int min_y = std::clamp(static_cast<int>(command.clip_min[1]), 0,
                                   static_cast<int>(LocalDisplay::kHeight));
      const int max_x =
          std::clamp(static_cast<int>(std::ceil(command.clip_max[0])), min_x,
                     static_cast<int>(LocalDisplay::kWidth));
      const int max_y =
          std::clamp(static_cast<int>(std::ceil(command.clip_max[1])), min_y,
                     static_cast<int>(LocalDisplay::kHeight));
      if (max_x == min_x || max_y == min_y)
        continue;
      glScissor(min_x, LocalDisplay::kHeight - max_y, max_x - min_x,
                max_y - min_y);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, textures_.at(command.texture).name);
      glDrawElements(GL_TRIANGLES, command.index_count, GL_UNSIGNED_SHORT,
                     indices + command.first_index);
    }
    if (vertex_count)
      finishDraw();
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    if (glGetError() != GL_NO_ERROR)
      return false;
    SDL_GL_SwapWindow(window_);
    return true;
  }

  bool drawFullscreen(GLuint texture) {
    constexpr std::array<OosGfxVertex, 4> vertices = {{
        {{0, 0}, {0, 0}, {255, 255, 255, 255}},
        {{240, 0}, {1, 0}, {255, 255, 255, 255}},
        {{0, 320}, {0, 1}, {255, 255, 255, 255}},
        {{240, 320}, {1, 1}, {255, 255, 255, 255}},
    }};
    constexpr std::array<uint16_t, 6> indices = {0, 1, 2, 2, 1, 3};
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, LocalDisplay::kWidth, LocalDisplay::kHeight);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    prepareDraw(vertices.data());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glDrawElements(GL_TRIANGLES, indices.size(), GL_UNSIGNED_SHORT,
                   indices.data());
    finishDraw();
    if (glGetError() != GL_NO_ERROR)
      return false;
    SDL_GL_SwapWindow(window_);
    return true;
  }

  bool showBootFrame(const uint16_t *pixels) {
    if (!makeCurrent() || !pixels)
      return false;
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, LocalDisplay::kWidth,
                 LocalDisplay::kHeight, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                 pixels);
    const bool success = glGetError() == GL_NO_ERROR && drawFullscreen(texture);
    glDeleteTextures(1, &texture);
    return success;
  }

  bool presentSurface(const compositor::SurfaceFrame &frame) {
    if (!makeCurrent() || !frame.buffer ||
        frame.buffer_width != LocalDisplay::kWidth ||
        frame.buffer_height != LocalDisplay::kHeight) {
      if (frame.acquire_fence_fd >= 0)
        close(frame.acquire_fence_fd);
      return false;
    }
    if (frame.acquire_fence_fd >= 0) {
      pollfd descriptor{frame.acquire_fence_fd, POLLIN, 0};
      int result;
      do {
        result = ::poll(&descriptor, 1, 3000);
      } while (result < 0 && errno == EINTR);
      close(frame.acquire_fence_fd);
      if (result <= 0)
        return false;
    }
    if (frame.buffer_type ==
        compositor::NativeBufferType::SharedMemoryArgb8888) {
      if (frame.buffer_stride != frame.buffer_width * 4)
        return false;
      if (!bgra_supported_)
        return false;
      if (!surface_texture_) {
        glGenTextures(1, &surface_texture_);
        glBindTexture(GL_TEXTURE_2D, surface_texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, frame.buffer_width,
                     frame.buffer_height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE,
                     frame.buffer);
      } else {
        glBindTexture(GL_TEXTURE_2D, surface_texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.buffer_width,
                        frame.buffer_height, GL_BGRA_EXT, GL_UNSIGNED_BYTE,
                        frame.buffer);
      }
      return glGetError() == GL_NO_ERROR && drawFullscreen(surface_texture_);
    }
    return false;
  }

  void shutdown() {
    if (context_ && window_)
      SDL_GL_MakeCurrent(window_, context_);
    for (const auto &entry : textures_)
      glDeleteTextures(1, &entry.second.name);
    textures_.clear();
    if (surface_texture_)
      glDeleteTextures(1, &surface_texture_);
    surface_texture_ = 0;
    if (program_)
      glDeleteProgram(program_);
    program_ = 0;
    if (context_)
      SDL_GL_DestroyContext(context_);
    context_ = nullptr;
    if (window_)
      SDL_DestroyWindow(window_);
    window_ = nullptr;
    if (sdl_started_)
      SDL_Quit();
    sdl_started_ = false;
    bgra_supported_ = false;
    initialized_ = false;
  }

private:
  SDL_Window *window_ = nullptr;
  SDL_GLContext context_ = nullptr;
  GLuint program_ = 0;
  GLint screen_size_uniform_ = -1;
  GLint texture_uniform_ = -1;
  GLuint surface_texture_ = 0;
  std::unordered_map<uint32_t, Texture> textures_;
  bool sdl_started_ = false;
  bool bgra_supported_ = false;
  bool initialized_ = false;
};

LocalDisplay::LocalDisplay() : impl_(std::make_unique<Impl>()) {}
LocalDisplay::~LocalDisplay() = default;

bool LocalDisplay::initialize() { return impl_->initialize(); }
bool LocalDisplay::showBootFrame(const uint16_t *pixels) {
  return impl_->showBootFrame(pixels);
}
bool LocalDisplay::presentSurface(const compositor::SurfaceFrame &frame) {
  return impl_->presentSurface(frame);
}
void LocalDisplay::refresh() {}
void LocalDisplay::shutdown() { impl_->shutdown(); }
uint32_t LocalDisplay::width() const { return kWidth; }
uint32_t LocalDisplay::height() const { return kHeight; }
bool LocalDisplay::setTexture(uint32_t texture, uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height, uint32_t flags,
                              const uint8_t *rgba, size_t rgba_size) {
  return impl_->setTexture(texture, x, y, width, height, flags, rgba,
                           rgba_size);
}
bool LocalDisplay::freeTexture(uint32_t texture) {
  return impl_->freeTexture(texture);
}
bool LocalDisplay::submit(const OosGfxVertex *vertices, size_t vertex_count,
                          const uint16_t *indices, size_t index_count,
                          const OosGfxDrawCommand *commands,
                          size_t command_count, uint32_t clear_rgba) {
  return impl_->submit(vertices, vertex_count, indices, index_count, commands,
                       command_count, clear_rgba);
}

} // namespace oos::local
