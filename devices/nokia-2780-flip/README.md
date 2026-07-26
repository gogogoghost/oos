# Nokia 2780 Flip

This directory contains the validated Nokia 2780 Flip platform implementation
for Orange OS and its on-device tests. The target runs KaiOS 3 on Android API
29 and uses the stock system and vendor graphics services.

## Production Libraries

`oos_nokia_2780_display` provides panel ownership and switching primitives:

- `nokia2780_prepare_primary()` prepares the HWC primary path and disables
  fb1.
- `nokia2780_show_cover_rgb565()` performs a standalone primary-to-cover
  transition and retains fb1 for the caller's lifetime.
- `nokia2780_show_cover_rgb565_after_primary_off()` and
  `nokia2780_hide_cover()` support a long-lived process that owns its HWC
  primary client.

`oos_nokia_2780_wpe_display` provides
`oos::nokia2780::WpeDisplayManager`. It owns the HWC2 client, RGB565 gralloc
target, EGL/GLES blit path, panel backlights, retained WPE frame, and fast
primary/cover switching. The public header uses a PIMPL boundary so HWC2,
GraphicBuffer, and EGL implementation types do not leak into application code.

The production `oos` entry point is currently empty and does not link these
libraries yet. They are ready for use when the application lifecycle is
defined.

## Validated Display Paths

The primary display is 240x320 RGB565. WPE WebKit supplies Android hardware
buffers, GLES copies them into a GPU/HWC-compatible RGB565 client target, and
HWC2 presents the result. A black preroll and a settled first submission keep
uninitialized panel memory hidden.

The cover display is fb1 with 128x160 visible RGB565 pixels and a 128x320
virtual framebuffer. The driver requires fb1 to remain open while the image is
visible. Panel initialization and transfer are serialized before the cover
backlight is enabled.

The validated single-process switch times are approximately:

- primary to cover: 622-637ms;
- cover to primary: 439ms.

These measurements exclude WebKit startup, HWC service startup, chroot mounts,
and ADB transfers.

## Hardware Constraint

The stock configuration exposes the cover as
`ro.kaios.display.ext_fb_dev=fb1`. Gecko's `NativeFramebufferDevice` copies a
producer buffer into fb1; it is not an HWC external display or a GPU zero-copy
scanout path.

Keeping fb0 and fb1 active together causes the primary panel ESD check to read
an invalid status and reset the primary panel. The public display manager
therefore enforces mutual exclusion. True simultaneous output requires kernel
or vendor HWC changes that expose the cover as a supported external display.

## Tests

All executable test entry points and fixtures live under `tests`:

- `oos_test_nokia_2780_wpe_display`: WPE primary smoke test and the
  single-process 3+3+3 second switch test.
- `oos_test_nokia_2780_primary_gl`: EGL/HWC animation test.
- `oos_test_nokia_2780_cover_secondary`: labeled cover fixture.
- `oos_test_nokia_2780_cover_green`: solid RGB565 cover fixture.
- `oos_test_nokia_2780_primary_bufferqueue`: exploratory BufferQueue path.

Run the validated fast switch test with:

```sh
./scripts/test-single-process-switch.sh
```

The device launcher creates a temporary chroot and overlays only its view of
`libc++_shared.so`; it never modifies the device's system partition.
