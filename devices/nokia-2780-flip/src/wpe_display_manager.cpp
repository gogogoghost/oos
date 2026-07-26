#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <android/hardware/power/1.0/IPower.h>
#include <android/hardware_buffer.h>
#include <binder/ProcessState.h>
#include <hardware/gralloc.h>
#include <private/android/AHardwareBufferHelpers.h>
#include <ui/Fence.h>
#include <ui/FloatRect.h>
#include <ui/GraphicBuffer.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <utils/Errors.h>

#include <wpe-android/view-backend.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "HWC2.h"
#include "oos/nokia2780/display_control.h"
#include "oos/nokia2780/wpe_display_manager.h"

namespace oos::nokia2780 {

constexpr useconds_t kPanelTransferSettleUs = 150000;

int64_t monotonicMicros() {
  timespec time = {};
  clock_gettime(CLOCK_MONOTONIC, &time);
  return static_cast<int64_t>(time.tv_sec) * 1000000 + time.tv_nsec / 1000;
}

class DisplayCallback final : public HWC2::ComposerCallback {
public:
  explicit DisplayCallback(HWC2::Device *device) : device_(device) {}
  void onHotplugReceived(int32_t, hwc2_display_t display,
                         HWC2::Connection connection) override {
    device_->onHotplug(display, connection);
    auto *current = device_->getDisplayById(display);
    std::shared_ptr<const HWC2::Display::Config> config;
    std::string name;
    if (current && current->isConnected()) {
      if (current->getActiveConfig(&config) != HWC2::Error::None)
        config.reset();
      if (current->getName(&name) != HWC2::Error::None)
        name.clear();
    }
    std::fprintf(
        stderr, "HWC hotplug id=%llu connection=%d name=%s size=%dx%d\n",
        static_cast<unsigned long long>(display), static_cast<int>(connection),
        name.c_str(), config ? config->getWidth() : 0,
        config ? config->getHeight() : 0);
    std::fflush(stderr);
  }
  void onRefreshReceived(int32_t, hwc2_display_t) override {}
  void onVsyncReceived(int32_t, hwc2_display_t, int64_t) override {}

private:
  HWC2::Device *const device_;
};

class WpeDisplayManager::Impl {
public:
  explicit Impl(bool auto_reveal) : auto_reveal_(auto_reveal) {}

  ~Impl() {
    shutdownDisplays();
    releaseGpu();
  }

  bool initialize() {
    power_ = ::android::hardware::power::V1_0::IPower::getService();
    if (power_ == nullptr || !power_->setInteractive(true).isOk())
      return false;
    if (!setBacklight(0))
      return false;
    owns_display_state_ = true;
    if (nokia2780_prepare_primary() != 0)
      return false;

    device_ = std::make_unique<HWC2::Device>("default");
    callback_ = std::make_unique<DisplayCallback>(device_.get());
    device_->registerCallback(callback_.get(), 0);
    // The cover becomes the most recent hotplug display once fb1 is enabled.
    // The main panel is always HWC_DISPLAY_PRIMARY on this device.
    display_ = device_->getDisplayById(HWC_DISPLAY_PRIMARY);
    if (display_ == nullptr || !display_->isConnected() ||
        display_->setPowerMode(HWC2::PowerMode::On) != HWC2::Error::None) {
      return false;
    }
    std::shared_ptr<const HWC2::Display::Config> config;
    if (display_->getActiveConfig(&config) != HWC2::Error::None)
      config.reset();
    std::fprintf(stderr, "HWC selected primary id=%llu size=%dx%d\n",
                 static_cast<unsigned long long>(HWC_DISPLAY_PRIMARY),
                 config ? config->getWidth() : 0,
                 config ? config->getHeight() : 0);
    std::fflush(stderr);

    const android::Rect frame(0, 0, WpeDisplayManager::kPrimaryWidth,
                              WpeDisplayManager::kPrimaryHeight);
    if (display_->createLayer(&layer_) != HWC2::Error::None ||
        layer_ == nullptr ||
        layer_->setCompositionType(HWC2::Composition::Client) !=
            HWC2::Error::None ||
        layer_->setBlendMode(HWC2::BlendMode::None) != HWC2::Error::None ||
        layer_->setSourceCrop(android::FloatRect(
            0, 0, WpeDisplayManager::kPrimaryWidth,
            WpeDisplayManager::kPrimaryHeight)) != HWC2::Error::None ||
        layer_->setDisplayFrame(frame) != HWC2::Error::None ||
        layer_->setVisibleRegion(android::Region(frame)) != HWC2::Error::None ||
        layer_->setPlaneAlpha(1.f) != HWC2::Error::None ||
        layer_->setZOrder(0) != HWC2::Error::None) {
      return false;
    }
    client_target_ = new android::GraphicBuffer(
        WpeDisplayManager::kPrimaryWidth, WpeDisplayManager::kPrimaryHeight,
        android::PIXEL_FORMAT_RGB_565, 1,
        static_cast<uint64_t>(GRALLOC_USAGE_HW_RENDER |
                              GRALLOC_USAGE_HW_COMPOSER),
        "wpe-primary-client-target");
    if (client_target_->initCheck() != android::NO_ERROR ||
        client_target_->handle == nullptr || !initializeGpu()) {
      return false;
    }
    return clearAndSubmitClientTarget();
  }

