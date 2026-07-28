#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include <hardware/power.h>
#include <sync/sync.h>
#include <system/window.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unistd.h>

namespace {

constexpr int kWidth = 240;
constexpr int kHeight = 320;
constexpr char kBacklightPath[] = "/sys/class/leds/lcd-backlight/brightness";

void nativeBufferIncRef(android_native_base_t *) {}
void nativeBufferDecRef(android_native_base_t *) {}

struct NativeBuffer : ANativeWindowBuffer {
  NativeBuffer(buffer_handle_t buffer_handle, int buffer_stride) {
    common.incRef = nativeBufferIncRef;
    common.decRef = nativeBufferDecRef;
    width = kWidth;
    height = kHeight;
    stride = buffer_stride;
    format = HAL_PIXEL_FORMAT_RGB_565;
    usage = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER;
    std::memset(reserved, 0, sizeof(reserved));
    handle = buffer_handle;
    std::memset(reserved_proc, 0, sizeof(reserved_proc));
  }
};

struct DisplayContents {
  hwc_display_contents_1_t contents;
  hwc_layer_1_t layers[2];
};

void onInvalidate(const hwc_procs_t *) {}
void onVsync(const hwc_procs_t *, int, int64_t) {}
void onHotplug(const hwc_procs_t *, int, int) {}

const hwc_procs_t kHwcProcs = {
    .invalidate = onInvalidate,
    .vsync = onVsync,
    .hotplug = onHotplug,
};

bool setBacklight(int value) {
  FILE *file = std::fopen(kBacklightPath, "w");
  if (!file) {
    std::fprintf(stderr, "open %s failed: %s\n", kBacklightPath,
                 std::strerror(errno));
    return false;
  }
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

void waitAndCloseFence(int &fence) {
  if (fence < 0)
    return;
  if (sync_wait(fence, 3000) != 0)
    std::fprintf(stderr, "sync_wait(%d) failed: %s\n", fence,
                 std::strerror(errno));
  close(fence);
  fence = -1;
}

bool present(hwc_composer_device_1_t *hwc, buffer_handle_t handle,
             bool geometry_changed) {
  DisplayContents frame{};
  frame.contents.retireFenceFd = -1;
  frame.contents.outbuf = nullptr;
  frame.contents.outbufAcquireFenceFd = -1;
  frame.contents.flags = geometry_changed ? HWC_GEOMETRY_CHANGED : 0;
  frame.contents.numHwLayers = 2;

  const hwc_rect_t visible_rect = {0, 0, kWidth, kHeight};
  hwc_layer_1_t &source_layer = frame.layers[0];
  source_layer.compositionType = HWC_FRAMEBUFFER;
  source_layer.flags = HWC_SKIP_LAYER;
  source_layer.handle = handle;
  source_layer.transform = 0;
  source_layer.blending = HWC_BLENDING_NONE;
  source_layer.sourceCropf = {0.0f, 0.0f, static_cast<float>(kWidth),
                              static_cast<float>(kHeight)};
  source_layer.displayFrame = {0, 0, kWidth, kHeight};
  source_layer.visibleRegionScreen = {1, &visible_rect};
  source_layer.acquireFenceFd = -1;
  source_layer.releaseFenceFd = -1;
  source_layer.planeAlpha = 255;
  source_layer.surfaceDamage = {1, &visible_rect};

  hwc_layer_1_t &target_layer = frame.layers[1];
  target_layer.compositionType = HWC_FRAMEBUFFER_TARGET;
  target_layer.handle = handle;
  target_layer.transform = 0;
  target_layer.blending = HWC_BLENDING_PREMULT;
  target_layer.sourceCropf = {0.0f, 0.0f, static_cast<float>(kWidth),
                              static_cast<float>(kHeight)};
  target_layer.displayFrame = {0, 0, kWidth, kHeight};
  target_layer.visibleRegionScreen = {1, &visible_rect};
  target_layer.acquireFenceFd = -1;
  target_layer.releaseFenceFd = -1;
  target_layer.planeAlpha = 255;
  target_layer.surfaceDamage = {1, &visible_rect};

  std::array<hwc_display_contents_1_t *, 1> displays = {&frame.contents};
  int result = hwc->prepare(hwc, displays.size(), displays.data());
  if (result != 0) {
    std::fprintf(stderr, "HWC prepare failed: %d (%s)\n", result,
                 std::strerror(-result));
    return false;
  }
  std::fprintf(stderr,
               "HWC prepared source type=%d target type=%d hints=0x%x\n",
               source_layer.compositionType, target_layer.compositionType,
               source_layer.hints);
  result = hwc->set(hwc, displays.size(), displays.data());
  if (result != 0) {
    std::fprintf(stderr, "HWC set failed: %d (%s)\n", result,
                 std::strerror(-result));
    return false;
  }
  if (source_layer.releaseFenceFd >= 0) {
    close(source_layer.releaseFenceFd);
    source_layer.releaseFenceFd = -1;
  }
  waitAndCloseFence(target_layer.releaseFenceFd);
  waitAndCloseFence(frame.contents.retireFenceFd);
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const unsigned seconds =
      argc == 2 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10))
                : 10;
  if (!setBacklight(0))
    return 1;

  const hw_module_t *power_hardware = nullptr;
  power_module_t *power = nullptr;
  if (hw_get_module(POWER_HARDWARE_MODULE_ID, &power_hardware) != 0 ||
      !power_hardware) {
    std::fprintf(stderr, "failed to load power HAL\n");
    return 1;
  }
  power = reinterpret_cast<power_module_t *>(
      const_cast<hw_module_t *>(power_hardware));
  if (power->init)
    power->init(power);
  if (power->setInteractive)
    power->setInteractive(power, 1);

