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

#include <glib.h>
#include <wpe/webkit.h>
#include <wpe-android/view-backend.h>

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <memory>
#include <unordered_map>

#include "HWC2.h"

extern "C" {
typedef struct _WebKitWebViewBackend WebKitWebViewBackend;
WebKitWebViewBackend* webkit_web_view_backend_new(struct wpe_view_backend*,
                                                   GDestroyNotify, gpointer);
WebKitWebView* webkit_web_view_new(WebKitWebViewBackend*);
void webkit_web_view_load_html(WebKitWebView*, const gchar*, const gchar*);
}

namespace {

constexpr uint32_t kWidth = 240;
constexpr uint32_t kHeight = 320;

class CoverRenderer {
 public:
  bool initialize() {
    fd_ = open("/dev/graphics/fb1", O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
      std::perror("open fb1");
      return false;
    }
    if (ioctl(fd_, FBIOGET_FSCREENINFO, &fix_) != 0 ||
        ioctl(fd_, FBIOGET_VSCREENINFO, &var_) != 0) {
      std::perror("query fb1");
      return false;
    }
    if (var_.bits_per_pixel != 16 || var_.xres == 0 || var_.yres == 0 ||
        fix_.line_length < var_.xres * sizeof(uint16_t)) {
      std::fprintf(stderr, "unsupported fb1: %ux%u bpp=%u stride=%u\n",
                   var_.xres, var_.yres, var_.bits_per_pixel, fix_.line_length);
      return false;
    }
    map_length_ = static_cast<size_t>(fix_.line_length) * var_.yres_virtual;
    pixels_ = static_cast<uint8_t*>(mmap(nullptr, map_length_, PROT_READ | PROT_WRITE,
                                         MAP_SHARED, fd_, 0));
    if (pixels_ == MAP_FAILED) {
      pixels_ = nullptr;
      std::perror("mmap fb1");
      return false;
    }

    var_.xoffset = 0;
    var_.yoffset = 0;
    var_.activate = FB_ACTIVATE_NOW;
    if (ioctl(fd_, FBIOPUT_VSCREENINFO, &var_) != 0 ||
        ioctl(fd_, FBIOBLANK, FB_BLANK_UNBLANK) != 0) {
      std::perror("enable fb1");
      return false;
    }
    draw();
    if (!setBacklight(255)) return false;
    std::fprintf(stderr, "cover framebuffer: %ux%u virtual=%ux%u stride=%u\n",
                 var_.xres, var_.yres, var_.xres_virtual, var_.yres_virtual,
                 fix_.line_length);
    std::fflush(stderr);
    return true;
  }

 private:
  static constexpr uint16_t kBackground = 0x01c4;  // #063b22 in RGB565.
  static constexpr uint16_t kForeground = 0xffff;  // #f6fff8 in RGB565.

  static const uint8_t* glyph(char character) {
    static constexpr uint8_t kS[7] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
    static constexpr uint8_t kA[7] = {0x00, 0x0e, 0x01, 0x0f, 0x11, 0x11, 0x0f};
    static constexpr uint8_t kC[7] = {0x00, 0x0e, 0x10, 0x10, 0x10, 0x10, 0x0e};
    static constexpr uint8_t kD[7] = {0x01, 0x01, 0x0f, 0x11, 0x11, 0x11, 0x0f};
    static constexpr uint8_t kE[7] = {0x00, 0x0e, 0x11, 0x1f, 0x10, 0x10, 0x0f};
    static constexpr uint8_t kN[7] = {0x00, 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11};
    static constexpr uint8_t kO[7] = {0x00, 0x0e, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static constexpr uint8_t kR[7] = {0x00, 0x16, 0x19, 0x10, 0x10, 0x10, 0x10};
    static constexpr uint8_t kY[7] = {0x00, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e};
    static constexpr uint8_t kSpace[7] = {};
    switch (character) {
      case 'S': return kS;
      case 'a': return kA;
      case 'c': return kC;
      case 'd': return kD;
      case 'e': return kE;
      case 'n': return kN;
      case 'o': return kO;
      case 'r': return kR;
      case 's': return kS;
      case 'y': return kY;
      default: return kSpace;
    }
  }

  void setPixel(int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= static_cast<int>(var_.xres) ||
        y >= static_cast<int>(var_.yres_virtual)) {
      return;
    }
    auto* row = reinterpret_cast<uint16_t*>(pixels_ + y * fix_.line_length);
    row[x] = color;
  }