  bool showBootFrame(const uint16_t *pixels) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (!pixels || !owns_display_state_ || !primary_active_ ||
        !blitRgb565ToClientTarget(pixels)) {
      return false;
    }
    last_target_ = client_target_;
    last_slot_ = 0;
    if (!submitClientTarget(0, client_target_, android::Fence::NO_FENCE))
      return false;
    usleep(kPanelTransferSettleUs);
    if (!submitClientTarget(0, client_target_, android::Fence::NO_FENCE))
      return false;
    usleep(kPanelTransferSettleUs);
    if (!setBacklight(255))
      return false;
    std::fprintf(stderr, "primary boot frame presented\n");
    std::fflush(stderr);
    return true;
  }

  void present(WPEAndroidViewBackend *backend, WPEAndroidBuffer *buffer,
               int acquire_fence_fd) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    auto *raw = WPEAndroidBuffer_getAHardwareBuffer(buffer);
    if (!blitToClientTarget(raw, acquire_fence_fd)) {
      WPEAndroidViewBackend_dispatchReleaseBuffer(backend, buffer);
      return;
    }
    last_target_ = client_target_;
    last_slot_ = 0;
    if (primary_active_ &&
        !submitClientTarget(0, client_target_, android::Fence::NO_FENCE)) {
      WPEAndroidViewBackend_dispatchReleaseBuffer(backend, buffer);
      return;
    }
    if (primary_active_ && !has_presented_frame_) {
      // HWC returns before this command-mode panel has necessarily completed
      // its physical transfer. Keep the backlight off through two settled
      // submissions so old panel GRAM can never become visible.
      usleep(kPanelTransferSettleUs);
      if (!submitClientTarget(0, client_target_, android::Fence::NO_FENCE)) {
        WPEAndroidViewBackend_dispatchReleaseBuffer(backend, buffer);
        return;
      }
      usleep(kPanelTransferSettleUs);
      if (auto_reveal_) {
        if (!setBacklight(255)) {
          std::fprintf(stderr, "failed to enable primary backlight\n");
          WPEAndroidViewBackend_dispatchReleaseBuffer(backend, buffer);
          return;
        }
        std::fprintf(stderr, "primary frame presented\n");
      } else {
        std::fprintf(stderr, "primary frame ready for switch demo\n");
      }
      std::fflush(stderr);
      has_presented_frame_ = true;
    }
    WPEAndroidViewBackend_dispatchReleaseBuffer(backend, buffer);
    WPEAndroidViewBackend_dispatchFrameComplete(backend);
  }

  void refresh() {
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (primary_active_ && last_target_ != nullptr)
      submitClientTarget(last_slot_, last_target_, android::Fence::NO_FENCE);
  }

  bool frameReady() {
    std::lock_guard<std::mutex> lock(present_mutex_);
    return has_presented_frame_;
  }

  bool showCover(const uint16_t *pixels) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    const int64_t started = monotonicMicros();
    if (!has_presented_frame_ || !setBacklight(0))
      return false;
    const int64_t backlight_off = monotonicMicros();
    primary_active_ = false;
    if (display_->setPowerMode(HWC2::PowerMode::Off) != HWC2::Error::None)
      return false;
    const int64_t primary_off = monotonicMicros();
    const int64_t guard_complete = monotonicMicros();
    if (nokia2780_show_cover_rgb565_after_primary_off(
            pixels, NOKIA_2780_COVER_WIDTH, NOKIA_2780_COVER_HEIGHT) != 0) {
      return false;
    }
    cover_active_ = true;
    const int64_t elapsed = monotonicMicros() - started;
    std::fprintf(stderr,
                 "single-process switch primary->cover completed in %.1f ms "
                 "(backlight %.1f, HWC off %.1f, cover %.1f)\n",
                 elapsed / 1000.0, (backlight_off - started) / 1000.0,
                 (primary_off - backlight_off) / 1000.0,
                 (monotonicMicros() - guard_complete) / 1000.0);
    std::fflush(stderr);
    return true;
  }

  bool showPrimary() {
    std::lock_guard<std::mutex> lock(present_mutex_);
    const int64_t started = monotonicMicros();
    if (!cover_active_ || last_target_ == nullptr ||
        nokia2780_hide_cover() != 0) {
      return false;
    }
    const int64_t cover_off = monotonicMicros();
    cover_active_ = false;
    if (display_->setPowerMode(HWC2::PowerMode::On) != HWC2::Error::None)
      return false;
    const int64_t primary_on = monotonicMicros();
    if (!submitClientTarget(last_slot_, last_target_, android::Fence::NO_FENCE))
      return false;
    const int64_t first_submit = monotonicMicros();
    usleep(kPanelTransferSettleUs);
    const int64_t first_guard = monotonicMicros();
    if (!setBacklight(255))
      return false;
    primary_active_ = true;
    const int64_t elapsed = monotonicMicros() - started;
    std::fprintf(stderr,
                 "single-process switch cover->primary completed in %.1f ms "
                 "(cover off %.1f, HWC on %.1f, submit %.1f, guard %.1f, "
                 "backlight %.1f)\n",
                 elapsed / 1000.0, (cover_off - started) / 1000.0,
                 (primary_on - cover_off) / 1000.0,
                 (first_submit - primary_on) / 1000.0,
                 (first_guard - first_submit) / 1000.0,
                 (monotonicMicros() - first_guard) / 1000.0);
    std::fflush(stderr);
    return true;
  }

  void shutdownDisplays() {
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (!owns_display_state_)
      return;
    setBacklight(0);
    if (cover_active_) {
      nokia2780_hide_cover();
      cover_active_ = false;
    }
    if (display_ &&
        display_->setPowerMode(HWC2::PowerMode::Off) != HWC2::Error::None) {
      std::fprintf(stderr, "failed to power down primary display\n");
    }
    primary_active_ = false;
    owns_display_state_ = false;
  }

