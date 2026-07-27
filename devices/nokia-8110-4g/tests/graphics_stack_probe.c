#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>

#include <stdio.h>
#include <string.h>

static void on_invalidate(const struct hwc_procs *procs) { (void)procs; }

static void on_vsync(const struct hwc_procs *procs, int display,
                     int64_t timestamp) {
  (void)procs;
  (void)display;
  (void)timestamp;
}

static void on_hotplug(const struct hwc_procs *procs, int display,
                       int connected) {
  (void)procs;
  printf("hwc hotplug: display=%d connected=%d\n", display, connected);
}

static const hwc_procs_t k_hwc_procs = {
    .invalidate = on_invalidate,
    .vsync = on_vsync,
    .hotplug = on_hotplug,
};

static int probe_hwc(void) {
  const hw_module_t *module = NULL;
  int result = hw_get_module(HWC_HARDWARE_MODULE_ID, &module);
  if (result != 0 || module == NULL) {
    fprintf(stderr, "hw_get_module(hwcomposer) failed: %d\n", result);
    return 1;
  }
  printf("hwc module: name=%s author=%s module_api=0x%04x hal_api=0x%04x\n",
         module->name ? module->name : "(null)",
         module->author ? module->author : "(null)", module->module_api_version,
         module->hal_api_version);

  hwc_composer_device_1_t *device = NULL;
  result = hwc_open_1(module, &device);
  if (result != 0 || device == NULL) {
    fprintf(stderr, "hwc_open_1 failed: %d\n", result);
    return 1;
  }
  printf("hwc device API: %u.%u header=%u (0x%08x)\n",
         (device->common.version >> 24) & 0xff,
         (device->common.version >> 16) & 0xff, device->common.version & 0xffff,
         device->common.version);
  if (device->registerProcs)
    device->registerProcs(device, &k_hwc_procs);

  if (device->query) {
    int value = 0;
    result = device->query(device, HWC_BACKGROUND_LAYER_SUPPORTED, &value);
    printf("hwc background layer: result=%d supported=%d\n", result, value);
    value = 0;
    result = device->query(device, HWC_VSYNC_PERIOD, &value);
    printf("hwc vsync period: result=%d ns=%d\n", result, value);
  }

  if (device->getDisplayConfigs && device->getDisplayAttributes) {
    uint32_t configs[8] = {0};
    size_t count = sizeof(configs) / sizeof(configs[0]);
    result =
        device->getDisplayConfigs(device, HWC_DISPLAY_PRIMARY, configs, &count);
    printf("hwc primary configs: result=%d count=%zu\n", result, count);
    if (result == 0 && count > 0) {
      const uint32_t attributes[] = {HWC_DISPLAY_WIDTH, HWC_DISPLAY_HEIGHT,
                                     HWC_DISPLAY_VSYNC_PERIOD,
                                     HWC_DISPLAY_DPI_X, HWC_DISPLAY_DPI_Y};
      const char *names[] = {"width", "height", "vsync_period", "dpi_x",
                             "dpi_y"};
      for (size_t index = 0; index < sizeof(attributes) / sizeof(attributes[0]);
           ++index) {
        const uint32_t request[] = {attributes[index],
                                    HWC_DISPLAY_NO_ATTRIBUTE};
        int32_t value = 0;
        result = device->getDisplayAttributes(device, HWC_DISPLAY_PRIMARY,
                                              configs[0], request, &value);
        printf("hwc primary %s: result=%d value=%d\n", names[index], result,
               value);
      }
    }
  }

  hwc_close_1(device);
  return 0;
}

static int probe_gralloc(void) {
  const hw_module_t *hardware_module = NULL;
  int result = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &hardware_module);
  if (result != 0 || hardware_module == NULL) {
    fprintf(stderr, "hw_get_module(gralloc) failed: %d\n", result);
    return 1;
  }
  printf("gralloc module: name=%s author=%s module_api=0x%04x\n",
         hardware_module->name ? hardware_module->name : "(null)",
         hardware_module->author ? hardware_module->author : "(null)",
         hardware_module->module_api_version);

  alloc_device_t *allocator = NULL;
  result = gralloc_open(hardware_module, &allocator);
  if (result != 0 || allocator == NULL) {
    fprintf(stderr, "gralloc_open failed: %d\n", result);
    return 1;
  }

  buffer_handle_t handle = NULL;
  int stride = 0;
  result = allocator->alloc(allocator, 240, 320, HAL_PIXEL_FORMAT_RGB_565,
                            GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER,
                            &handle, &stride);
  printf("gralloc RGB565 allocation: result=%d handle=%p stride=%d\n", result,
         handle, stride);
  if (result == 0 && handle != NULL)
    allocator->free(allocator, handle);
  gralloc_close(allocator);
  return result == 0 ? 0 : 1;
}

static int has_extension(const char *extensions, const char *extension) {
  return extensions != NULL && strstr(extensions, extension) != NULL;
}

static int probe_egl(void) {
  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major = 0;
  EGLint minor = 0;
  if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
    fprintf(stderr, "eglInitialize failed: 0x%x\n", eglGetError());
    return 1;
  }

  const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
  printf("EGL: version=%d.%d vendor=%s native_buffer=%d image_base=%d\n", major,
         minor, eglQueryString(display, EGL_VENDOR),
         has_extension(extensions, "EGL_ANDROID_image_native_buffer"),
         has_extension(extensions, "EGL_KHR_image_base"));

  const EGLint config_attributes[] = {EGL_SURFACE_TYPE,
                                      EGL_PBUFFER_BIT,
                                      EGL_RENDERABLE_TYPE,
                                      EGL_OPENGL_ES2_BIT,
                                      EGL_RED_SIZE,
                                      5,
                                      EGL_GREEN_SIZE,
                                      6,
                                      EGL_BLUE_SIZE,
                                      5,
                                      EGL_NONE};
  EGLConfig config = NULL;
  EGLint config_count = 0;
  if (!eglChooseConfig(display, config_attributes, &config, 1, &config_count) ||
      config_count == 0) {
    fprintf(stderr, "eglChooseConfig(RGB565) failed: 0x%x\n", eglGetError());
    eglTerminate(display);
    return 1;
  }

  const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  EGLContext context =
      eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
  EGLSurface surface =
      eglCreatePbufferSurface(display, config, surface_attributes);
  if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
      !eglMakeCurrent(display, surface, surface, context)) {
    fprintf(stderr, "EGL GLES2 context creation failed: 0x%x\n", eglGetError());
    if (surface != EGL_NO_SURFACE)
      eglDestroySurface(display, surface);
    if (context != EGL_NO_CONTEXT)
      eglDestroyContext(display, context);
    eglTerminate(display);
    return 1;
  }
  printf("GLES: vendor=%s renderer=%s version=%s\n", glGetString(GL_VENDOR),
         glGetString(GL_RENDERER), glGetString(GL_VERSION));

  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(display, surface);
  eglDestroyContext(display, context);
  eglTerminate(display);
  return 0;
}

int main(void) {
  int failures = 0;
  failures += probe_hwc();
  failures += probe_gralloc();
  failures += probe_egl();
  printf("graphics probe: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
