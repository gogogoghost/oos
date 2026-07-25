# KaiOS Display POC

This repository contains independent display-path POCs for KaiOS devices. It
does not start B2G. The Nokia 2780 Flip implementation currently proves:

- main-panel GPU rendering through explicit RGB565 gralloc buffers, EGLImage,
  and HWC client target submission;
- main-panel backlight and power sequencing;
- cover-panel RGB565 framebuffer and `sublcd-backlight` control.

## Layout

- `devices/nokia-2780-flip`: validated 2780-specific POCs and experiments.
- `devices/nokia-8110-4g`: reserved device slot; it has no implementation yet.
- `third_party`: local AOSP, Gecko, and KaiOS source checkouts, excluded from
  the main Git repository.
- `tools/kaios-hidl-gen`: host build wrapper for the KaiOS HIDL generator.
- `patches`: compatibility changes applied to clean third-party checkouts.

## Build Nokia 2780 Flip

Set the extracted Android `/system` location in `.env`. The current local
configuration is:

```sh
SYSTEM_DIR=/home/jax/tmp/system-stock-2780/system
```

Then configure and build:

```sh
./scripts/configure-android.sh nokia-2780-flip
cmake --build build/android-nokia-2780-flip -j2
```

Outputs are placed in `build/android-nokia-2780-flip/bin/nokia-2780-flip/`.
The configuration script generates Android 10 HIDL headers and links directly
against `$SYSTEM_DIR/lib`; no copied system libraries are used.
