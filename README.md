# Orange OS

Orange OS (`oos`) is a native C++ system shell intended to replace B2G on
supported KaiOS phones. OOS owns display composition, input, lifecycle, device
services, and application storage. Applications are ZIP packages containing a
WAMR WebAssembly module, normally an ARMv7 AOT image plus a portable Wasm
fallback.

This branch is deliberately browser-free. It contains no WPE WebKit runtime,
JavaScript engine, HTML launcher, or KaiOS Web application compatibility layer.
The production Launcher is a process-local C++/LVGL application. SystemUI is a
separate process-local application that owns the status and overlay surfaces.
Dear ImGui has a native GPU-mesh backend for diagnostics and system tools.
Rust/egui remains the first WIT framework adapter and regression application
for third-party Wasm apps.
The phone shell uses `#E65100` as its single interaction accent and obtains
battery, charging, Wi-Fi, signal-strength, roaming, and radio-technology state
from a non-blocking device status monitor.

## Repository Layout

- `apps/launcher`: the production LVGL Launcher application and its assets.
- `apps/systemui`: status bar, notifications, lock screen, and global overlays.
- `apps/tests`: framework integration and WIT regression applications.
- `sdk`: versioned WIT interfaces and reusable egui/LVGL/ImGui backends.
- `system/src/oos`: native runtime, compositor, application registry, storage,
  services, and device-independent hardware contracts.
- `system/devices/nokia-2780-flip`: Android 10/HWC2 device implementation.
- `system/devices/nokia-8110-4g`: Android 6/HWC1 device implementation.
- `system/devices/local`: SDL/llvmpipe development device.
- `system/packaging/scaffold`: device-side mount and lifecycle scripts.
- `scripts`: build, package, deployment, and hardware test entry points.
- `third_party`: ignored source checkouts plus committed source pins.

Firmware reference trees under `third_party` document proprietary device
protocols. They are not application runtimes and are not linked into OOS.

## Build Environment

Copy `.env.example` to `.env` and configure both extracted stock systems:

```sh
NOKIA_2780_SYSTEM_DIR=/home/jax/tmp/system-stock-2780/system
NOKIA_8110_SYSTEM_DIR=/home/jax/tmp/system-stock-8110-4g/system
ANDROID_NDK=/home/jax/Android/Sdk/ndk/r21e
```

Each Android device has a separate CMake build directory and consumes only its
matching stock libraries. WAMR builds locally by default. Set
`OOS_WAMR_DISTROBOX=oos-debian12` only when the host lacks the compatible LLVM
toolchain used by `wamrc`.

Build the native guest and both device targets:

```sh
./scripts/fetch-wamr.sh
./scripts/fetch-ui-frameworks.sh
make system DEVICE=nokia-2780-flip
make system DEVICE=nokia-8110-4g
```

Useful outputs are:

- `build/native-apps/egui-demo.wasm`: portable egui SDK test module.
- `build/native-apps/egui-demo.aot`: optimized ARMv7 WAMR AOT test module.
- `build/android-<device>/bin/oos`: device-specific system process.
- `build/android-<device>/bin/tests/<device>/`: device tests, when enabled.

Build and run the local device in its isolated rootfs with:

```sh
make run-local
```

The local target uses a synthetic `/dev` and software rendering, so it does not
open physical host device nodes.

## Validation

Run the native runtime and repository tests with:

```sh
make test-wasm
```

Configure production builds without device diagnostics using:

```sh
make system DEVICE=nokia-2780-flip \
  CMAKE_ARGS=-DOOS_BUILD_DEVICE_TESTS=OFF
```

The hardware test runners remain device-specific:

```sh
./scripts/test-device-contract.sh nokia-2780-flip
./scripts/test-network.sh smoke
./scripts/test-modem.sh smoke
./scripts/test-hardware.sh smoke
```

See [docs/device-platform.md](docs/device-platform.md),
[docs/graphics.md](docs/graphics.md), [docs/input.md](docs/input.md),
[docs/system-ui.md](docs/system-ui.md), [docs/ui-icons.md](docs/ui-icons.md),
and the device READMEs for the common
hardware API, system icon policy, and verified platform behavior.

## Packaging

Deployment separates a stable scaffold from an atomic, versioned runtime
resource directory:

```sh
./scripts/package-oos-scaffold.sh \
  --device nokia-2780-flip --res-version 1.0.0 --tgz
./scripts/package-oos-res.sh 1.0.0 \
  --device nokia-2780-flip --activate --tgz
```

The resource package contains `oos`, shared application fonts, and licenses.
The LVGL SystemUI and boot splash are compiled into `oos`; they are not
deployed as separate files. The package does not contain a browser engine or
Web helper processes.
See [docs/deployment.md](docs/deployment.md) for the mount and upgrade contract,
[docs/applications.md](docs/applications.md) for the ZIP/registry format, and
[docs/wasm-runtime.md](docs/wasm-runtime.md) for the WIT/WAMR architecture.
