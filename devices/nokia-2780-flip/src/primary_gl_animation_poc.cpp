#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <android/hardware/power/1.0/IPower.h>
#include <binder/ProcessState.h>
#include <hardware/gralloc.h>
#include <ui/Fence.h>
#include <ui/FloatRect.h>
#include <ui/GraphicBuffer.h>
#include <ui/Rect.h>
#include <ui/Region.h>
#include <utils/Errors.h>

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

#include "HWC2.h"

namespace {

constexpr uint32_t kWidth = 240;
constexpr uint32_t kHeight = 320;

class PrimaryDisplayCallback final : public HWC2::ComposerCallback {
 public:
  explicit PrimaryDisplayCallback(HWC2::Device* device) : device_(device) {}

  void onHotplugReceived(int32_t, hwc2_display_t display,
                         HWC2::Connection connection) override {
    std::fprintf(stderr, "hotplug display=%" PRIu64 " connection=%d\n",
                 display, static_cast<int>(connection));
    device_->onHotplug(display, connection);
  }

  void onRefreshReceived(int32_t, hwc2_display_t) override {}
  void onVsyncReceived(int32_t, hwc2_display_t, int64_t) override {}

 private:
  HWC2::Device* const device_;
};

bool check_egl(EGLBoolean result, const char* operation) {
  if (result == EGL_TRUE) return true;
  std::fprintf(stderr, "%s failed: 0x%x\n", operation, eglGetError());
  return false;
}

bool set_primary_backlight(int value) {
  constexpr char kPath[] = "/sys/class/leds/lcd-backlight/brightness";
  char text[16];
  const int length = std::snprintf(text, sizeof(text), "%d\n", value);
  const int fd = open(kPath, O_WRONLY | O_CLOEXEC);
  if (fd < 0 || length <= 0 || length >= static_cast<int>(sizeof(text)) ||
      write(fd, text, static_cast<size_t>(length)) != length) {
    std::fprintf(stderr, "backlight write %d failed\n", value);
    if (fd >= 0) close(fd);
    return false;
  }
  close(fd);
  std::fprintf(stderr, "primary backlight=%d\n", value);
  return true;
}

bool check_gl_framebuffer() {
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status == GL_FRAMEBUFFER_COMPLETE) return true;
  std::fprintf(stderr, "GPU framebuffer incomplete: 0x%x\n", status);
  return false;
}

GLuint compile_shader(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) return shader;
  char log[256] = {};
  glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
  std::fprintf(stderr, "shader compile failed: %s\n", log);
  glDeleteShader(shader);
  return 0;
}

GLuint create_animation_program() {
  constexpr char kVertexShader[] =
      "attribute vec2 aPosition;\n"
      "uniform float uAngle;\n"
      "void main() {\n"
      "  float c = cos(uAngle);\n"
      "  float s = sin(uAngle);\n"
      "  vec2 p = vec2(c * aPosition.x - s * aPosition.y,\n"
      "                s * aPosition.x + c * aPosition.y) * 0.34;\n"
      "  gl_Position = vec4(p.x * 1.3333333, p.y, 0.0, 1.0);\n"
      "}\n";
  constexpr char kFragmentShader[] =
      "precision mediump float;\n"
      "void main() { gl_FragColor = vec4(0.15, 1.0, 0.20, 1.0); }\n";
  const GLuint vertex = compile_shader(GL_VERTEX_SHADER, kVertexShader);
  const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, kFragmentShader);
  if (vertex == 0 || fragment == 0) return 0;
  const GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glBindAttribLocation(program, 0, "aPosition");
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE) return program;
  char log[256] = {};
  glGetProgramInfoLog(program, sizeof(log), nullptr, log);
  std::fprintf(stderr, "shader link failed: %s\n", log);
  glDeleteProgram(program);
  return 0;
}

}  // namespace

