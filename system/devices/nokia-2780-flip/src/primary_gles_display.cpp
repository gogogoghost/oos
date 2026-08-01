#include "oos/nokia2780/primary_gles_display.h"

#include "oos/runtime/gles_executor.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <android/hardware/power/1.0/IPower.h>
#include <binder/ProcessState.h>
#include <hardware/gralloc.h>
#include <sync/sync.h>
#include <ui/Fence.h>
#include <ui/FloatRect.h>
#include <ui/GraphicBuffer.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <utils/Errors.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

struct AHardwareBuffer;
extern "C" {
void AHardwareBuffer_acquire(AHardwareBuffer *buffer);
void AHardwareBuffer_release(AHardwareBuffer *buffer);
EGLClientBuffer EGLAPIENTRY
eglGetNativeClientBufferANDROID(const AHardwareBuffer *buffer);
}

#include "HWC2.h"
#include "oos/nokia2780/display_control.h"

namespace oos::nokia2780 {
namespace {

constexpr useconds_t kPanelTransferSettleUs = 150000;
constexpr size_t kTargetBufferCount = 2;
constexpr char kPrimaryBacklight[] = "/sys/class/leds/lcd-backlight/brightness";

class DisplayCallback final : public HWC2::ComposerCallback {
public:
  explicit DisplayCallback(HWC2::Device *device) : device_(device) {}

  void onHotplugReceived(int32_t, hwc2_display_t display,
                         HWC2::Connection connection) override {
    device_->onHotplug(display, connection);
  }
  void onRefreshReceived(int32_t, hwc2_display_t) override {}
  void onVsyncReceived(int32_t, hwc2_display_t, int64_t) override {}

private:
  HWC2::Device *const device_;
};

bool setBacklight(int value) {
  FILE *file = std::fopen(kPrimaryBacklight, "w");
  if (!file)
    return false;
  const bool success = std::fprintf(file, "%d\n", value) > 0;
  std::fclose(file);
  return success;
}

bool checkEgl(EGLBoolean result, const char *operation) {
  if (result == EGL_TRUE)
    return true;
  std::fprintf(stderr, "%s failed: 0x%x\n", operation, eglGetError());
  return false;
}

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
  std::fprintf(stderr, "OOS graphics shader compilation failed: %s\n",
               log.data());
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

class PrimaryGlesDisplay::Impl final : public runtime::GlesFrameTarget {
public:
  struct TargetBuffer {
    android::sp<android::GraphicBuffer> buffer;
    android::sp<android::Fence> present_fence;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
  };

  Impl() : gles_(*this) {}

  ~Impl() { shutdown(); }

  bool detachRenderContext() {
    return egl_display_ != EGL_NO_DISPLAY &&
           eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                          EGL_NO_CONTEXT) == EGL_TRUE;
  }

  bool attachRenderContext() {
    return egl_display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT &&
           pbuffer_ != EGL_NO_SURFACE &&
           eglMakeCurrent(egl_display_, pbuffer_, pbuffer_, context_) ==
               EGL_TRUE;
  }

