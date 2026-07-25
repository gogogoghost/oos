#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * libxul.so exports these methods on the target build.  ANativeWindow is
 * opaque here: EGL accepts it as EGLNativeWindowType.  The probe avoids the
 * unstable GonkDisplay vtable and resolves the exact exported methods.
 */
typedef void *(*GetGonkDisplayFn)(void);
typedef void (*SetDisplayEnabledFn)(void *, int);
typedef void (*SetDisplayVisibilityFn)(void *, int);
/* android::sp<T> is a non-trivial C++ return type on ARM EABI. */
typedef void (*GetSurfaceFn)(void *out_sp, void *gonk_display,
                             unsigned int display_type);
typedef int (*SwapBuffersFn)(void *gonk_display, unsigned int display_type);

static int egl_error(const char *where) {
  fprintf(stderr, "%s: EGL error 0x%04x\n", where, eglGetError());
  return 1;
}

static int post_external_cpu(ANativeWindow *window) {
  ANativeWindow_Buffer buffer;
  int lock_result = ANativeWindow_lock(window, &buffer, 0);
  if (lock_result) {
    fprintf(stderr, "ANativeWindow_lock(external) failed: %d\n", lock_result);
    return 9;
  }
  if (buffer.format != 4) {
    fprintf(stderr, "unexpected external format: %d\n", buffer.format);
    return 10;
  }
  unsigned short *pixels = buffer.bits;
  for (int y = 0; y < buffer.height; ++y) {
    for (int x = 0; x < buffer.stride; ++x) pixels[y * buffer.stride + x] = 0x001f;
  }
  int post_result = ANativeWindow_unlockAndPost(window);
  if (post_result) {
    fprintf(stderr, "ANativeWindow_unlockAndPost(external) failed: %d\n", post_result);
    return 11;
  }
  fprintf(stderr, "CPU BufferQueue post succeeded for external (blue)\n");
  return 0;
}

