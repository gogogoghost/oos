#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>

static int fail(const char *step) {
  fprintf(stderr, "%s failed: EGL error 0x%04x\n", step, eglGetError());
  return 1;
}

int main(void) {
  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display == EGL_NO_DISPLAY) return fail("eglGetDisplay");
  if (!eglInitialize(display, NULL, NULL)) return fail("eglInitialize");
  if (!eglBindAPI(EGL_OPENGL_ES_API)) return fail("eglBindAPI");

  const EGLint config_attrs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
      EGL_NONE,
  };
  EGLConfig config;
  EGLint config_count;
  if (!eglChooseConfig(display, config_attrs, &config, 1, &config_count) || !config_count)
    return fail("eglChooseConfig");

  const EGLint pbuffer_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  EGLSurface surface = eglCreatePbufferSurface(display, config, pbuffer_attrs);
  if (surface == EGL_NO_SURFACE) return fail("eglCreatePbufferSurface");
  const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
  if (context == EGL_NO_CONTEXT) return fail("eglCreateContext");
  if (!eglMakeCurrent(display, surface, surface, context)) return fail("eglMakeCurrent");

  glClearColor(0.0f, 0.75f, 0.25f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  unsigned char pixel[4] = {0};
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  if (glGetError() != GL_NO_ERROR) {
    fputs("GLES draw/readback failed\n", stderr);
    return 1;
  }
  printf("EGL vendor: %s\nGLES renderer: %s\nGLES version: %s\npixel: %u,%u,%u,%u\n",
         eglQueryString(display, EGL_VENDOR), glGetString(GL_RENDERER),
         glGetString(GL_VERSION), pixel[0], pixel[1], pixel[2], pixel[3]);

  eglDestroyContext(display, context);
  eglDestroySurface(display, surface);
  eglTerminate(display);
  return 0;
}