  void drawText(const char* text, int origin_x, int origin_y, int scale) {
    int x = origin_x;
    for (const char* current = text; *current; ++current) {
      const uint8_t* rows = glyph(*current);
      for (int row = 0; row < 7; ++row) {
        for (int column = 0; column < 5; ++column) {
          if ((rows[row] & (1u << (4 - column))) == 0) continue;
          for (int dy = 0; dy < scale; ++dy) {
            for (int dx = 0; dx < scale; ++dx)
              setPixel(x + column * scale + dx, origin_y + row * scale + dy,
                       kForeground);
          }
        }
      }
      x += 6 * scale;
    }
  }

  void draw() {
    for (uint32_t y = 0; y < var_.yres_virtual; ++y) {
      auto* row = reinterpret_cast<uint16_t*>(pixels_ + y * fix_.line_length);
      for (uint32_t x = 0; x < var_.xres; ++x) row[x] = kBackground;
    }

    constexpr int kScale = 2;
    constexpr int kSecondaryWidth = 9 * 6 * kScale;
    constexpr int kScreenWidth = 6 * 6 * kScale;
    const int center_y = static_cast<int>(var_.yres) / 2;
    for (uint32_t page = 0; page < var_.yres_virtual; page += var_.yres) {
      drawText("Secondary", (static_cast<int>(var_.xres) - kSecondaryWidth) / 2,
               center_y - 17 + static_cast<int>(page), kScale);
      drawText("screen", (static_cast<int>(var_.xres) - kScreenWidth) / 2,
               center_y + 3 + static_cast<int>(page), kScale);
    }
    msync(pixels_, map_length_, MS_SYNC);
    var_.activate = FB_ACTIVATE_VBL;
    if (ioctl(fd_, FBIOPUT_VSCREENINFO, &var_) != 0) std::perror("present fb1");
  }

  static bool setBacklight(int value) {
    FILE* file = std::fopen("/sys/class/leds/sublcd-backlight/brightness", "w");
    if (!file) {
      std::perror("cover backlight");
      return false;
    }
    const bool ok = std::fprintf(file, "%d\n", value) > 0;
    std::fclose(file);
    return ok;
  }

  int fd_ = -1;
  fb_fix_screeninfo fix_ {};
  fb_var_screeninfo var_ {};
  uint8_t* pixels_ = nullptr;
  size_t map_length_ = 0;
};

class DisplayCallback final : public HWC2::ComposerCallback {
 public:
  explicit DisplayCallback(HWC2::Device* device) : device_(device) {}
  void onHotplugReceived(int32_t, hwc2_display_t display,
                         HWC2::Connection connection) override {
    device_->onHotplug(display, connection);
    auto* current = device_->getDisplayById(display);
    std::shared_ptr<const HWC2::Display::Config> config;
    std::string name;
    if (current && current->isConnected()) {
      current->getActiveConfig(&config);
      current->getName(&name);
    }
    std::fprintf(stderr, "HWC hotplug id=%llu connection=%d name=%s size=%dx%d\n",
                 static_cast<unsigned long long>(display),
                 static_cast<int>(connection), name.c_str(),
                 config ? config->getWidth() : 0, config ? config->getHeight() : 0);
    std::fflush(stderr);
  }
  void onRefreshReceived(int32_t, hwc2_display_t) override {}
  void onVsyncReceived(int32_t, hwc2_display_t, int64_t) override {}

 private:
  HWC2::Device* const device_;
};

class Renderer {
 public:
  bool initialize() {
    power_ = ::android::hardware::power::V1_0::IPower::getService();
    if (power_ == nullptr || !power_->setInteractive(true).isOk()) return false;

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
    display_->getActiveConfig(&config);
    std::fprintf(stderr, "HWC selected primary id=%llu size=%dx%d\n",
                 static_cast<unsigned long long>(HWC_DISPLAY_PRIMARY),
                 config ? config->getWidth() : 0, config ? config->getHeight() : 0);
    std::fflush(stderr);

    const android::Rect frame(0, 0, kWidth, kHeight);
    if (display_->createLayer(&layer_) != HWC2::Error::None || layer_ == nullptr ||
        layer_->setCompositionType(HWC2::Composition::Client) != HWC2::Error::None ||
        layer_->setBlendMode(HWC2::BlendMode::None) != HWC2::Error::None ||
        layer_->setSourceCrop(android::FloatRect(0, 0, kWidth, kHeight)) !=
            HWC2::Error::None ||
        layer_->setDisplayFrame(frame) != HWC2::Error::None ||
        layer_->setVisibleRegion(android::Region(frame)) != HWC2::Error::None ||
        layer_->setPlaneAlpha(1.f) != HWC2::Error::None ||
        layer_->setZOrder(0) != HWC2::Error::None) {
      return false;
    }
    client_target_ = new android::GraphicBuffer(
        kWidth, kHeight, android::PIXEL_FORMAT_RGB_565, 1,
        static_cast<uint64_t>(GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_COMPOSER),
        "wpe-primary-client-target");
    if (client_target_->initCheck() != android::NO_ERROR ||
        client_target_->handle == nullptr || !initializeGpu()) {
      return false;
    }
    return setBacklight(255);
  }