int main() {
  android::ProcessState::self()->startThreadPool();

  auto power = ::android::hardware::power::V1_0::IPower::getService();
  if (power == nullptr || !power->setInteractive(true).isOk()) {
    std::fprintf(stderr, "failed to enable interactive power\n");
    return 1;
  }

  HWC2::Device device("default");
  PrimaryDisplayCallback callback(&device);
  device.registerCallback(&callback, 0);
  HWC2::Display* display = device.getDisplayById(device.getDefaultDisplayId());
  if (display == nullptr || !display->isConnected() ||
      display->setPowerMode(HWC2::PowerMode::On) != HWC2::Error::None) {
    std::fprintf(stderr, "failed to enable primary HWC display\n");
    return 1;
  }
  HWC2::Layer* client_layer = nullptr;
  const android::Rect frame(0, 0, kWidth, kHeight);
  if (display->createLayer(&client_layer) != HWC2::Error::None ||
      client_layer == nullptr ||
      client_layer->setCompositionType(HWC2::Composition::Client) !=
          HWC2::Error::None ||
      client_layer->setBlendMode(HWC2::BlendMode::None) != HWC2::Error::None ||
      client_layer->setSourceCrop(
          android::FloatRect(0.0f, 0.0f, kWidth, kHeight)) != HWC2::Error::None ||
      client_layer->setDisplayFrame(frame) != HWC2::Error::None ||
      client_layer->setVisibleRegion(android::Region(frame)) != HWC2::Error::None ||
      client_layer->setPlaneAlpha(1.0f) != HWC2::Error::None ||
      client_layer->setZOrder(0) != HWC2::Error::None) {
    std::fprintf(stderr, "failed to configure HWC client layer\n");
    return 1;
  }

  android::sp<android::GraphicBuffer> buffer = new android::GraphicBuffer(
      kWidth, kHeight, android::PIXEL_FORMAT_RGB_565, 1,
      static_cast<uint64_t>(GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER),
      "primary-green-direct-poc");
  if (buffer->initCheck() != android::NO_ERROR || buffer->handle == nullptr) {
    std::fprintf(stderr, "RGB565 GraphicBuffer allocation failed\n");
    return 1;
  }
  std::fprintf(stderr, "explicit buffer format=%d size=%ux%u stride=%u\n",
               buffer->getPixelFormat(), buffer->getWidth(), buffer->getHeight(),
               buffer->getStride());

  const EGLDisplay egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (egl_display == EGL_NO_DISPLAY ||
      !check_egl(eglInitialize(egl_display, nullptr, nullptr), "eglInitialize")) {
    return 1;
  }
  const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
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
  const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  const EGLContext context =
      eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attributes);
  const EGLint pbuffer_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  const EGLSurface pbuffer =
      eglCreatePbufferSurface(egl_display, config, pbuffer_attributes);
  if (context == EGL_NO_CONTEXT || pbuffer == EGL_NO_SURFACE ||
      !check_egl(eglMakeCurrent(egl_display, pbuffer, pbuffer, context),
                 "eglMakeCurrent")) {
    return 1;
  }

  const auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
      eglGetProcAddress("eglCreateImageKHR"));
  const auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
      eglGetProcAddress("eglDestroyImageKHR"));
  const auto image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
      eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (create_image == nullptr || destroy_image == nullptr ||
      image_target == nullptr) {
    std::fprintf(stderr, "EGLImage extension entry points are unavailable\n");
    return 1;
  }
  const EGLImageKHR image = create_image(
      egl_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
      reinterpret_cast<EGLClientBuffer>(buffer->getNativeBuffer()), nullptr);
  if (image == EGL_NO_IMAGE_KHR) {
    std::fprintf(stderr, "eglCreateImageKHR(native buffer) failed: 0x%x\n",
                 eglGetError());
    return 1;
  }

  GLuint texture = 0;
  GLuint framebuffer = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  image_target(GL_TEXTURE_2D, reinterpret_cast<GLeglImageOES>(image));
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         texture, 0);
  if (!check_gl_framebuffer()) return 1;
  const GLuint program = create_animation_program();
  if (program == 0) return 1;
  const GLint angle_uniform = glGetUniformLocation(program, "uAngle");
  constexpr GLfloat square[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f,
                                1.0f,  1.0f,  1.0f};

  android::sp<android::Fence> present_fence;
  const auto draw_and_present = [&](float angle) {
    if (present_fence != nullptr && present_fence->isValid()) {
      present_fence->waitForever("primary-green-animation");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, kWidth, kHeight);
    glClearColor(0.01f, 0.02f, 0.03f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glUniform1f(angle_uniform, angle);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, square);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glFinish();

    uint32_t changes = 0;
    uint32_t requests = 0;
    const HWC2::Error validate_error = display->validate(&changes, &requests);
    if ((validate_error != HWC2::Error::None &&
         validate_error != HWC2::Error::HasChanges) ||
        changes != 0 || requests != 0 ||
        display->acceptChanges() != HWC2::Error::None ||
        display->setClientTarget(0, buffer, android::Fence::NO_FENCE,
                                 android::ui::Dataspace::UNKNOWN) !=
            HWC2::Error::None ||
        display->present(&present_fence) != HWC2::Error::None) {
      std::fprintf(stderr,
                   "HWC animation frame failed: validate=%d changes=%u requests=%u\n",
                   static_cast<int>(validate_error), changes, requests);
      return false;
    }
    return true;
  };

  const auto start = std::chrono::steady_clock::now();
  if (!draw_and_present(0.0f) || !set_primary_backlight(255)) return 1;
  int frames = 1;
  auto next_frame = start;
  const auto deadline = start + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    const float seconds = std::chrono::duration<float>(
                            std::chrono::steady_clock::now() - start)
                            .count();
    if (!draw_and_present(seconds * 2.4f)) break;
    ++frames;
    next_frame += std::chrono::milliseconds(16);
    std::this_thread::sleep_until(next_frame);
  }
  std::fprintf(stderr, "animation frames submitted=%d\n", frames);

  set_primary_backlight(0);
  (void)display->setPowerMode(HWC2::PowerMode::Off);
  power->setInteractive(false);
  (void)display->destroyLayer(client_layer);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteProgram(program);
  glDeleteFramebuffers(1, &framebuffer);
  glDeleteTextures(1, &texture);
  destroy_image(egl_display, image);
  eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(egl_display, pbuffer);
  eglDestroyContext(egl_display, context);
  eglTerminate(egl_display);
  return 0;
}