private:
  bool clearAndSubmitClientTarget() {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, WpeDisplayManager::kPrimaryWidth,
               WpeDisplayManager::kPrimaryHeight);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    if (glGetError() != GL_NO_ERROR) {
      std::fprintf(stderr, "failed to clear primary client target\n");
      return false;
    }
    if (!submitClientTarget(0, client_target_, android::Fence::NO_FENCE))
      return false;
    last_target_ = client_target_;
    last_slot_ = 0;
    usleep(kPanelTransferSettleUs);
    std::fprintf(stderr, "primary black preroll presented\n");
    std::fflush(stderr);
    return true;
  }

  bool submitClientTarget(uint32_t slot,
                          const android::sp<android::GraphicBuffer> &target,
                          const android::sp<android::Fence> &acquire) {
    uint32_t changes = 0;
    uint32_t requests = 0;
    const auto validate = display_->validate(&changes, &requests);
    if (validate == HWC2::Error::HasChanges &&
        display_->acceptChanges() != HWC2::Error::None) {
      std::fprintf(stderr, "HWC acceptChanges failed: changes=%u requests=%u\n",
                   changes, requests);
      return false;
    }
    if (validate != HWC2::Error::None && validate != HWC2::Error::HasChanges) {
      std::fprintf(stderr, "HWC validate failed: %d changes=%u requests=%u\n",
                   static_cast<int>(validate), changes, requests);
      return false;
    }
    if (display_->setClientTarget(slot, target, acquire,
                                  android::ui::Dataspace::UNKNOWN) !=
        HWC2::Error::None) {
      std::fprintf(stderr, "HWC setClientTarget failed\n");
      return false;
    }
    android::sp<android::Fence> present_fence;
    if (display_->present(&present_fence) != HWC2::Error::None) {
      std::fprintf(stderr, "HWC present failed\n");
      return false;
    }
    if (present_fence != nullptr && present_fence->isValid())
      present_fence->waitForever("wpe-hello-present");
    return true;
  }

  uint32_t bufferSlot(AHardwareBuffer *raw,
                      const android::GraphicBuffer *buffer) {
    const auto existing = slots_.find(raw);
    if (existing != slots_.end())
      return existing->second;
    const uint32_t slot = static_cast<uint32_t>(slots_.size());
    slots_.emplace(raw, slot);
    std::fprintf(
        stderr,
        "WPE direct RGB565 buffer=%p slot=%u format=%d size=%ux%u stride=%u\n",
        raw, slot, buffer->getPixelFormat(), buffer->getWidth(),
        buffer->getHeight(), buffer->getStride());
    std::fflush(stderr);
    return slot;
  }

  static bool checkEgl(EGLBoolean result, const char *operation) {
    if (result == EGL_TRUE)
      return true;
    std::fprintf(stderr, "%s failed: 0x%x\n", operation, eglGetError());
    return false;
  }

  static GLuint compileShader(GLenum type, const char *source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
      return shader;
    char log[256] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::fprintf(stderr, "shader compilation failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }

  bool initializeGpu() {
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
    egl_context_ = eglCreateContext(egl_display_, config, EGL_NO_CONTEXT,
                                    context_attributes);
    const EGLint pbuffer_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    pbuffer_ =
        eglCreatePbufferSurface(egl_display_, config, pbuffer_attributes);
    if (egl_context_ == EGL_NO_CONTEXT || pbuffer_ == EGL_NO_SURFACE ||
        !checkEgl(
            eglMakeCurrent(egl_display_, pbuffer_, pbuffer_, egl_context_),
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

    target_image_ = create_image_(
        egl_display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
        reinterpret_cast<EGLClientBuffer>(client_target_->getNativeBuffer()),
        nullptr);
    if (target_image_ == EGL_NO_IMAGE_KHR)
      return false;

    glGenTextures(1, &source_texture_);
    glGenTextures(1, &target_texture_);
    glBindTexture(GL_TEXTURE_2D, target_texture_);
    image_target_(GL_TEXTURE_2D,
                  reinterpret_cast<GLeglImageOES>(target_image_));
    glGenFramebuffers(1, &framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           target_texture_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      return false;

    constexpr char vertex_shader[] =
        "attribute vec2 position;\n"
        "attribute vec2 texcoord;\n"
        "varying vec2 vTexcoord;\n"
        "void main() { gl_Position = vec4(position, 0.0, 1.0); vTexcoord = "
        "texcoord; }\n";
    constexpr char fragment_shader[] =
        "precision mediump float;\n"
        "varying vec2 vTexcoord;\n"
        "uniform sampler2D source;\n"
        "void main() { gl_FragColor = texture2D(source, vTexcoord); }\n";
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertex_shader);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragment_shader);
    if (!vertex || !fragment)
      return false;
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glBindAttribLocation(program_, 0, "position");
    glBindAttribLocation(program_, 1, "texcoord");
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
      return false;
    source_uniform_ = glGetUniformLocation(program_, "source");
    return source_uniform_ >= 0;
  }

  bool blitToClientTarget(AHardwareBuffer *source, int acquire_fence_fd) {
    if (acquire_fence_fd >= 0) {
      android::sp<android::Fence> acquire =
          new android::Fence(acquire_fence_fd);
      acquire->waitForever("wpe-source-acquire");
    }
    android::sp<android::GraphicBuffer> source_graphic =
        android::GraphicBuffer::fromAHardwareBuffer(source);
    if (source_graphic == nullptr) {
      std::fprintf(stderr, "WPE AHardwareBuffer has no GraphicBuffer bridge\n");
      return false;
    }
    if (!logged_source_buffer_) {
      std::fprintf(stderr, "WPE source format=%d size=%ux%u stride=%u\n",
                   source_graphic->getPixelFormat(), source_graphic->getWidth(),
                   source_graphic->getHeight(), source_graphic->getStride());
      std::fflush(stderr);
      logged_source_buffer_ = true;
    }
    const EGLImageKHR source_image = create_image_(
        egl_display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
        reinterpret_cast<EGLClientBuffer>(source_graphic->getNativeBuffer()),
        nullptr);
    if (source_image == EGL_NO_IMAGE_KHR) {
      std::fprintf(stderr, "eglCreateImageKHR(WPE source) failed: 0x%x\n",
                   eglGetError());
      return false;
    }
    glBindTexture(GL_TEXTURE_2D, source_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target_(GL_TEXTURE_2D, reinterpret_cast<GLeglImageOES>(source_image));
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, WpeDisplayManager::kPrimaryWidth,
               WpeDisplayManager::kPrimaryHeight);
    glUseProgram(program_);
    constexpr GLfloat positions[] = {-1.f, -1.f, 1.f, -1.f,
                                     -1.f, 1.f,  1.f, 1.f};
    constexpr GLfloat texcoords[] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f};
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source_texture_);
    glUniform1i(source_uniform_, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, positions);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glFinish();
    destroy_image_(egl_display_, source_image);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR)
      std::fprintf(stderr, "WPE GPU blit GL error: 0x%x\n", error);
    return error == GL_NO_ERROR;
  }

  bool blitRgb565ToClientTarget(const uint16_t *pixels) {
    glBindTexture(GL_TEXTURE_2D, source_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, WpeDisplayManager::kPrimaryWidth,
                 WpeDisplayManager::kPrimaryHeight, 0, GL_RGB,
                 GL_UNSIGNED_SHORT_5_6_5, pixels);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, WpeDisplayManager::kPrimaryWidth,
               WpeDisplayManager::kPrimaryHeight);
    glUseProgram(program_);
    constexpr GLfloat positions[] = {-1.f, -1.f, 1.f, -1.f,
                                     -1.f, 1.f,  1.f, 1.f};
    // Match the orientation already established by the WPE source buffer path.
    constexpr GLfloat texcoords[] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f};
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source_texture_);
    glUniform1i(source_uniform_, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, positions);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glFinish();
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR)
      std::fprintf(stderr, "boot frame GPU upload GL error: 0x%x\n", error);
    return error == GL_NO_ERROR;
  }

  bool setBacklight(int value) {
    FILE *file = std::fopen("/sys/class/leds/lcd-backlight/brightness", "w");
    if (!file)
      return false;
    const bool ok = std::fprintf(file, "%d\n", value) > 0;
    std::fclose(file);
    return ok;
  }

  void releaseGpu() {
    const bool context_current =
        egl_display_ != EGL_NO_DISPLAY && egl_context_ != EGL_NO_CONTEXT &&
        pbuffer_ != EGL_NO_SURFACE &&
        eglMakeCurrent(egl_display_, pbuffer_, pbuffer_, egl_context_) ==
            EGL_TRUE;
    if (context_current) {
      if (program_)
        glDeleteProgram(program_);
      if (framebuffer_)
        glDeleteFramebuffers(1, &framebuffer_);
      if (source_texture_)
        glDeleteTextures(1, &source_texture_);
      if (target_texture_)
        glDeleteTextures(1, &target_texture_);
    }
    if (target_image_ != EGL_NO_IMAGE_KHR && destroy_image_)
      destroy_image_(egl_display_, target_image_);
    if (egl_display_ != EGL_NO_DISPLAY) {
      eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                     EGL_NO_CONTEXT);
      if (pbuffer_ != EGL_NO_SURFACE)
        eglDestroySurface(egl_display_, pbuffer_);
      if (egl_context_ != EGL_NO_CONTEXT)
        eglDestroyContext(egl_display_, egl_context_);
      eglTerminate(egl_display_);
    }
  }

  ::android::sp<::android::hardware::power::V1_0::IPower> power_;
  std::unique_ptr<HWC2::Device> device_;
  std::unique_ptr<DisplayCallback> callback_;
  HWC2::Display *display_ = nullptr;
  HWC2::Layer *layer_ = nullptr;
  android::sp<android::GraphicBuffer> last_target_;
  uint32_t last_slot_ = 0;
  android::sp<android::GraphicBuffer> client_target_;
  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLContext egl_context_ = EGL_NO_CONTEXT;
  EGLSurface pbuffer_ = EGL_NO_SURFACE;
  EGLImageKHR target_image_ = EGL_NO_IMAGE_KHR;
  PFNEGLCREATEIMAGEKHRPROC create_image_ = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image_ = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_ = nullptr;
  GLuint source_texture_ = 0;
  GLuint target_texture_ = 0;
  GLuint framebuffer_ = 0;
  GLuint program_ = 0;
  GLint source_uniform_ = -1;
  bool logged_source_buffer_ = false;
  std::mutex present_mutex_;
  const bool auto_reveal_;
  bool primary_active_ = true;
  bool cover_active_ = false;
  bool has_presented_frame_ = false;
  bool owns_display_state_ = false;
  std::unordered_map<AHardwareBuffer *, uint32_t> slots_;
};

WpeDisplayManager::WpeDisplayManager(bool reveal_first_frame)
    : impl_(std::make_unique<Impl>(reveal_first_frame)) {}

WpeDisplayManager::~WpeDisplayManager() = default;

bool WpeDisplayManager::initialize() { return impl_->initialize(); }

bool WpeDisplayManager::showBootFrame(const uint16_t *rgb565_pixels) {
  return impl_->showBootFrame(rgb565_pixels);
}

void WpeDisplayManager::present(WPEAndroidViewBackend *backend,
                                WPEAndroidBuffer *buffer,
                                int acquire_fence_fd) {
  impl_->present(backend, buffer, acquire_fence_fd);
}

void WpeDisplayManager::refresh() { impl_->refresh(); }

bool WpeDisplayManager::frameReady() { return impl_->frameReady(); }

bool WpeDisplayManager::showCover(const uint16_t *rgb565_pixels) {
  return impl_->showCover(rgb565_pixels);
}

bool WpeDisplayManager::showPrimary() { return impl_->showPrimary(); }

void WpeDisplayManager::shutdownDisplays() { impl_->shutdownDisplays(); }

} // namespace oos::nokia2780