  bool initialize() {
    if (initialized_)
      return true;
    power_ = ::android::hardware::power::V1_0::IPower::getService();
    if (power_ == nullptr || !power_->setInteractive(true).isOk() ||
        !setBacklight(0) || nokia2780_prepare_primary() != 0) {
      return false;
    }
    owns_display_state_ = true;
    android::ProcessState::self()->startThreadPool();
    device_ = std::make_unique<HWC2::Device>("default");
    callback_ = std::make_unique<DisplayCallback>(device_.get());
    device_->registerCallback(callback_.get(), 0);
    display_ = device_->getDisplayById(HWC_DISPLAY_PRIMARY);
    if (!display_ || !display_->isConnected() ||
        display_->setPowerMode(HWC2::PowerMode::On) != HWC2::Error::None) {
      return false;
    }

    const android::Rect frame(0, 0, PrimaryGlesDisplay::kWidth,
                              PrimaryGlesDisplay::kHeight);
    if (display_->createLayer(&layer_) != HWC2::Error::None || !layer_ ||
        layer_->setCompositionType(HWC2::Composition::Client) !=
            HWC2::Error::None ||
        layer_->setBlendMode(HWC2::BlendMode::None) != HWC2::Error::None ||
        layer_->setSourceCrop(android::FloatRect(
            0, 0, PrimaryGlesDisplay::kWidth, PrimaryGlesDisplay::kHeight)) !=
            HWC2::Error::None ||
        layer_->setDisplayFrame(frame) != HWC2::Error::None ||
        layer_->setVisibleRegion(android::Region(frame)) != HWC2::Error::None ||
        layer_->setPlaneAlpha(1.0f) != HWC2::Error::None ||
        layer_->setZOrder(0) != HWC2::Error::None) {
      return false;
    }

    for (TargetBuffer &target : targets_) {
      target.buffer = new android::GraphicBuffer(
          PrimaryGlesDisplay::kWidth, PrimaryGlesDisplay::kHeight,
          android::PIXEL_FORMAT_RGB_565, 1,
          static_cast<uint64_t>(GRALLOC_USAGE_HW_RENDER |
                                GRALLOC_USAGE_HW_COMPOSER),
          "oos-wasm-primary-target");
      if (target.buffer->initCheck() != android::NO_ERROR ||
          !target.buffer->handle) {
        return false;
      }
    }
    if (!initializeEgl() || !initializeProgram() || !beginFrame())
      return false;

    glViewport(0, 0, PrimaryGlesDisplay::kWidth, PrimaryGlesDisplay::kHeight);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    if (glGetError() != GL_NO_ERROR || !presentTarget())
      return false;
    usleep(kPanelTransferSettleUs);
    initialized_ = true;
    std::fprintf(stderr,
                 "OOS GLES display initialized: RGB565 %ux%u targets=%zu, "
                 "GLES=%s\n",
                 PrimaryGlesDisplay::kWidth, PrimaryGlesDisplay::kHeight,
                 targets_.size(), glGetString(GL_VERSION));
    return true;
  }