int main(int argc, char **argv) {
  const int combined = argc > 1 && argv[1][0] == 'm';
  const int power_cycle = argc > 1 && argv[1][0] == 'p';
  const int both = argc > 1 && (argv[1][0] == 'b' || combined);
  const int cpu_external = argc > 1 && (argv[1][0] == 'c' || combined);
  const unsigned int display_type = (argc > 1 &&
                                     (argv[1][0] == 'e' || cpu_external)) ? 1 : 0;
  const int external_only = display_type && !both;
  unsigned int hold_seconds = argc > 2 ? (unsigned int)strtoul(argv[2], 0, 10) : 60;
  fprintf(stderr, "stage: dlopen libxul\n");
  void *xul = dlopen("/system/b2g/libxul.so", RTLD_NOW | RTLD_GLOBAL);
  if (!xul) {
    fprintf(stderr, "dlopen libxul.so failed: %s\n", dlerror());
    return 2;
  }

  GetGonkDisplayFn get_display =
      (GetGonkDisplayFn)dlsym(xul, "_ZN7mozilla14GetGonkDisplayEv");
  SetDisplayEnabledFn set_primary = (SetDisplayEnabledFn)dlsym(
      xul, "_ZN7mozilla12GonkDisplayP10SetEnabledEb");
  SetDisplayEnabledFn set_external = (SetDisplayEnabledFn)dlsym(
      xul, "_ZN7mozilla12GonkDisplayP13SetExtEnabledEb");
  SetDisplayVisibilityFn set_visibility = (SetDisplayVisibilityFn)dlsym(
      xul, "_ZN7mozilla12GonkDisplayP20SetDisplayVisibilityEb");
  GetSurfaceFn get_surface = (GetSurfaceFn)dlsym(
      xul, "_ZN7mozilla12GonkDisplayP10GetSurfaceE11DisplayType");
  SwapBuffersFn post_to_hwc = (SwapBuffersFn)dlsym(
      xul, "_ZN7mozilla12GonkDisplayP11SwapBuffersE11DisplayType");
  if (!get_display || !set_primary || !set_external || !set_visibility || !get_surface ||
      !post_to_hwc) {
    fprintf(stderr, "required Gonk display exports are unavailable\n");
    return 3;
  }

  fprintf(stderr, "stage: GetGonkDisplay\n");
  void *gonk_display = get_display();
  if (!gonk_display) {
    fprintf(stderr, "GetGonkDisplay returned null\n");
    return 4;
  }
  fprintf(stderr, "stage: primary surface\n");
  void *primary_window = 0;
  if (!external_only) {
    if (power_cycle) {
      fprintf(stderr, "stage: primary HWC power cycle\n");
      set_primary(gonk_display, 0);
      usleep(200000);
    }
    set_primary(gonk_display, 1);
    set_visibility(gonk_display, 1);
    get_surface(&primary_window, gonk_display, 0);
    if (!primary_window) {
      fprintf(stderr, "GetSurface(primary) returned null\n");
      return 5;
    }
    fprintf(stderr, "primary window=%p %dx%d format=%d\n", primary_window,
            ANativeWindow_getWidth((ANativeWindow *)primary_window),
            ANativeWindow_getHeight((ANativeWindow *)primary_window),
            ANativeWindow_getFormat((ANativeWindow *)primary_window));
  }
  void *external_window = 0;
  if (both || display_type) {
    fprintf(stderr, "stage: external surface\n");
    set_external(gonk_display, 1);
    get_surface(&external_window, gonk_display, 1);
    if (!external_window) {
      fprintf(stderr, "GetSurface(external) returned null\n");
      return 6;
    }
    fprintf(stderr, "external window=%p %dx%d format=%d\n", external_window,
            ANativeWindow_getWidth((ANativeWindow *)external_window),
            ANativeWindow_getHeight((ANativeWindow *)external_window),
            ANativeWindow_getFormat((ANativeWindow *)external_window));
  }

  fprintf(stderr, "stage: EGL initialize\n");
  EGLDisplay egl = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (egl == EGL_NO_DISPLAY || !eglInitialize(egl, 0, 0)) return egl_error("eglInitialize");
  const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                          EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6,
                          EGL_BLUE_SIZE, 5, EGL_NONE};
  EGLConfig config;
  EGLint count;
  if (!eglChooseConfig(egl, attrs, &config, 1, &count) || !count) return egl_error("eglChooseConfig");
  const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context = eglCreateContext(egl, config, EGL_NO_CONTEXT, context_attrs);
  if (context == EGL_NO_CONTEXT) return egl_error("eglCreateContext");
  if (primary_window) {
    EGLSurface primary = eglCreateWindowSurface(egl, config,
        (EGLNativeWindowType)primary_window, 0);
    if (primary == EGL_NO_SURFACE) return egl_error("eglCreateWindowSurface(primary)");
    if (!eglMakeCurrent(egl, primary, primary, context)) return egl_error("eglMakeCurrent(primary)");
    glViewport(0, 0, 240, 320);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!eglSwapBuffers(egl, primary)) return egl_error("eglSwapBuffers(primary)");
    if (!post_to_hwc(gonk_display, 0)) {
      fprintf(stderr, "GonkDisplayP::SwapBuffers(primary) failed\n");
      return 7;
    }
    fprintf(stderr, "GPU window-surface swap succeeded for primary (green)\n");
  }

  if (cpu_external) {
    int result = post_external_cpu((ANativeWindow *)external_window);
    if (result) return result;
  }

  if (external_window && !cpu_external) {
    EGLSurface external = eglCreateWindowSurface(egl, config,
        (EGLNativeWindowType)external_window, 0);
    if (external == EGL_NO_SURFACE) return egl_error("eglCreateWindowSurface(external)");
    if (!eglMakeCurrent(egl, external, external, context)) return egl_error("eglMakeCurrent(external)");
    glViewport(0, 0, 128, 160);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!eglSwapBuffers(egl, external)) return egl_error("eglSwapBuffers(external)");
    if (!post_to_hwc(gonk_display, 1)) {
      fprintf(stderr, "GonkDisplayP::SwapBuffers(external) failed\n");
      return 8;
    }
    fprintf(stderr, "GPU window-surface swap succeeded for external (blue)\n");
  }
  sleep(hold_seconds);
  return 0;
}
