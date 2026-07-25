# KaiOS Display POC

This repository contains independent display-path POCs for KaiOS devices. It
does not start B2G. The Nokia 2780 Flip implementation currently proves:

- main-panel GPU rendering through explicit RGB565 gralloc buffers, EGLImage,
  and HWC client target submission;
- main-panel backlight and power sequencing;
- cover-panel RGB565 framebuffer and `sublcd-backlight` control when the main
  panel is inactive.

See [the Nokia 2780 device notes](devices/nokia-2780-flip/README.md) for the
validated runtime, display ownership rules, and the current dual-panel limit.

## Layout

- `devices/nokia-2780-flip`: validated 2780-specific POCs and experiments.
- `devices/nokia-8110-4g`: reserved device slot; it has no implementation yet.
- `third_party`: local AOSP, Gecko, and KaiOS source checkouts, excluded from
  the main Git repository.
- `tools/kaios-hidl-gen`: host build wrapper for the KaiOS HIDL generator.
- `patches`: compatibility changes applied to clean third-party checkouts.

## Build WPE Runtime

The Nokia 2780 runtime is an Android API 29 ARMv7 build. It uses WPEPlatform
and Android `AHardwareBuffer`, while deliberately excluding the legacy libwpe,
Wayland, DRM/GBM, multimedia, WebRTC, Inspector, and WebDriver stacks.

Set `WPE_DISTROBOX=oos-debian12` in `.env` to run WPE/Cerbero compilation in
the Debian 12 / Python 3.11 Distrobox. When it is unset, the script compiles
in the current environment instead. Run:

```sh
./scripts/build-wpe-sysroot.sh
```

This invokes the checked-out WPE Android Cerbero recipes inside the container
with the tracked API-29 override, applies
`patches/wpe-android-cerbero-kaios-minimal.patch`, and writes target
libraries, headers, `WPEWebProcess`, and
`WPENetworkProcess` under `build/wpe-sysroot/nokia-2780-flip/`.

The WebKit source revision is set by `WPEWEBKIT_COMMIT` in `.env` when needed;
the default is the checked-out WPE WebKit 2.52.5 release commit.

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

## Run WPE In A Chroot

The WPE runtime needs a newer `libc++_shared.so` than the stock system copy.
`./scripts/run-wpe-chroot.sh start` creates a temporary Android chroot on the
device, bind-mounts the live system, vendor, Runtime APEX, binder/device nodes,
and pseudo-filesystems, then overlays only the chroot view of
`/system/lib/libc++_shared.so` with the matching runtime from `WPE_NDK`. It
never writes to the device's system partition. The script expects the current
WPE runtime to already be present at `/data/local/tmp/oos-wpe/`; use `status`
for the process and log, or `stop` to terminate it and release every mount.

The default run mode is the validated primary-screen WPE sample. Do not set
`OOS_ENABLE_COVER=1` for normal use: the Nokia 2780 vendor driver does not
support fb0 and fb1 remaining active together. That switch exists only for
diagnostic work described in the device notes.
