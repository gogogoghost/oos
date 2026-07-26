#include "oos/nokia2780/display_control.h"

#include <binder/ProcessState.h>
#include <cutils/properties.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "HWC2.h"

namespace {

constexpr char kComposerService[] = "vendor.hwcomposer-2-1";
constexpr char kComposerStateProperty[] = "init.svc.vendor.hwcomposer-2-1";
constexpr char kPrimaryBacklight[] = "/sys/class/leds/lcd-backlight/brightness";
constexpr char kCoverBacklight[] =
    "/sys/class/leds/sublcd-backlight/brightness";
constexpr char kCoverFramebuffer[] = "/dev/graphics/fb1";
constexpr useconds_t kCoverPanelInitUs = 180000;
constexpr useconds_t kCoverTransferSettleUs = 100000;

int g_cover_fd = -1;
uint8_t *g_cover_mapping = nullptr;
size_t g_cover_mapping_length = 0;
fb_fix_screeninfo g_cover_fix = {};
fb_var_screeninfo g_cover_var = {};

class ComposerCallback final : public HWC2::ComposerCallback {
public:
  explicit ComposerCallback(HWC2::Device *device) : device_(device) {}

  void onHotplugReceived(int32_t, hwc2_display_t display,
                         HWC2::Connection connection) override {
    device_->onHotplug(display, connection);
  }
  void onRefreshReceived(int32_t, hwc2_display_t) override {}
  void onVsyncReceived(int32_t, hwc2_display_t, int64_t) override {}

private:
  HWC2::Device *const device_;
};

int readInteger(const char *path) {
  FILE *file = std::fopen(path, "r");
  if (!file)
    return -1;
  int value = -1;
  if (std::fscanf(file, "%d", &value) != 1)
    value = -1;
  std::fclose(file);
  return value;
}

bool writeInteger(const char *path, int value, unsigned int attempts = 1) {
  char text[16];
  const int length = std::snprintf(text, sizeof(text), "%d\n", value);
  for (unsigned int attempt = 0; attempt < attempts; ++attempt) {
    const int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
      const ssize_t written = write(fd, text, static_cast<size_t>(length));
      const int saved_errno = errno;
      close(fd);
      if (written == length)
        return true;
      errno = saved_errno;
    }
    if (attempt + 1 < attempts)
      usleep(100000);
  }
  std::fprintf(stderr, "write %s=%d failed: %s\n", path, value,
               std::strerror(errno));
  return false;
}

bool ensureComposerRunning() {
  char state[PROPERTY_VALUE_MAX] = {};
  property_get(kComposerStateProperty, state, "");
  if (std::strcmp(state, "running") == 0)
    return true;

  if (property_set("ctl.start", kComposerService) != 0) {
    std::fprintf(stderr, "failed to start %s\n", kComposerService);
    return false;
  }
  for (unsigned int attempt = 0; attempt < 50; ++attempt) {
    usleep(100000);
    property_get(kComposerStateProperty, state, "");
    if (std::strcmp(state, "running") == 0)
      return true;
  }
  std::fprintf(stderr, "%s did not reach running state\n", kComposerService);
  return false;
}

bool setPrimaryPower(HWC2::PowerMode mode) {
  if (!ensureComposerRunning())
    return false;
  android::ProcessState::self()->startThreadPool();
  HWC2::Device device("default");
  ComposerCallback callback(&device);
  device.registerCallback(&callback, 0);
  HWC2::Display *display = device.getDisplayById(HWC_DISPLAY_PRIMARY);
  if (!display || !display->isConnected()) {
    std::fprintf(stderr, "primary HWC display is unavailable\n");
    return false;
  }
  const HWC2::Error error = display->setPowerMode(mode);
  if (error != HWC2::Error::None) {
    std::fprintf(stderr, "primary HWC power mode %d failed: %d\n",
                 static_cast<int>(mode), static_cast<int>(error));
    return false;
  }
  return true;
}

void releaseCoverResources() {
  if (g_cover_mapping) {
    munmap(g_cover_mapping, g_cover_mapping_length);
    g_cover_mapping = nullptr;
    g_cover_mapping_length = 0;
  }
  if (g_cover_fd >= 0) {
    close(g_cover_fd);
    g_cover_fd = -1;
  }
  g_cover_fix = {};
  g_cover_var = {};
}

bool blankCover() {
  writeInteger(kCoverBacklight, 0);
  const bool owns_fd = g_cover_fd < 0;
  const int fd =
      owns_fd ? open(kCoverFramebuffer, O_RDWR | O_CLOEXEC) : g_cover_fd;
  if (fd < 0) {
    std::fprintf(stderr, "open %s failed: %s\n", kCoverFramebuffer,
                 std::strerror(errno));
    return false;
  }
  const bool ok = ioctl(fd, FBIOBLANK, FB_BLANK_POWERDOWN) == 0;
  if (!ok)
    std::perror("power down fb1");
  if (owns_fd) {
    close(fd);
  } else {
    releaseCoverResources();
  }
  return ok;
}

} // namespace

