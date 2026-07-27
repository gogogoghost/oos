# Orange OS

Orange OS (`oos`) is a native C++ system shell intended to replace B2G on
KaiOS devices. The production executable presents a native boot frame, starts
the WAMR native-app runtime, loads an egui Launcher compiled to WebAssembly,
and forwards physical keys through the capability-limited OOS ABI. WPE WebKit
remains packaged for running compatible KaiOS applications; it is no longer
the system Launcher runtime.

## Repository Layout

- `app/main.cpp` and `app/runtime.cpp`: device-independent production entry
  point and launcher lifecycle.
- `apps/launcher`: production egui Launcher compiled to WebAssembly.
- `sdk/rust/oos-app`: low-level Rust bindings for the OOS native-app ABI.
- `sdk/rust/oos-egui`: reusable egui texture and mesh adapter.
- `launcher`: retained Solid.js/WPE prototype for KaiOS web-app work.
- `runtime/wamr`: pinned WAMR runtime configuration.
- `core`: device-independent runtime modules. `oos_input` provides reusable
  Linux evdev key capture for the production event loop. The native app
  manager keeps up to three isolated WASM apps resident while only the active
  app receives input and frame callbacks. The network
  interfaces define reusable Wi-Fi, IP, and Bluetooth lifecycle APIs. The
  modem interface exposes Radio HAL state without leaking HIDL types. Hardware
  interfaces cover audio, vibration, power/flip lifecycle, camera/flash, and
  hardware video codecs.
- `core/include/oos/device`: standard device, display, capability, and service
  initialization contracts used by OOS.
- `devices/nokia-2780-flip`: Android 10/HWC2 platform implementation.
- `devices/nokia-2780-flip/tests`: on-device smoke, lifecycle, animation, and
  investigation test programs. Test-only fixtures live under `tests/support`.
- `devices/nokia-8110-4g`: Android 6/HWC1 platform implementation.
- `tests/device`: shared device/service contracts plus network and modem tests
  compiled against both backends.
- `scripts`: build, deployment, and test runners.
- `packaging/scaffold`: stable device-side bootstrap script templates.
- `third_party`: local AOSP, Gecko, KaiOS, WAMR, and WPE checkouts, excluded
  from the main Git repository. Their pinned revisions are tracked separately.
- `patches`: tracked compatibility changes for clean third-party checkouts.

Production code must not depend on files under a device's `tests` directory.
Validated functionality moves into a public device library before it is used
by `app/main.cpp`.

See [docs/device-platform.md](docs/device-platform.md) for the standard API,
capability matrix, service initialization helpers, and new-device checklist.
Run `./scripts/test-device-contract.sh DEVICE` for the common non-mutating
device and Manager link contracts.

## Build Environment

Keep the extracted Android system paths separate in `.env`:

```sh
NOKIA_2780_SYSTEM_DIR=/home/jax/tmp/system-stock-2780/system
NOKIA_8110_SYSTEM_DIR=/home/jax/tmp/system-stock-8110-4g/system
```

`configure-android.sh` selects the matching path from its device argument, so
configuring one target cannot silently replace the other target's sysroot.

Set `WPE_DISTROBOX=oos-debian12` to build WPE/Cerbero inside the Debian 12
Distrobox. Leave it unset to build in the current environment.
`WPE_BUILD_JOBS` optionally caps parallel compilation; it defaults to all
available CPU threads.

WAMR builds locally by default. Set `OOS_WAMR_DISTROBOX=oos-debian12` when a
compatible LLVM is provided by that container. WAMR 2.4.4 is currently built
with LLVM 14. This setting is intentionally independent of `WPE_DISTROBOX`.

Build each WPE runtime profile, configure the corresponding Android target,
and compile Orange OS:

```sh
./scripts/build-wpe-sysroot.sh nokia-2780-flip
./scripts/build-wpe-sysroot.sh nokia-8110-4g
./scripts/fetch-wamr.sh
./scripts/build-native-app-aot.sh
./scripts/configure-android.sh nokia-2780-flip
cmake --build build/android-nokia-2780-flip -j8

./scripts/configure-android.sh nokia-8110-4g
cmake --build build/android-nokia-8110-4g -j8
```

The outputs are:

