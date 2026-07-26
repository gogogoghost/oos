#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>

#include <android/hardware/power/1.0/IPower.h>
#include <binder/ProcessState.h>
#include <gui/BufferItem.h>
#include <gui/BufferQueue.h>
#include <gui/ConsumerBase.h>
#include <gui/Surface.h>
#include <hardware/gralloc.h>
#include <ui/Fence.h>
#include <ui/FloatRect.h>
#include <ui/GraphicBuffer.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/Mutex.h>

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

#include "HWC2.h"
#include "oos/nokia2780/display_control.h"

namespace {

class PrimaryDisplayCallback final : public HWC2::ComposerCallback {
public:
  explicit PrimaryDisplayCallback(HWC2::Device *device) : device_(device) {}

  void onHotplugReceived(int32_t, hwc2_display_t display,
                         HWC2::Connection connection) override {
    std::fprintf(stderr, "hotplug display=%" PRIu64 " connection=%d\n", display,
                 static_cast<int>(connection));
    device_->onHotplug(display, connection);
  }

  void onRefreshReceived(int32_t, hwc2_display_t) override {}
  void onVsyncReceived(int32_t, hwc2_display_t, int64_t) override {}

private:
  HWC2::Device *const device_;
};

class PrimaryConsumer final : public android::ConsumerBase {
public:
  PrimaryConsumer(const android::sp<android::IGraphicBufferConsumer> &consumer,
                  HWC2::Display *display)
      : ConsumerBase(consumer, false), display_(display) {
    mConsumer->setConsumerName(android::String8("primary-green-poc"));
    mConsumer->setConsumerUsageBits(GRALLOC_USAGE_HW_RENDER |
                                    GRALLOC_USAGE_HW_COMPOSER);
    mConsumer->setDefaultBufferFormat(android::PIXEL_FORMAT_RGB_565);
    mConsumer->setDefaultBufferSize(240, 320);
    mConsumer->setMaxAcquiredBufferCount(3);
  }

private:
  void onFrameAvailable(const android::BufferItem &) override {
    android::sp<android::GraphicBuffer> buffer;
    android::sp<android::Fence> acquire_fence;
    int slot = android::BufferQueue::INVALID_BUFFER_SLOT;

    {
      android::Mutex::Autolock lock(mMutex);
      android::BufferItem item;
      const android::status_t status = acquireBufferLocked(&item, 0);
      if (status != android::NO_ERROR) {
        ALOGE("acquireBufferLocked failed: %d", status);
        return;
      }
      slot = item.mSlot;
      buffer = mSlots[slot].mGraphicBuffer;
      acquire_fence = item.mFence;
    }

    if (!logged_buffer_) {
      std::fprintf(stderr,
                   "client buffer format=%d size=%ux%u stride=%u slot=%d\n",
                   buffer->getPixelFormat(), buffer->getWidth(),
                   buffer->getHeight(), buffer->getStride(), slot);
      logged_buffer_ = true;
    }

    uint32_t changes = 0;
    uint32_t requests = 0;
    auto error = display_->validate(&changes, &requests);
    if ((error != HWC2::Error::None && error != HWC2::Error::HasChanges) ||
        changes != 0 || requests != 0) {
      ALOGE("unexpected HWC validation result: error=%d changes=%u requests=%u",
            static_cast<int>(error), changes, requests);
      release(slot, buffer, android::Fence::NO_FENCE);
      return;
    }

    error = display_->acceptChanges();
    if (error != HWC2::Error::None) {
      ALOGE("acceptChanges failed: %d", static_cast<int>(error));
      release(slot, buffer, android::Fence::NO_FENCE);
      return;
    }

    error = display_->setClientTarget(static_cast<uint32_t>(slot), buffer,
                                      acquire_fence,
                                      android::ui::Dataspace::UNKNOWN);
    if (error != HWC2::Error::None) {
      ALOGE("setClientTarget failed: %d", static_cast<int>(error));
      release(slot, buffer, android::Fence::NO_FENCE);
      return;
    }

    android::sp<android::Fence> present_fence;
    error = display_->present(&present_fence);
    if (error != HWC2::Error::None) {
      ALOGE("present failed: %d", static_cast<int>(error));
      release(slot, buffer, android::Fence::NO_FENCE);
      return;
    }
    if (!submitted_frame_) {
      std::fprintf(stderr, "first HWC frame submitted\n");
      submitted_frame_ = true;
    }

    int previous_slot = android::BufferQueue::INVALID_BUFFER_SLOT;
    android::sp<android::GraphicBuffer> previous_buffer;
    {
      android::Mutex::Autolock lock(mMutex);
      previous_slot = current_slot_;
      previous_buffer = current_buffer_;
      current_slot_ = slot;
      current_buffer_ = buffer;
    }
    if (previous_slot != android::BufferQueue::INVALID_BUFFER_SLOT &&
        previous_slot != slot) {
      release(previous_slot, previous_buffer, present_fence);
    }
  }

  void release(int slot, const android::sp<android::GraphicBuffer> &buffer,
               const android::sp<android::Fence> &fence) {
    android::Mutex::Autolock lock(mMutex);
    if (fence != nullptr && fence->isValid()) {
      addReleaseFenceLocked(slot, buffer, fence);
    }
    const android::status_t status = releaseBufferLocked(slot, buffer);
    if (status != android::NO_ERROR) {
      ALOGE("releaseBufferLocked failed: %d", status);
    }
  }

