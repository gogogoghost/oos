# Nokia 8110 4G

The Nokia 8110 4G port targets its stock KaiOS 2.5 / Android 6.0.1 base:

- Qualcomm MSM8909 family, ARMv7, Android API 23
- one 240x320 command-mode SPI panel
- RGB565 framebuffer with a 240x640 double-buffer virtual surface
- gralloc0 and Hardware Composer 1, rather than the 2780 Flip's HWC2/HIDL stack
- Adreno EGL/OpenGL ES 3.0 userspace driver

Configure it with an extracted stock `/system` tree:

```sh
NOKIA_8110_SYSTEM_DIR=/home/jax/tmp/system-stock-8110-4g/system
scripts/configure-android.sh nokia-8110-4g
cmake --build build/android-nokia-8110-4g \
  --target oos -j"$(nproc)"
```

The first investigated handset runs build `17.00.17.01`. The available Gerda
image is build `13.00.17.01`; it is suitable as a base, but its Android
framework and display HAL files are not byte-identical. OOS links against the
Gerda image and loads the handset's own libraries at runtime. The graphics
probe built from the Gerda sysroot passes on the newer handset, validating the
C HAL and EGL ABI subset used by the initial port without relying on unstable
bulk ADB extraction.

The production runtime is enabled through the same `oos::device::Device` and
`oos::device::Display` interfaces as the Nokia 2780. The shared OOS runtime has
no Nokia-specific include or control flow.

WPE uses the independent `android23-armv7-jit` sysroot and a gralloc0-backed
AHardwareBuffer compatibility library. WPE only produces GPU buffers;
its isolated producer sends them over the common Unix surface transport. The
OOS host imports them into the existing RGB565 HWC1 target. WPE and the host
use separate dynamic-library paths so Android 6 system C++ and WPE libraries
cannot collide. Build and run its shared smoke test with:

```sh
./scripts/build-wpe-sysroot.sh nokia-8110-4g
./scripts/configure-android.sh nokia-8110-4g
cmake --build build/android-nokia-8110-4g --target \
  oos_test_nokia_8110_4g_wpe_producer \
  oos_test_nokia_8110_4g_wpe_host -j8
./scripts/run-wpe-chroot.sh nokia-8110-4g start
```

## Validated Backends

- Primary display: gralloc0 RGB565 target, EGL/GLES2 rendering, HWC1 client
  composition, explicit panel power sequencing, and 240x320 launcher output.
- Input: matrix keypad, slider hall sensors, power/volume GPIO inputs, and
  headset events through the shared evdev multiplexer.
- Audio: OpenSL ES playback and 16 kHz microphone capture, with an OOS-owned
  permission Binder service after B2G exits.
- Camera: one back-facing HAL1 camera, off-screen JPEG capture up to 1600x1200,
  and torch sysfs control.
- Power: battery/USB state, uevents, wake locks, Power HAL interactive state,
  and slider state. RTC wake and deep suspend are implemented but await final
  unplugged lifecycle validation.
- Network: Wi-Fi scan/association lifecycle, DHCP/static IPv4, classic
  Bluetooth discovery, and BLE discovery using the older KaiOS b2g48 GATT
  protocol.
- Modem: root-owned `/dev/socket/rild` Parcel protocol, no-SIM state, baseband,
  registration, signal, call/data-call, and radio-capability snapshots.

Hardware video codecs, GPS, sensors, and FM remain `planned`; they are not
reported as usable. The 8110 has no secondary display and no validated NFC.

The read-only contract tests are:

```sh
cmake --build build/android-nokia-8110-4g -j8 --target \
  oos_test_nokia_8110_device_contract \
  oos_test_nokia_8110_service_contract
```

Low-level graphics, camera, and RIL probes remain for investigation, but new
OOS code should link the common `oos::platform`, `oos::hardware`,
`oos::network`, and `oos::modem` targets.