  bool initializeEgl() {
    egl_display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display_ == EGL_NO_DISPLAY ||
        !checkEgl(eglInitialize(egl_display_, nullptr, nullptr),
                  "eglInitialize")) {
      return false;
    }
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE,
        EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!checkEgl(eglChooseConfig(egl_display_, config_attributes, &config, 1,
                                  &count),
                  "eglChooseConfig") ||
        count == 0) {
      return false;
    }
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                         EGL_NONE};
    context_ = eglCreateContext(egl_display_, config, EGL_NO_CONTEXT,
                                context_attributes);
    const EGLint pbuffer_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    pbuffer_ =
        eglCreatePbufferSurface(egl_display_, config, pbuffer_attributes);
    if (context_ == EGL_NO_CONTEXT || pbuffer_ == EGL_NO_SURFACE ||
        !checkEgl(eglMakeCurrent(egl_display_, pbuffer_, pbuffer_, context_),
                  "eglMakeCurrent")) {
      return false;
    }
    create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    image_target_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!create_image_ || !destroy_image_ || !image_target_)
      return false;
    for (TargetBuffer &target : targets_) {
      target.image = create_image_(
          egl_display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
          reinterpret_cast<EGLClientBuffer>(target.buffer->getNativeBuffer()),
          nullptr);
      if (target.image == EGL_NO_IMAGE_KHR)
        return false;
      glGenTextures(1, &target.texture);
      glBindTexture(GL_TEXTURE_2D, target.texture);
      image_target_(GL_TEXTURE_2D,
                    reinterpret_cast<GLeglImageOES>(target.image));
    }
    glGenFramebuffers(1, &framebuffer_);
    return bindTarget(render_target_);
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
        "  vec2 position = aPosition / uScreenSize * 2.0 - 1.0;\n"
        "  gl_Position = vec4(position, 0.0, 1.0);\n"
        "  vTexcoord = aTexcoord;\n"
        "  vColor = aColor;\n"
        "}\n";
    constexpr char kFragmentShader[] =
        "precision mediump float;\n"
        "uniform sampler2D uTexture;\n"
        "uniform int uTextureFormat;\n"
        "varying vec2 vTexcoord;\n"
        "varying vec4 vColor;\n"
        "void main() {\n"
        "  vec4 sampled = texture2D(uTexture, vTexcoord);\n"
        "  if (uTextureFormat == 0) sampled = vec4(sampled.a);\n"
        "  gl_FragColor = vColor * sampled;\n"
        "}\n";
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
    texture_format_uniform_ = glGetUniformLocation(program_, "uTextureFormat");
    return linked == GL_TRUE && screen_size_uniform_ >= 0 &&
           texture_uniform_ >= 0 && texture_format_uniform_ >= 0;
  }

  bool setTexture(uint32_t handle, uint32_t format, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t row_stride,
                  uint32_t flags, const uint8_t *pixels, size_t pixel_bytes) {
    return gles_.setTexture(handle, format, x, y, width, height, row_stride,
                            flags, pixels, pixel_bytes);
  }

  bool freeTexture(uint32_t handle) { return gles_.freeTexture(handle); }

  bool submit(const OosGfxVertex *vertices, size_t vertex_count,
              const uint16_t *indices, size_t index_count,
              const OosGfxDrawCommand *commands, size_t command_count,
              uint32_t clear_rgba) {
    if (!initialized_ || (vertex_count && !vertices) ||
        (index_count && !indices) || (command_count && !commands)) {
      return false;
    }
    for (size_t command_index = 0; command_index < command_count;
         ++command_index) {
      const OosGfxDrawCommand &command = commands[command_index];
      if (!finiteRect(command) || command.first_index > index_count ||
          command.index_count > index_count - command.first_index ||
          gles_.textureName(command.texture) == 0) {
        return false;
      }
    }
    for (size_t index = 0; index < index_count; ++index) {
      if (indices[index] >= vertex_count)
        return false;
    }

    const std::array<uint8_t, 4> clear = {
        static_cast<uint8_t>(clear_rgba),
        static_cast<uint8_t>(clear_rgba >> 8),
        static_cast<uint8_t>(clear_rgba >> 16),
        static_cast<uint8_t>(clear_rgba >> 24),
    };
    if (!beginFrame())
      return false;
    glViewport(0, 0, PrimaryGlesDisplay::kWidth, PrimaryGlesDisplay::kHeight);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(clear[0] / 255.0f, clear[1] / 255.0f, clear[2] / 255.0f,
                 clear[3] / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program_);
    glUniform2f(screen_size_uniform_, PrimaryGlesDisplay::kWidth,
                PrimaryGlesDisplay::kHeight);
    glUniform1i(texture_uniform_, 0);
    glUniform1i(texture_format_uniform_, OOS_TEXTURE_RGBA8888);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    if (vertex_count) {
      glEnableVertexAttribArray(0);
      glEnableVertexAttribArray(1);
      glEnableVertexAttribArray(2);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OosGfxVertex),
                            &vertices[0].position);
      glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(OosGfxVertex),
                            &vertices[0].uv);
      glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                            sizeof(OosGfxVertex), &vertices[0].color);
    }

    for (size_t command_index = 0; command_index < command_count;
         ++command_index) {
      const OosGfxDrawCommand &command = commands[command_index];
      const int min_x =
          std::clamp(static_cast<int>(command.clip_min[0]), 0,
                     static_cast<int>(PrimaryGlesDisplay::kWidth));
      const int min_y =
          std::clamp(static_cast<int>(command.clip_min[1]), 0,
                     static_cast<int>(PrimaryGlesDisplay::kHeight));
      const int max_x =
          std::clamp(static_cast<int>(std::ceil(command.clip_max[0])), min_x,
                     static_cast<int>(PrimaryGlesDisplay::kWidth));
      const int max_y =
          std::clamp(static_cast<int>(std::ceil(command.clip_max[1])), min_y,
                     static_cast<int>(PrimaryGlesDisplay::kHeight));
      if (max_x == min_x || max_y == min_y)
        continue;
      glScissor(min_x, min_y, max_x - min_x, max_y - min_y);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, gles_.textureName(command.texture));
      glUniform1i(texture_format_uniform_,
                  static_cast<GLint>(gles_.textureFormat(command.texture)));
      glDrawElements(GL_TRIANGLES, command.index_count, GL_UNSIGNED_SHORT,
                     indices + command.first_index);
    }
    if (vertex_count) {
      glDisableVertexAttribArray(2);
      glDisableVertexAttribArray(1);
      glDisableVertexAttribArray(0);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glFinish();
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
      std::fprintf(stderr, "OOS WASM frame GL error: 0x%x\n", error);
      return false;
    }
    return presentAndReveal();
  }

  bool showBootFrame(const uint16_t *pixels) {
    if (!initialized_ || !pixels)
      return false;
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, PrimaryGlesDisplay::kWidth,
                 PrimaryGlesDisplay::kHeight, 0, GL_RGB,
                 GL_UNSIGNED_SHORT_5_6_5, pixels);

    constexpr OosGfxVertex vertices[] = {
        {{0, 0}, {0, 0}, {255, 255, 255, 255}},
        {{240, 0}, {1, 0}, {255, 255, 255, 255}},
        {{0, 320}, {0, 1}, {255, 255, 255, 255}},
        {{240, 320}, {1, 1}, {255, 255, 255, 255}},
    };
    constexpr uint16_t indices[] = {0, 1, 2, 2, 1, 3};
    if (!beginFrame()) {
      glDeleteTextures(1, &texture);
      return false;
    }
    glViewport(0, 0, PrimaryGlesDisplay::kWidth, PrimaryGlesDisplay::kHeight);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(program_);
    glUniform2f(screen_size_uniform_, PrimaryGlesDisplay::kWidth,
                PrimaryGlesDisplay::kHeight);
    glUniform1i(texture_uniform_, 0);
    glUniform1i(texture_format_uniform_, OOS_TEXTURE_RGBA8888);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OosGfxVertex),
                          &vertices[0].position);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(OosGfxVertex),
                          &vertices[0].uv);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(OosGfxVertex),
                          &vertices[0].color);
    glDrawElements(GL_TRIANGLES, std::size(indices), GL_UNSIGNED_SHORT,
                   indices);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
    glFinish();
    glDeleteTextures(1, &texture);
    return glGetError() == GL_NO_ERROR && presentAndReveal();
  }

  bool presentSurface(const compositor::SurfaceFrame &frame) {
    if (!initialized_ || !frame.buffer ||
        frame.buffer_type !=
            compositor::NativeBufferType::AndroidHardwareBuffer ||
        frame.buffer_width != PrimaryGlesDisplay::kWidth ||
        frame.buffer_height != PrimaryGlesDisplay::kHeight || !beginFrame()) {
      if (frame.acquire_fence_fd >= 0)
        close(frame.acquire_fence_fd);
      return false;
    }
    if (frame.acquire_fence_fd >= 0) {
      const int result = sync_wait(frame.acquire_fence_fd, 3000);
      close(frame.acquire_fence_fd);
      if (result != 0) {
        std::fprintf(stderr, "OOS external buffer acquire fence timeout\n");
        return false;
      }
    }
    ExternalSurface *external =
        externalSurface(static_cast<AHardwareBuffer *>(frame.buffer));
    if (!external)
      return false;
    glBindTexture(GL_TEXTURE_2D, external->texture);

    constexpr OosGfxVertex vertices[] = {
        {{0, 0}, {0, 0}, {255, 255, 255, 255}},
        {{240, 0}, {1, 0}, {255, 255, 255, 255}},
        {{0, 320}, {0, 1}, {255, 255, 255, 255}},
        {{240, 320}, {1, 1}, {255, 255, 255, 255}},
    };
    constexpr uint16_t indices[] = {0, 1, 2, 2, 1, 3};
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, PrimaryGlesDisplay::kWidth, PrimaryGlesDisplay::kHeight);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(program_);
    glUniform2f(screen_size_uniform_, PrimaryGlesDisplay::kWidth,
                PrimaryGlesDisplay::kHeight);
    glUniform1i(texture_uniform_, 0);
    glUniform1i(texture_format_uniform_, OOS_TEXTURE_RGBA8888);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OosGfxVertex),
                          &vertices[0].position);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(OosGfxVertex),
                          &vertices[0].uv);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(OosGfxVertex),
                          &vertices[0].color);
    glDrawElements(GL_TRIANGLES, std::size(indices), GL_UNSIGNED_SHORT,
                   indices);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);
    glFinish();
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
      std::fprintf(stderr, "OOS external surface GL error: 0x%x\n", error);
      return false;
    }
    return presentAndReveal();
  }

  bool presentAndReveal() {
    if (!presentTarget())
      return false;
    if (!revealed_) {
      usleep(kPanelTransferSettleUs);
      if (!repeatLastTarget())
        return false;
      usleep(kPanelTransferSettleUs);
      if (!setBacklight(255))
        return false;
      revealed_ = true;
      std::fprintf(stderr, "OOS primary display revealed\n");
      std::fflush(stderr);
    }
    return true;
  }

  bool waitPresentFence(size_t target_index) {
    if (target_index >= targets_.size() ||
        !targets_[target_index].present_fence ||
        !targets_[target_index].present_fence->isValid()) {
      return true;
    }
    const int result = targets_[target_index].present_fence->waitForever(
        "oos-compositor-present");
    targets_[target_index].present_fence.clear();
    if (result == android::NO_ERROR)
      return true;
    std::fprintf(stderr, "OOS HWC2 present fence wait failed: %d\n", result);
    return false;
  }

  bool presentTarget() {
    const size_t target_index = render_target_;
    if (!presentTarget(target_index))
      return false;
    last_presented_target_ = target_index;
    render_target_ = (render_target_ + 1) % targets_.size();
    return true;
  }

  bool repeatLastTarget() {
    return last_presented_target_ < targets_.size() &&
           presentTarget(last_presented_target_);
  }

  bool presentTarget(size_t target_index) {
    if (target_index >= targets_.size() || !targets_[target_index].buffer ||
        !waitPresentFence(target_index)) {
      return false;
    }
    if (display_->setClientTarget(
            0, targets_[target_index].buffer, android::Fence::NO_FENCE,
            android::ui::Dataspace::UNKNOWN) != HWC2::Error::None) {
      return false;
    }
    uint32_t changes = 0;
    uint32_t requests = 0;
    const HWC2::Error validation = display_->validate(&changes, &requests);
    if (validation == HWC2::Error::HasChanges &&
        display_->acceptChanges() != HWC2::Error::None) {
      return false;
    }
    if ((validation != HWC2::Error::None &&
         validation != HWC2::Error::HasChanges) ||
        display_->present(&targets_[target_index].present_fence) !=
            HWC2::Error::None) {
      std::fprintf(
          stderr,
          "OOS HWC present failed: validate=%d changes=%u requests=%u\n",
          static_cast<int>(validation), changes, requests);
      return false;
    }
    return true;
  }

  bool bindTarget(size_t target_index) {
    if (target_index >= targets_.size() || !framebuffer_ ||
        !targets_[target_index].texture) {
      return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           targets_[target_index].texture, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  }

  bool beginFrame() {
    return waitPresentFence(render_target_) && bindTarget(render_target_);
  }

  void waitAllPresentFences() {
    for (size_t index = 0; index < targets_.size(); ++index)
      waitPresentFence(index);
  }

  void refresh() {
    if (initialized_ && last_presented_target_ < targets_.size())
      repeatLastTarget();
  }

  void shutdown() {
    if (owns_display_state_)
      setBacklight(0);
    waitAllPresentFences();
    const bool context_current =
        egl_display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT &&
        pbuffer_ != EGL_NO_SURFACE &&
        eglMakeCurrent(egl_display_, pbuffer_, pbuffer_, context_) == EGL_TRUE;
    if (context_current) {
      clearExternalSurfaces(true);
      gles_.reset();
      if (program_)
        glDeleteProgram(program_);
      if (stencil_buffer_)
        glDeleteRenderbuffers(1, &stencil_buffer_);
      if (depth_buffer_)
        glDeleteRenderbuffers(1, &depth_buffer_);
      if (framebuffer_)
        glDeleteFramebuffers(1, &framebuffer_);
      for (TargetBuffer &target : targets_) {
        if (target.texture)
          glDeleteTextures(1, &target.texture);
      }
    } else
      clearExternalSurfaces(false);
    if (destroy_image_) {
      for (TargetBuffer &target : targets_) {
        if (target.image != EGL_NO_IMAGE_KHR)
          destroy_image_(egl_display_, target.image);
      }
    }
    if (egl_display_ != EGL_NO_DISPLAY) {
      eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                     EGL_NO_CONTEXT);
      if (pbuffer_ != EGL_NO_SURFACE)
        eglDestroySurface(egl_display_, pbuffer_);
      if (context_ != EGL_NO_CONTEXT)
        eglDestroyContext(egl_display_, context_);
      eglTerminate(egl_display_);
    }
    if (display_ && layer_)
      (void)display_->destroyLayer(layer_);
    if (display_ && owns_display_state_)
      (void)display_->setPowerMode(HWC2::PowerMode::Off);
    if (power_ && owns_display_state_)
      power_->setInteractive(false);
    for (TargetBuffer &target : targets_)
      target = {};
    callback_.reset();
    device_.reset();
    display_ = nullptr;
    layer_ = nullptr;
    power_.clear();
    egl_display_ = EGL_NO_DISPLAY;
    context_ = EGL_NO_CONTEXT;
    pbuffer_ = EGL_NO_SURFACE;
    framebuffer_ = 0;
    depth_buffer_ = 0;
    stencil_buffer_ = 0;
    program_ = 0;
    initialized_ = false;
    revealed_ = false;
    owns_display_state_ = false;
    render_target_ = 0;
    last_presented_target_ = targets_.size();
  }