  HWC2::Display *const display_;
  int current_slot_ = android::BufferQueue::INVALID_BUFFER_SLOT;
  android::sp<android::GraphicBuffer> current_buffer_;
  bool submitted_frame_ = false;
  bool logged_buffer_ = false;
};

bool set_primary_backlight(int value) {
  constexpr char kBacklightPath[] = "/sys/class/leds/lcd-backlight/brightness";
  char text[16];
  const int length = std::snprintf(text, sizeof(text), "%d\n", value);
  const int fd = open(kBacklightPath, O_WRONLY | O_CLOEXEC);
  if (fd < 0 || length <= 0 || length >= static_cast<int>(sizeof(text)) ||
      write(fd, text, static_cast<size_t>(length)) != length) {
    std::fprintf(stderr, "backlight write %d failed\n", value);
    if (fd >= 0)
      close(fd);
    return false;
  }
  close(fd);
  std::fprintf(stderr, "primary backlight=%d\n", value);
  return true;
}

bool check_egl(EGLBoolean result, const char *operation) {
  if (result == EGL_TRUE)
    return true;
  std::fprintf(stderr, "%s failed: 0x%x\n", operation, eglGetError());
  return false;
}

} // namespace

int main() {
  android::ProcessState::self()->startThreadPool();

  auto power = ::android::hardware::power::V1_0::IPower::getService();
  if (power == nullptr) {
    std::fprintf(stderr, "IPower service is unavailable\n");
    return 1;
  }
  if (!power->setInteractive(true).isOk()) {
    std::fprintf(stderr, "IPower setInteractive(true) failed\n");
    return 1;
  }
  std::fprintf(stderr, "IPower interactive enabled\n");
  if (nokia2780_prepare_primary() != 0) {
    std::fprintf(stderr, "failed to switch from cover to primary display\n");
    return 1;
  }

  HWC2::Device device("default");
  PrimaryDisplayCallback callback(&device);
  device.registerCallback(&callback, 0);
  HWC2::Display *display = device.getDisplayById(device.getDefaultDisplayId());
  if (display == nullptr || !display->isConnected()) {
    std::fprintf(stderr, "primary HWC display is unavailable\n");
    return 1;
  }

  std::shared_ptr<const HWC2::Display::Config> hwc_config;
  if (display->getActiveConfig(&hwc_config) != HWC2::Error::None ||
      hwc_config == nullptr) {
    std::fprintf(stderr, "failed to query primary display config\n");
    return 1;
  }
  std::fprintf(stderr, "HWC primary id=%" PRIu64 " size=%dx%d\n",
               display->getId(), hwc_config->getWidth(),
               hwc_config->getHeight());

  if (display->setPowerMode(HWC2::PowerMode::On) != HWC2::Error::None) {
    std::fprintf(stderr, "failed to enable primary display\n");
    return 1;
  }

  android::sp<android::IGraphicBufferProducer> producer;
  android::sp<android::IGraphicBufferConsumer> consumer;
  android::BufferQueue::createBufferQueue(&producer, &consumer);
  android::sp<PrimaryConsumer> primary_consumer =
      new PrimaryConsumer(consumer, display);
  android::sp<android::Surface> surface = new android::Surface(producer, true);
  if (ANativeWindow_setBuffersGeometry(surface.get(), 240, 320,
                                       android::PIXEL_FORMAT_RGB_565) != 0) {
    std::fprintf(stderr, "failed to force RGB565 BufferQueue geometry\n");
    return 1;
  }

  EGLDisplay egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (egl_display == EGL_NO_DISPLAY ||
      !check_egl(eglInitialize(egl_display, nullptr, nullptr),
                 "eglInitialize")) {
    return 1;
  }
  const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE,
      EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE,
      EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE,
      5,
      EGL_GREEN_SIZE,
      6,
      EGL_BLUE_SIZE,
      5,
      EGL_ALPHA_SIZE,
      0,
      EGL_DEPTH_SIZE,
      24,
      EGL_NONE,
  };
  EGLConfig config = nullptr;
  EGLint config_count = 0;
  if (!check_egl(eglChooseConfig(egl_display, config_attributes, &config, 1,
                                 &config_count),
                 "eglChooseConfig") ||
      config_count == 0) {
    return 1;
  }
  EGLint native_visual_id = 0;
  if (!check_egl(eglGetConfigAttrib(egl_display, config, EGL_NATIVE_VISUAL_ID,
                                    &native_visual_id),
                 "eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID)")) {
    return 1;
  }
  std::fprintf(stderr, "EGL native visual format=%d\n", native_visual_id);
  const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context =
      eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attributes);
  EGLSurface egl_surface = eglCreateWindowSurface(
      egl_display, config, static_cast<EGLNativeWindowType>(surface.get()),
      nullptr);
  if (context == EGL_NO_CONTEXT || egl_surface == EGL_NO_SURFACE ||
      !check_egl(eglMakeCurrent(egl_display, egl_surface, egl_surface, context),
                 "eglMakeCurrent")) {
    return 1;
  }

  glViewport(0, 0, 240, 320);
  glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  if (!check_egl(eglSwapBuffers(egl_display, egl_surface),
                 "initial eglSwapBuffers") ||
      !set_primary_backlight(255)) {
    return 1;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    glViewport(0, 0, 240, 320);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!check_egl(eglSwapBuffers(egl_display, egl_surface),
                   "eglSwapBuffers")) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }

  if (!set_primary_backlight(0))
    std::fprintf(stderr, "failed to disable primary backlight\n");
  if (display->setPowerMode(HWC2::PowerMode::Off) != HWC2::Error::None)
    std::fprintf(stderr, "failed to power down primary display\n");
  power->setInteractive(false);
  primary_consumer->abandon();
  eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(egl_display, egl_surface);
  eglDestroyContext(egl_display, context);
  eglTerminate(egl_display);
  return 0;
}
