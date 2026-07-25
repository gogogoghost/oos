#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int write_value(const char* path, int value) {
  char text[16];
  const int length = snprintf(text, sizeof(text), "%d\n", value);
  const int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0 || length <= 0 || write(fd, text, (size_t)length) != length) {
    perror(path);
    if (fd >= 0) close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

int main(void) {
  const char* const path = "/dev/graphics/fb1";
  const char* const backlight =
      "/sys/devices/platform/soc/soc:qcom,mdss_spi_ext_panel/"
      "soc:qcom,mdss_spi_ext_panel:qcom,mdss_fb1_primary/leds/"
      "sublcd-backlight/brightness";
  const int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    perror(path);
    return 1;
  }

  struct fb_fix_screeninfo fix = {};
  struct fb_var_screeninfo var = {};
  if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0 ||
      ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0) {
    perror("fb1 query");
    close(fd);
    return 1;
  }
  if (var.bits_per_pixel != 16) {
    fprintf(stderr, "fb1 has unsupported bpp=%u\n", var.bits_per_pixel);
    close(fd);
    return 1;
  }

  const size_t length = (size_t)fix.line_length * var.yres_virtual;
  unsigned short* const pixels =
      mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels == MAP_FAILED) {
    perror("fb1 mmap");
    close(fd);
    return 1;
  }

  var.xoffset = 0;
  var.yoffset = 0;
  var.activate = FB_ACTIVATE_NOW;
  if (ioctl(fd, FBIOPUT_VSCREENINFO, &var) != 0 ||
      ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK) != 0) {
    perror("fb1 enable");
    munmap(pixels, length);
    close(fd);
    return 1;
  }

  for (size_t i = 0; i < length / sizeof(*pixels); ++i) {
    pixels[i] = 0x07e0;  // RGB565 green
  }
  msync(pixels, length, MS_SYNC);
  var.activate = FB_ACTIVATE_VBL;
  if (ioctl(fd, FBIOPUT_VSCREENINFO, &var) != 0) perror("fb1 present");
  if (write_value(backlight, 255) != 0) {
    munmap(pixels, length);
    close(fd);
    return 1;
  }
  fprintf(stderr, "fb1 green: %ux%u virtual=%ux%u stride=%u\n", var.xres,
          var.yres, var.xres_virtual, var.yres_virtual, fix.line_length);

  sleep(10);
  write_value(backlight, 0);
  ioctl(fd, FBIOBLANK, FB_BLANK_POWERDOWN);
  munmap(pixels, length);
  close(fd);
  return 0;
}