private:
  friend class PrimaryGlesDisplay;

  struct ExternalSurface {
    AHardwareBuffer *buffer = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    uint64_t last_used = 0;
  };

  void destroyExternalSurface(ExternalSurface &surface, bool destroy_egl) {
    if (destroy_egl && surface.texture)
      glDeleteTextures(1, &surface.texture);
    if (destroy_egl && surface.image != EGL_NO_IMAGE_KHR && destroy_image_)
      destroy_image_(egl_display_, surface.image);
    if (surface.buffer)
      AHardwareBuffer_release(surface.buffer);
    surface = {};
  }

  void clearExternalSurfaces(bool destroy_egl) {
    if (destroy_egl && !external_surfaces_.empty())
      glFinish();
    for (ExternalSurface &surface : external_surfaces_)
      destroyExternalSurface(surface, destroy_egl);
    external_surfaces_.clear();
  }

  ExternalSurface *externalSurface(AHardwareBuffer *buffer) {
    for (ExternalSurface &surface : external_surfaces_) {
      if (surface.buffer == buffer) {
        surface.last_used = ++external_surface_epoch_;
        return &surface;
      }
    }

    constexpr size_t kMaximumCachedSurfaces = 8;
    if (external_surfaces_.size() >= kMaximumCachedSurfaces) {
      auto oldest = std::min_element(
          external_surfaces_.begin(), external_surfaces_.end(),
          [](const ExternalSurface &left, const ExternalSurface &right) {
            return left.last_used < right.last_used;
          });
      glFinish();
      destroyExternalSurface(*oldest, true);
      external_surfaces_.erase(oldest);
    }

    EGLClientBuffer native = eglGetNativeClientBufferANDROID(buffer);
    if (!native) {
      std::fprintf(stderr, "OOS failed to obtain Android native buffer\n");
      return nullptr;
    }
    ExternalSurface surface;
    surface.buffer = buffer;
    AHardwareBuffer_acquire(buffer);
    surface.image = create_image_(egl_display_, EGL_NO_CONTEXT,
                                  EGL_NATIVE_BUFFER_ANDROID, native, nullptr);
    if (surface.image == EGL_NO_IMAGE_KHR) {
      std::fprintf(stderr, "OOS external eglCreateImageKHR failed: 0x%x\n",
                   eglGetError());
      destroyExternalSurface(surface, false);
      return nullptr;
    }
    while (glGetError() != GL_NO_ERROR) {
    }
    glGenTextures(1, &surface.texture);
    glBindTexture(GL_TEXTURE_2D, surface.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target_(GL_TEXTURE_2D,
                  reinterpret_cast<GLeglImageOES>(surface.image));
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
      std::fprintf(stderr, "OOS external EGLImage bind failed: 0x%x\n", error);
      destroyExternalSurface(surface, true);
      return nullptr;
    }
    surface.last_used = ++external_surface_epoch_;
    external_surfaces_.push_back(surface);
    return &external_surfaces_.back();
  }

  bool makeGlesContextCurrent() override {
    return egl_display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT &&
           pbuffer_ != EGL_NO_SURFACE &&
           eglMakeCurrent(egl_display_, pbuffer_, pbuffer_, context_) ==
               EGL_TRUE;
  }
  bool bindGlesSurface(bool require_depth, bool require_stencil) override {
    while (glGetError() != GL_NO_ERROR) {
    }
    if (!beginFrame())
      return false;
    bool created_depth = false;
    bool created_stencil = false;
    if (require_depth && !depth_buffer_) {
      glGenRenderbuffers(1, &depth_buffer_);
      glBindRenderbuffer(GL_RENDERBUFFER, depth_buffer_);
      glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                            PrimaryGlesDisplay::kWidth,
                            PrimaryGlesDisplay::kHeight);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, depth_buffer_);
      created_depth = true;
    }
    if (require_stencil && !stencil_buffer_) {
      glGenRenderbuffers(1, &stencil_buffer_);
      glBindRenderbuffer(GL_RENDERBUFFER, stencil_buffer_);
      glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8,
                            PrimaryGlesDisplay::kWidth,
                            PrimaryGlesDisplay::kHeight);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                GL_RENDERBUFFER, stencil_buffer_);
      created_stencil = true;
    }
    const bool success =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
        glGetError() == GL_NO_ERROR;
    if (!success && created_stencil) {
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                GL_RENDERBUFFER, 0);
      glDeleteRenderbuffers(1, &stencil_buffer_);
      stencil_buffer_ = 0;
    }
    if (!success && created_depth) {
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, 0);
      glDeleteRenderbuffers(1, &depth_buffer_);
      depth_buffer_ = 0;
    }
    return success;
  }
  bool presentGlesSurface() override {
    glFinish();
    return glGetError() == GL_NO_ERROR && presentAndReveal();
  }
  uint32_t glesSurfaceWidth() const override {
    return PrimaryGlesDisplay::kWidth;
  }
  uint32_t glesSurfaceHeight() const override {
    return PrimaryGlesDisplay::kHeight;
  }
  uint32_t glesDepthBits() const override { return 16; }
  uint32_t glesStencilBits() const override { return 8; }

  ::android::sp<::android::hardware::power::V1_0::IPower> power_;
  std::unique_ptr<HWC2::Device> device_;
  std::unique_ptr<DisplayCallback> callback_;
  HWC2::Display *display_ = nullptr;
  HWC2::Layer *layer_ = nullptr;
  std::array<TargetBuffer, kTargetBufferCount> targets_;
  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface pbuffer_ = EGL_NO_SURFACE;
  PFNEGLCREATEIMAGEKHRPROC create_image_ = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image_ = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_ = nullptr;
  GLuint framebuffer_ = 0;
  GLuint depth_buffer_ = 0;
  GLuint stencil_buffer_ = 0;
  GLuint program_ = 0;
  GLint screen_size_uniform_ = -1;
  GLint texture_uniform_ = -1;
  GLint texture_format_uniform_ = -1;
  runtime::GlesExecutor gles_;
  bool initialized_ = false;
  bool revealed_ = false;
  bool owns_display_state_ = false;
  size_t render_target_ = 0;
  size_t last_presented_target_ = kTargetBufferCount;
  uint64_t external_surface_epoch_ = 0;
  std::vector<ExternalSurface> external_surfaces_;
};

