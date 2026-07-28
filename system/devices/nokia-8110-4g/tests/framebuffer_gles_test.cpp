#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <system/window.h>
#define INCLUDED_FROM_FRAMEBUFFER_NATIVE_WINDOW_CPP
#include <ui/FramebufferNativeWindow.h>
#undef INCLUDED_FROM_FRAMEBUFFER_NATIVE_WINDOW_CPP
#include <utils/StrongPointer.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

constexpr char kBacklightPath[] = "/sys/class/leds/lcd-backlight/brightness";

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

} // namespace

int main(int argc, char **argv) {
  const unsigned seconds =
      argc == 2 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10))
                : 10;
  if (!setBacklight(0))
    return 1;

  const hw_module_t *gralloc_module = nullptr;
  framebuffer_device_t *direct_framebuffer = nullptr;
  int result = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &gralloc_module);
  if (result == 0)
    result = framebuffer_open(gralloc_module, &direct_framebuffer);
  std::fprintf(stderr, "direct framebuffer_open: result=%d device=%p\n", result,
               direct_framebuffer);
  if (direct_framebuffer)
    framebuffer_close(direct_framebuffer);

  android::sp<android::FramebufferNativeWindow> window =
      new android::FramebufferNativeWindow();
  auto *framebuffer = const_cast<framebuffer_device_t *>(window->getDevice());
  if (!framebuffer) {
    std::fprintf(stderr, "FramebufferNativeWindow did not open fb0\n");
    return 1;
  }
  std::fprintf(
      stderr, "framebuffer: %ux%u stride=%d format=%d buffers=%d fps=%.2f\n",
      framebuffer->width, framebuffer->height, framebuffer->stride,
      framebuffer->format, framebuffer->numFramebuffers, framebuffer->fps);
  if (framebuffer->width != 240 || framebuffer->height != 320 ||
      framebuffer->format != HAL_PIXEL_FORMAT_RGB_565) {
    std::fprintf(stderr, "unexpected Nokia 8110 framebuffer geometry\n");
    return 1;
  }
  if (framebuffer->enableScreen && framebuffer->enableScreen(framebuffer, 1)) {
    std::fprintf(stderr, "framebuffer enableScreen failed\n");
    return 1;
  }

  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display == EGL_NO_DISPLAY ||
      !checkEgl(eglInitialize(display, nullptr, nullptr), "eglInitialize")) {
    return 1;
  }
  const EGLint config_attributes[] = {EGL_SURFACE_TYPE,
                                      EGL_WINDOW_BIT,
                                      EGL_RENDERABLE_TYPE,
                                      EGL_OPENGL_ES2_BIT,
                                      EGL_NATIVE_VISUAL_ID,
                                      framebuffer->format,
                                      EGL_RED_SIZE,
                                      5,
                                      EGL_GREEN_SIZE,
                                      6,
                                      EGL_BLUE_SIZE,
                                      5,
                                      EGL_ALPHA_SIZE,
                                      0,
                                      EGL_NONE};
  EGLConfig config = nullptr;
  EGLint config_count = 0;
  if (!checkEgl(eglChooseConfig(display, config_attributes, &config, 1,
                                &config_count),
                "eglChooseConfig") ||
      config_count != 1) {
    std::fprintf(stderr, "no RGB565 framebuffer EGL config\n");
    return 1;
  }

  const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context =
      eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
  EGLSurface surface = eglCreateWindowSurface(
      display, config, reinterpret_cast<EGLNativeWindowType>(window.get()),
      nullptr);
  if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
      !checkEgl(eglMakeCurrent(display, surface, surface, context),
                "eglMakeCurrent") ||
      !checkEgl(eglSwapInterval(display, 1), "eglSwapInterval")) {
    std::fprintf(stderr, "window surface setup failed: 0x%x\n", eglGetError());
    return 1;
  }

  for (int frame = 0; frame < 3; ++frame) {
    glViewport(0, 0, framebuffer->width, framebuffer->height);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (glGetError() != GL_NO_ERROR ||
        !checkEgl(eglSwapBuffers(display, surface), "eglSwapBuffers")) {
      return 1;
    }
  }
  if (!setBacklight(200))
    return 1;
  std::fprintf(stderr, "Nokia 8110 GPU framebuffer test visible for %u s\n",
               seconds);
  sleep(seconds);

  setBacklight(0);
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(display, surface);
  eglDestroyContext(display, context);
  eglTerminate(display);
  if (framebuffer->enableScreen)
    framebuffer->enableScreen(framebuffer, 0);
  window.clear();
  return 0;
}
