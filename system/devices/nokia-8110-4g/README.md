# Nokia 8110 4G

The Nokia 8110 4G port targets the stock KaiOS 2.5 / Android 6.0.1 base:

- Qualcomm MSM8909 family, ARMv7, Android API 23
- one 240x320 command-mode SPI panel
- RGB565 gralloc0 buffers and Hardware Composer 1
- Adreno EGL/OpenGL ES 3.0 userspace driver

Configure it with an extracted stock system tree:

```sh
NOKIA_8110_SYSTEM_DIR=/home/jax/tmp/system-stock-8110-4g/system
./scripts/configure-android.sh nokia-8110-4g
cmake --build build/android-nokia-8110-4g -j24 --target oos
```

The investigated handset runs build `17.00.17.01`; the available Gerda image
is build `13.00.17.01`. OOS compiles against the extracted image and loads the
handset's stock libraries at runtime. The validated HAL/EGL subset remains
small enough that unstable bulk ADB extraction is unnecessary.

The native compositor allocates gralloc0 RGB565 targets and presents them with
HWC1. A compatibility library supplies the API 26-style AHardwareBuffer symbols
missing from Android 6 while using its existing gralloc and ashmem primitives.
This is part of the native display backend, not a browser transport.

## Validated Backends

- Primary display: EGL/GLES2 rendering, HWC1 client composition, explicit
  panel power sequencing, and 240x320 Launcher output.
- Input: matrix keypad, slider hall sensors, power/volume GPIO inputs, and
  headset events through the shared evdev multiplexer.
- Audio: OpenSL ES playback and 16 kHz microphone capture with an OOS-owned
  permission Binder service after B2G exits.
- Camera: one HAL1 back camera, off-screen JPEG capture up to 1600x1200, and
  torch control.
- Power: battery/USB state, uevents, wake locks, Power HAL interactive state,
  and slider state.
- Network: Wi-Fi, DHCP/static IPv4, classic Bluetooth discovery, and BLE
  discovery through the stock daemon protocol.
- Modem: root-owned `/dev/socket/rild` Parcel protocol for state, baseband,
  registration, signal, call/data-call, and radio capability.

Hardware video codecs, GPS, sensors, and FM remain planned. The 8110 has no
secondary display and no validated NFC.

Build the read-only contracts with:

```sh
cmake --build build/android-nokia-8110-4g -j24 --target \
  oos_test_nokia_8110_device_contract \
  oos_test_nokia_8110_service_contract
```

Low-level graphics, camera, and RIL probes remain for investigation. New OOS
code should consume the common platform, hardware, network, and modem APIs.