PrimaryGlesDisplay::PrimaryGlesDisplay() : impl_(std::make_unique<Impl>()) {}

PrimaryGlesDisplay::~PrimaryGlesDisplay() = default;

bool PrimaryGlesDisplay::initialize() { return impl_->initialize(); }

bool PrimaryGlesDisplay::showBootFrame(const uint16_t *rgb565_pixels) {
  return impl_->showBootFrame(rgb565_pixels);
}

bool PrimaryGlesDisplay::presentSurface(const compositor::SurfaceFrame &frame) {
  return impl_->presentSurface(frame);
}

bool PrimaryGlesDisplay::detachRenderContext() {
  return impl_->detachRenderContext();
}

bool PrimaryGlesDisplay::attachRenderContext() {
  return impl_->attachRenderContext();
}

void PrimaryGlesDisplay::refresh() { impl_->refresh(); }

void PrimaryGlesDisplay::shutdown() { impl_->shutdown(); }

uint32_t PrimaryGlesDisplay::width() const { return kWidth; }

uint32_t PrimaryGlesDisplay::height() const { return kHeight; }

uint32_t PrimaryGlesDisplay::surfaceFormat() const {
  return OOS_TEXTURE_RGB565;
}

uint32_t PrimaryGlesDisplay::supportedTextureFormats() const {
  return OOS_TEXTURE_FORMAT_MASK;
}