  void present(WPEAndroidViewBackend* backend, WPEAndroidBuffer* buffer,
               int acquire_fence_fd) {
    std::lock_guard<std::mutex> lock(present_mutex_);
    auto* raw = WPEAndroidBuffer_getAHardwareBuffer(buffer);
    if (!blitToClientTarget(raw, acquire_fence_fd) ||
        !submitClientTarget(0, client_target_, android::Fence::NO_FENCE)) {
      WPEAndroidViewBackend_dispatchReleaseBuffer(backend, buffer);
      return;
    }
    last_target_ = client_target_;
    last_slot_ = 0;
    WPEAndroidViewBackend_dispatchReleaseBuffer(backend, buffer);
    WPEAndroidViewBackend_dispatchFrameComplete(backend);
  }

  void refresh() {
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (last_target_ != nullptr)
      submitClientTarget(last_slot_, last_target_, android::Fence::NO_FENCE);
  }

 private:
  bool submitClientTarget(uint32_t slot, const android::sp<android::GraphicBuffer>& target,
                          const android::sp<android::Fence>& acquire) {
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
                                  android::ui::Dataspace::UNKNOWN) != HWC2::Error::None) {
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

  uint32_t bufferSlot(AHardwareBuffer* raw, const android::GraphicBuffer* buffer) {
    const auto existing = slots_.find(raw);
    if (existing != slots_.end()) return existing->second;
    const uint32_t slot = static_cast<uint32_t>(slots_.size());
    slots_.emplace(raw, slot);
    std::fprintf(stderr, "WPE direct RGB565 buffer=%p slot=%u format=%d size=%ux%u stride=%u\n",
                 raw, slot, buffer->getPixelFormat(), buffer->getWidth(),
                 buffer->getHeight(), buffer->getStride());
    std::fflush(stderr);
    return slot;
  }

  static bool checkEgl(EGLBoolean result, const char* operation) {
    if (result == EGL_TRUE) return true;
    std::fprintf(stderr, "%s failed: 0x%x\n", operation, eglGetError());
    return false;
  }

  static GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;
    char log[256] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::fprintf(stderr, "shader compilation failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
  }

  bool initializeGpu() {
    egl_display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display_ == EGL_NO_DISPLAY ||
        !checkEgl(eglInitialize(egl_display_, nullptr, nullptr), "eglInitialize")) {
      return false;
    }
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!checkEgl(eglChooseConfig(egl_display_, config_attributes, &config, 1, &count),
                  "eglChooseConfig") || count == 0) {
      return false;
    }
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl_context_ = eglCreateContext(egl_display_, config, EGL_NO_CONTEXT,
                                    context_attributes);
    const EGLint pbuffer_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    pbuffer_ = eglCreatePbufferSurface(egl_display_, config, pbuffer_attributes);
    if (egl_context_ == EGL_NO_CONTEXT || pbuffer_ == EGL_NO_SURFACE ||
        !checkEgl(eglMakeCurrent(egl_display_, pbuffer_, pbuffer_, egl_context_),
                  "eglMakeCurrent")) {
      return false;
    }
    create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    image_target_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!create_image_ || !destroy_image_ || !image_target_) return false;

    target_image_ = create_image_(
        egl_display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
        reinterpret_cast<EGLClientBuffer>(client_target_->getNativeBuffer()), nullptr);
    if (target_image_ == EGL_NO_IMAGE_KHR) return false;

    glGenTextures(1, &source_texture_);
    glGenTextures(1, &target_texture_);
    glBindTexture(GL_TEXTURE_2D, target_texture_);
    image_target_(GL_TEXTURE_2D, reinterpret_cast<GLeglImageOES>(target_image_));
    glGenFramebuffers(1, &framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           target_texture_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;

    constexpr char vertex_shader[] =
        "attribute vec2 position;\n"
        "attribute vec2 texcoord;\n"
        "varying vec2 vTexcoord;\n"
        "void main() { gl_Position = vec4(position, 0.0, 1.0); vTexcoord = texcoord; }\n";
    constexpr char fragment_shader[] =
        "precision mediump float;\n"
        "varying vec2 vTexcoord;\n"
        "uniform sampler2D source;\n"
        "void main() { gl_FragColor = texture2D(source, vTexcoord); }\n";
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertex_shader);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragment_shader);
    if (!vertex || !fragment) return false;
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
    if (linked != GL_TRUE) return false;
    source_uniform_ = glGetUniformLocation(program_, "source");
    return source_uniform_ >= 0;
  }

  bool blitToClientTarget(AHardwareBuffer* source, int acquire_fence_fd) {
    if (acquire_fence_fd >= 0) {
      android::sp<android::Fence> acquire = new android::Fence(acquire_fence_fd);
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
        reinterpret_cast<EGLClientBuffer>(source_graphic->getNativeBuffer()), nullptr);
    if (source_image == EGL_NO_IMAGE_KHR) {
      std::fprintf(stderr, "eglCreateImageKHR(WPE source) failed: 0x%x\n", eglGetError());
      return false;
    }
    glBindTexture(GL_TEXTURE_2D, source_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target_(GL_TEXTURE_2D, reinterpret_cast<GLeglImageOES>(source_image));
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, kWidth, kHeight);
    glUseProgram(program_);
    constexpr GLfloat positions[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
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
    if (error != GL_NO_ERROR) std::fprintf(stderr, "WPE GPU blit GL error: 0x%x\n", error);
    return error == GL_NO_ERROR;
  }

  bool setBacklight(int value) {
    FILE* file = std::fopen("/sys/class/leds/lcd-backlight/brightness", "w");
    if (!file) return false;
    const bool ok = std::fprintf(file, "%d\n", value) > 0;
    std::fclose(file);
    return ok;
  }

  ::android::sp<::android::hardware::power::V1_0::IPower> power_;
  std::unique_ptr<HWC2::Device> device_;
  std::unique_ptr<DisplayCallback> callback_;
  HWC2::Display* display_ = nullptr;
  HWC2::Layer* layer_ = nullptr;
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
  std::unordered_map<AHardwareBuffer*, uint32_t> slots_;
};