  const hw_module_t *gralloc_module = nullptr;
  if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &gralloc_module) != 0 ||
      !gralloc_module) {
    std::fprintf(stderr, "failed to load gralloc module\n");
    return 1;
  }
  alloc_device_t *allocator = nullptr;
  if (gralloc_open(gralloc_module, &allocator) != 0 || !allocator) {
    std::fprintf(stderr, "failed to open gralloc allocator\n");
    return 1;
  }
  buffer_handle_t handle = nullptr;
  int stride = 0;
  int result = allocator->alloc(
      allocator, kWidth, kHeight, HAL_PIXEL_FORMAT_RGB_565,
      GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER, &handle, &stride);
  if (result != 0 || !handle) {
    std::fprintf(stderr, "gralloc allocation failed: %d\n", result);
    gralloc_close(allocator);
    return 1;
  }
  NativeBuffer native_buffer(handle, stride);

  const hw_module_t *hwc_module = nullptr;
  hwc_composer_device_1_t *hwc = nullptr;
  if (hw_get_module(HWC_HARDWARE_MODULE_ID, &hwc_module) != 0 || !hwc_module ||
      hwc_open_1(hwc_module, &hwc) != 0 || !hwc) {
    std::fprintf(stderr, "failed to open HWC1 device\n");
    allocator->free(allocator, handle);
    gralloc_close(allocator);
    return 1;
  }
  if (hwc->registerProcs)
    hwc->registerProcs(hwc, &kHwcProcs);
  if (!hwc->setPowerMode) {
    std::fprintf(stderr, "HWC setPowerMode is unavailable\n");
    hwc_close_1(hwc);
    allocator->free(allocator, handle);
    gralloc_close(allocator);
    return 1;
  }
  hwc->setPowerMode(hwc, HWC_DISPLAY_PRIMARY, HWC_POWER_MODE_OFF);
  usleep(200000);
  if ((result = hwc->setPowerMode(hwc, HWC_DISPLAY_PRIMARY,
                                  HWC_POWER_MODE_NORMAL)) != 0) {
    std::fprintf(stderr, "HWC setPowerMode(NORMAL) failed: %d\n", result);
    hwc_close_1(hwc);
    allocator->free(allocator, handle);
    gralloc_close(allocator);
    return 1;
  }

  EGLDisplay egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface pbuffer = EGL_NO_SURFACE;
  EGLImageKHR image = EGL_NO_IMAGE_KHR;
  GLuint texture = 0;
  GLuint framebuffer = 0;
  bool success = false;
  if (egl_display == EGL_NO_DISPLAY ||
      !checkEgl(eglInitialize(egl_display, nullptr, nullptr), "eglInitialize"))
    goto cleanup;

  {
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE,
        EGL_PBUFFER_BIT,
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
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!checkEgl(
            eglChooseConfig(egl_display, config_attributes, &config, 1, &count),
            "eglChooseConfig") ||
        count == 0)
      goto cleanup;
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                         EGL_NONE};
    const EGLint pbuffer_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT,
                               context_attributes);
    pbuffer = eglCreatePbufferSurface(egl_display, config, pbuffer_attributes);
  }
  if (context == EGL_NO_CONTEXT || pbuffer == EGL_NO_SURFACE ||
      !checkEgl(eglMakeCurrent(egl_display, pbuffer, pbuffer, context),
                "eglMakeCurrent"))
    goto cleanup;

  {
    const auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    const auto image_target =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!create_image || !image_target) {
      std::fprintf(stderr, "required EGLImage entry points are unavailable\n");
      goto cleanup;
    }
    image = create_image(egl_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                         reinterpret_cast<EGLClientBuffer>(&native_buffer),
                         nullptr);
    if (image == EGL_NO_IMAGE_KHR) {
      std::fprintf(stderr, "eglCreateImageKHR failed: 0x%x\n", eglGetError());
      goto cleanup;
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    image_target(GL_TEXTURE_2D, reinterpret_cast<GLeglImageOES>(image));
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           texture, 0);
  }
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    std::fprintf(stderr, "RGB565 EGLImage framebuffer is incomplete: 0x%x\n",
                 glCheckFramebufferStatus(GL_FRAMEBUFFER));
    goto cleanup;
  }
  glViewport(0, 0, kWidth, kHeight);
  glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glFinish();
  if (glGetError() != GL_NO_ERROR || !present(hwc, handle, true) ||
      !present(hwc, handle, false) || !present(hwc, handle, false))
    goto cleanup;
  if (!setBacklight(180))
    goto cleanup;
  std::fprintf(stderr,
               "Nokia 8110 HWC1 RGB565 GPU frame visible for %u seconds\n",
               seconds);
  sleep(seconds);
  success = true;

cleanup:
  setBacklight(0);
  if (egl_display != EGL_NO_DISPLAY) {
    if (framebuffer)
      glDeleteFramebuffers(1, &framebuffer);
    if (texture)
      glDeleteTextures(1, &texture);
    if (image != EGL_NO_IMAGE_KHR) {
      const auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
          eglGetProcAddress("eglDestroyImageKHR"));
      if (destroy_image)
        destroy_image(egl_display, image);
    }
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (pbuffer != EGL_NO_SURFACE)
      eglDestroySurface(egl_display, pbuffer);
    if (context != EGL_NO_CONTEXT)
      eglDestroyContext(egl_display, context);
    eglTerminate(egl_display);
  }
  if (hwc->setPowerMode)
    hwc->setPowerMode(hwc, HWC_DISPLAY_PRIMARY, HWC_POWER_MODE_OFF);
  if (power && power->setInteractive)
    power->setInteractive(power, 0);
  hwc_close_1(hwc);
  allocator->free(allocator, handle);
  gralloc_close(allocator);
  return success ? 0 : 1;
}