extern "C" int nokia2780_prepare_primary(void) {
  const int cover_brightness = readInteger(kCoverBacklight);
  if (cover_brightness > 0) {
    std::fprintf(stderr, "cover is active (backlight=%d); disabling it\n",
                 cover_brightness);
  }
  if (!blankCover())
    return -1;
  usleep(150000);
  if (!setPrimaryPower(HWC2::PowerMode::On))
    return -1;
  return 0;
}

static int showCoverRgb565(const uint16_t *source, uint32_t width,
                           uint32_t height, bool manage_primary) {
  if (!source || width != NOKIA_2780_COVER_WIDTH ||
      height != NOKIA_2780_COVER_HEIGHT) {
    std::fprintf(stderr, "cover frame must be %dx%d RGB565\n",
                 NOKIA_2780_COVER_WIDTH, NOKIA_2780_COVER_HEIGHT);
    return -1;
  }

  if (g_cover_fd < 0) {
    if (manage_primary) {
      const int primary_brightness = readInteger(kPrimaryBacklight);
      if (primary_brightness > 0) {
        std::fprintf(stderr, "primary is active (backlight=%d); disabling it\n",
                     primary_brightness);
      }
      if (!setPrimaryPower(HWC2::PowerMode::Off))
        return -1;
    }
    if (!writeInteger(kPrimaryBacklight, 0))
      return -1;
    if (manage_primary)
      usleep(200000);

    g_cover_fd = open(kCoverFramebuffer, O_RDWR | O_CLOEXEC);
    if (g_cover_fd < 0) {
      std::fprintf(stderr, "open %s failed: %s\n", kCoverFramebuffer,
                   std::strerror(errno));
      return -1;
    }
    if (ioctl(g_cover_fd, FBIOGET_FSCREENINFO, &g_cover_fix) != 0 ||
        ioctl(g_cover_fd, FBIOGET_VSCREENINFO, &g_cover_var) != 0 ||
        g_cover_var.bits_per_pixel != 16 || g_cover_var.xres != width ||
        g_cover_var.yres != height ||
        g_cover_fix.line_length < width * sizeof(uint16_t)) {
      std::perror("query fb1");
      releaseCoverResources();
      return -1;
    }

    writeInteger(kCoverBacklight, 0);
    g_cover_var.xoffset = 0;
    g_cover_var.yoffset = 0;
    g_cover_var.activate = FB_ACTIVATE_NOW;
    if (ioctl(g_cover_fd, FBIOPUT_VSCREENINFO, &g_cover_var) != 0 ||
        ioctl(g_cover_fd, FBIOBLANK, FB_BLANK_UNBLANK) != 0) {
      std::perror("enable fb1");
      releaseCoverResources();
      return -1;
    }

    // The panel initialization commands overwrite GRAM. Wait for them to
    // finish before copying the frame and requesting the first transfer.
    usleep(kCoverPanelInitUs);
    g_cover_mapping_length =
        static_cast<size_t>(g_cover_fix.line_length) * g_cover_var.yres_virtual;
    g_cover_mapping = static_cast<uint8_t *>(
        mmap(nullptr, g_cover_mapping_length, PROT_READ | PROT_WRITE,
             MAP_SHARED, g_cover_fd, 0));
    if (g_cover_mapping == MAP_FAILED) {
      g_cover_mapping = nullptr;
      std::perror("mmap fb1");
      releaseCoverResources();
      return -1;
    }
  }

  for (uint32_t page_y = 0; page_y < g_cover_var.yres_virtual;
       page_y += g_cover_var.yres) {
    const uint32_t rows =
        std::min(g_cover_var.yres, g_cover_var.yres_virtual - page_y);
    for (uint32_t y = 0; y < rows; ++y) {
      std::memcpy(g_cover_mapping +
                      static_cast<size_t>(page_y + y) * g_cover_fix.line_length,
                  source + static_cast<size_t>(y) * width,
                  static_cast<size_t>(width) * sizeof(uint16_t));
    }
  }
  msync(g_cover_mapping, g_cover_mapping_length, MS_SYNC);
  g_cover_var.activate = FB_ACTIVATE_VBL;
  const bool presented =
      ioctl(g_cover_fd, FBIOPUT_VSCREENINFO, &g_cover_var) == 0;
  if (!presented)
    std::perror("present fb1");
  if (!presented) {
    releaseCoverResources();
    return -1;
  }

  usleep(kCoverTransferSettleUs);
  if (!writeInteger(kCoverBacklight, 255, 10))
    return -1;
  std::fprintf(stderr, "cover frame presented: %ux%u virtual=%ux%u stride=%u\n",
               g_cover_var.xres, g_cover_var.yres, g_cover_var.xres_virtual,
               g_cover_var.yres_virtual, g_cover_fix.line_length);
  return 0;
}

extern "C" int nokia2780_show_cover_rgb565(const uint16_t *source,
                                           uint32_t width, uint32_t height) {
  return showCoverRgb565(source, width, height, true);
}

extern "C" int
nokia2780_show_cover_rgb565_after_primary_off(const uint16_t *source,
                                              uint32_t width, uint32_t height) {
  return showCoverRgb565(source, width, height, false);
}

extern "C" int nokia2780_hide_cover(void) { return blankCover() ? 0 : -1; }