- `build/android-nokia-2780-flip/bin/oos`: production executable.
- `build/native-apps/launcher.wasm`: portable interpreter/debug Launcher.
- `build/native-apps/launcher.aot`: production ARMv7 AOT Launcher.
- `build/android-nokia-2780-flip/bin/tests/nokia-2780-flip/`: on-device tests.
- `build/android-nokia-8110-4g/bin/oos`: Nokia 8110 production executable.
- `build/wpe-sysroot/nokia-2780-flip`: isolated Android 10 WPE sysroot.
- `build/wpe-sysroot/nokia-8110-4g`: isolated Android 6 WPE sysroot.

The KaiOS performance profile deliberately retains JavaScriptCore Baseline and
DFG JIT plus WebAssembly BBQ JIT. FTL/OMG remain disabled because WebKit does
not support those optimizing tiers on this 32-bit ARM target. The build script
checks the configured features and ARMv7 JIT objects before reporting success.
See [docs/wpe-runtime-profile.md](docs/wpe-runtime-profile.md) for the retained
runtime surface, disabled feature groups, and ARMv7 softfp JIT boundary.

For a production-only build, configure with:

```sh
./scripts/configure-android.sh nokia-2780-flip \
  -DOOS_BUILD_DEVICE_TESTS=OFF
```

## Display Tests

WPE never owns a display connection. Its isolated producer process transfers
GPU buffers to the OOS host over the common surface transport. Native/WASM
applications and received WPE surfaces are composed only by the OOS host;
only the selected device backend can access HWC, panel power, and backlights.
Run the shared WPE compositor smoke test with:

```sh
./scripts/run-wpe-chroot.sh nokia-2780-flip start
./scripts/run-wpe-chroot.sh nokia-8110-4g start
```

Other repeatable tests are:

```sh
./scripts/test-primary-lifecycle.sh 3
./scripts/test-display-lifecycle.sh 3
./scripts/run-cover-test.sh secondary
./scripts/test-key-input.sh
./scripts/run-wpe-chroot.sh nokia-2780-flip stop
./scripts/run-wpe-chroot.sh nokia-8110-4g stop
```

See `devices/nokia-2780-flip/README.md` for the hardware constraints, public
API ownership rules, and individual test targets.

See [docs/input.md](docs/input.md) for the shared key input API, the detected
Nokia 2780 input topology, exclusive-grab policy, and visual test workflow.

## Network Tests

Build, deploy, and run the recovery-safe Nokia 2780 Wi-Fi and Bluetooth smoke
test with:

```sh
./scripts/test-network.sh smoke
```

See [docs/network.md](docs/network.md) for the implemented headless APIs,
validated device results, commands requiring a known hotspot or Bluetooth
peer, and the remaining daily-use connectivity surface.

## Modem Tests

Build, deploy, and run the read-only Nokia 2780 Radio HAL smoke test with:

```sh
./scripts/test-modem.sh smoke
```

See [docs/modem.md](docs/modem.md) for the reusable modem API, validated no-SIM
results, privacy and radio-power constraints, and the SIM-dependent call, SMS,
and packet-data work that remains.

## Hardware Tests

Build, deploy, and run the Nokia 2780 audio, vibration, power, camera, flash,
and Qualcomm H.264 codec smoke tests with:

```sh
./scripts/test-hardware.sh smoke
```

See [docs/hardware.md](docs/hardware.md) for the reusable native APIs, verified
device results, Camera2 firmware limitation, USB suspend boundary, and future
Web System UI integration contract.

## Runtime Packaging

OOS deployment separates the stable chroot bootstrap from versioned `res`
runtime packages. Here `res` contains the native `oos` executable, WPE WebKit,
its shared libraries, and helper processes. Generate both directory and tgz
outputs with:

```sh
./scripts/package-oos-scaffold.sh --res-version 1.0.0 --tgz
./scripts/package-oos-res.sh 1.0.0 --activate --tgz
```

See [docs/deployment.md](docs/deployment.md) for the production
`/system/oos`, trial `/data/local/tmp/oos`, persistent `/data/oos`, upgrade,
mount, and lifecycle contract.

See [docs/wasm-runtime.md](docs/wasm-runtime.md) for the native-app lifecycle,
graphics ABI, WAMR isolation model, egui adapter, and iced/LVGL integration
plan.