bool PrimaryGlesDisplay::setTexture(uint32_t texture, uint32_t format,
                                    uint32_t x, uint32_t y, uint32_t width,
                                    uint32_t height, uint32_t row_stride,
                                    uint32_t flags, const uint8_t *pixels,
                                    size_t pixel_bytes) {
  return impl_->setTexture(texture, format, x, y, width, height, row_stride,
                           flags, pixels, pixel_bytes);
}

bool PrimaryGlesDisplay::freeTexture(uint32_t texture) {
  return impl_->freeTexture(texture);
}

bool PrimaryGlesDisplay::submit(const OosGfxVertex *vertices,
                                size_t vertex_count, const uint16_t *indices,
                                size_t index_count,
                                const OosGfxDrawCommand *commands,
                                size_t command_count, uint32_t clear_rgba) {
  return impl_->submit(vertices, vertex_count, indices, index_count, commands,
                       command_count, clear_rgba);
}

bool PrimaryGlesDisplay::glesCapabilities(OosGlesCapabilities &result) {
  return impl_->gles_.capabilities(result);
}
bool PrimaryGlesDisplay::setGlesBuffer(uint32_t buffer, uint32_t size,
                                       uint32_t usage, const uint8_t *data,
                                       size_t data_size) {
  return impl_->gles_.setBuffer(buffer, size, usage, data, data_size);
}
bool PrimaryGlesDisplay::writeGlesBuffer(uint32_t buffer, uint32_t offset,
                                         const uint8_t *data,
                                         size_t data_size) {
  return impl_->gles_.writeBuffer(buffer, offset, data, data_size);
}
bool PrimaryGlesDisplay::freeGlesBuffer(uint32_t buffer) {
  return impl_->gles_.freeBuffer(buffer);
}
bool PrimaryGlesDisplay::setGlesShader(uint32_t shader, uint32_t stage,
                                       const char *source, size_t source_size) {
  return impl_->gles_.setShader(shader, stage, source, source_size);
}
bool PrimaryGlesDisplay::freeGlesShader(uint32_t shader) {
  return impl_->gles_.freeShader(shader);
}
bool PrimaryGlesDisplay::setGlesProgram(uint32_t program,
                                        uint32_t vertex_shader,
                                        uint32_t fragment_shader) {
  return impl_->gles_.setProgram(program, vertex_shader, fragment_shader);
}
bool PrimaryGlesDisplay::freeGlesProgram(uint32_t program) {
  return impl_->gles_.freeProgram(program);
}
int32_t PrimaryGlesDisplay::glesAttributeLocation(uint32_t program,
                                                  const char *name,
                                                  size_t name_size) {
  return impl_->gles_.attributeLocation(program, name, name_size);
}
int32_t PrimaryGlesDisplay::glesUniformLocation(uint32_t program,
                                                const char *name,
                                                size_t name_size) {
  return impl_->gles_.uniformLocation(program, name, name_size);
}
bool PrimaryGlesDisplay::submitGles(const OosGlesCommand *commands,
                                    size_t command_count, const uint32_t *data,
                                    size_t data_words) {
  return impl_->gles_.submit(commands, command_count, data, data_words);
}

} // namespace oos::nokia2780
