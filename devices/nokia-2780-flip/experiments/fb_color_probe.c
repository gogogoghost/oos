#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int fill_fb(const char *path, unsigned short rgb565) {
  int fd = open(path, O_RDWR);
  struct fb_fix_screeninfo fix;
  struct fb_var_screeninfo var;
  if (fd < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) ||
      ioctl(fd, FBIOGET_VSCREENINFO, &var)) {
    perror(path);
    return 1;
  }
  size_t length = fix.line_length * var.yres_virtual;
  unsigned short *pixels = mmap(0, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels == MAP_FAILED) {
    perror("mmap");
    close(fd);
    return 1;
  }
  for (size_t i = 0; i < length / sizeof(*pixels); ++i) pixels[i] = rgb565;
  var.xoffset = 0;
  var.yoffset = 0;
  var.activate = FB_ACTIVATE_NOW;
  if (ioctl(fd, FBIOPUT_VSCREENINFO, &var)) perror("FBIOPUT_VSCREENINFO");
  fprintf(stderr, "%s: %ux%u virtual=%ux%u stride=%u bpp=%u\n", path,
          var.xres, var.yres, var.xres_virtual, var.yres_virtual,
          fix.line_length, var.bits_per_pixel);
  msync(pixels, length, MS_SYNC);
  munmap(pixels, length);
  close(fd);
  return 0;
}

int main(void) {
  int result = fill_fb("/dev/graphics/fb0", 0x07e0);  // green
  result |= fill_fb("/dev/graphics/fb1", 0x001f);     // blue
  return result;
}