struct CommitContext {
  Renderer* renderer;
  WPEAndroidViewBackend* backend;
};

void commit_buffer(void* context, WPEAndroidBuffer* buffer, int fence_fd) {
  auto* commit = static_cast<CommitContext*>(context);
  commit->renderer->present(commit->backend, buffer, fence_fd);
}

void destroy_backend(gpointer backend) {
  WPEAndroidViewBackend_destroy(static_cast<WPEAndroidViewBackend*>(backend));
}

gboolean initialize_cover_after_primary(gpointer cover) {
  auto* renderer = static_cast<CoverRenderer*>(cover);
  if (!renderer->initialize()) {
    std::fprintf(stderr, "failed to initialize Nokia 2780 cover display\n");
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_REMOVE;
}

gboolean refresh_primary(gpointer renderer) {
  static_cast<Renderer*>(renderer)->refresh();
  return G_SOURCE_CONTINUE;
}

}  // namespace

int main() {
  android::ProcessState::self()->startThreadPool();
  Renderer renderer;
  if (!renderer.initialize()) {
    std::fprintf(stderr, "failed to initialize Nokia 2780 primary display\n");
    return 1;
  }
  auto* backend = WPEAndroidViewBackend_create(kWidth, kHeight);
  if (!backend) return 1;
  CommitContext commit { &renderer, backend };
  WPEAndroidViewBackend_setCommitBufferHandler(backend, &commit, commit_buffer);
  auto* wrapped = webkit_web_view_backend_new(
      WPEAndroidViewBackend_getWPEViewBackend(backend), destroy_backend, backend);
  auto* view = webkit_web_view_new(wrapped);
  if (!view) {
    std::fprintf(stderr, "failed to create WebKit view\n");
    destroy_backend(backend);
    return 1;
  }

  gchar* html = nullptr;
  gsize html_size = 0;
  GError* error = nullptr;
  if (!g_file_get_contents("/data/local/tmp/oos-wpe/hello.html", &html, &html_size,
                           &error)) {
    std::fprintf(stderr, "failed to read hello.html: %s\n",
                 error ? error->message : "unknown error");
    if (error) g_error_free(error);
    destroy_backend(backend);
    return 1;
  }
  webkit_web_view_load_html(view, html, "file:///data/local/tmp/oos-wpe/");
  g_free(html);
  CoverRenderer cover;
  // The primary SPI panel periodically recovers from ESD and loses its
  // scanout target. Re-submit the retained GPU buffer without CPU copying.
  g_timeout_add(500, refresh_primary, &renderer);
  const char* enable_cover = std::getenv("OOS_ENABLE_COVER");
  if (enable_cover == nullptr || std::strcmp(enable_cover, "0") != 0) {
    // Keep fb1 out of the first HWC present; its panel init otherwise races fb0.
    g_timeout_add(2000, initialize_cover_after_primary, &cover);
  } else {
    std::fprintf(stderr, "cover output disabled for isolated primary test\n");
  }
  g_main_loop_run(g_main_loop_new(nullptr, FALSE));
  return 0;
}
