# Nokia 2780 Flip

This directory contains device-specific experiments and the current WPE
runtime POC for the Nokia 2780 Flip running KaiOS 3 / Android API 29.

## Validated Main Display Path

`nokia_2780_wpe_hello` is the current replacement-process smoke test. It
renders the static HTML fixture with WPE WebKit, receives RGB565
`AHardwareBuffer` frames from the WPE Android backend, and submits them as the
primary HWC client target. The primary panel is 240x320 RGB565.

The device launcher is `scripts/run-wpe-chroot.sh`. It resets display state,
restarts the vendor composer, adjusts the primary backlight, and launches the
program inside an Android chroot. The chroot overlays only its own view of
`libc++_shared.so`; it does not modify `/system`.

Use the following commands after building the WPE sysroot and Android target:

```sh
./scripts/run-wpe-chroot.sh start
./scripts/run-wpe-chroot.sh status
./scripts/run-wpe-chroot.sh stop
```

The default is primary-only. A reset waits for the vendor composer to return
to `running` before the POC starts.

## Cover Display

The cover panel is `fb1`, 128x160 visible pixels, RGB565, with a 128x320
virtual framebuffer. `cover_green_poc.c` and the optional cover path in
`wpe_hello_poc.cpp` establish its direct framebuffer/backlight controls.

The stock display configuration sets `ro.kaios.display.ext_fb_dev=fb1`.
KaiOS Gecko handles that property with `NativeFramebufferDevice`: it locks a
producer buffer for CPU reading, copies it into `/dev/graphics/fb1`, and uses
`FBIOPUT_VSCREENINFO` to present it. RGB8888 producer buffers require a CPU
conversion to RGB565. Thus the stock `fb1` path is not a GPU zero-copy HWC
output.

## Dual-Panel Limit

The vendor HWC library contains a generic `HWCDisplayExternal` implementation,
but the Nokia 2780 `fb1` path does not use it. It follows the direct Gecko
framebuffer path above. When `fb0` and `fb1` are both active, the primary SPI
panel's ESD check reads an invalid status value and resets the primary panel.
This reproduces even when `fb1` is written only once and then left idle.

For that reason `OOS_ENABLE_COVER` defaults to `0`. Setting it to `1` is an
unsupported diagnostic experiment, not a dual-display mode. A production
dual-panel implementation requires kernel/HWC work that exposes `fb1` as a
real HWC external display; it cannot be supplied solely by a B2G replacement
process.

## Source Layout

- `src/wpe_hello_poc.cpp`: primary WPE WebKit smoke test and diagnostic fb1
  helper.
- `src/primary_gl_animation_poc.cpp`: independent EGL/HWC primary animation
  test.
- `src/cover_green_poc.c`: minimal direct fb1 RGB565 test.
- `experiments/`: investigation programs, including the stock Gonk window
  path and BufferQueue probes.
- `scripts/wpe_chroot_device.sh`: device-side chroot lifecycle and display
  reset operations.
- `assets/hello.html`: static WPE fixture.
