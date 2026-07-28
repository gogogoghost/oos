# Orange OS

Orange OS (`oos`) is a native C++ system shell intended to replace B2G on
KaiOS devices. The production executable presents a native boot frame, starts
the WAMR native-app runtime, loads an egui Launcher compiled to WebAssembly,
and forwards physical keys through the capability-limited OOS WIT interfaces.
WPE WebKit remains packaged for running compatible KaiOS applications; it is
no longer the system Launcher runtime.

## Repository Layout

- `apps`: self-contained application workspace. It owns the Rust workspace,
  production WIT/egui launcher, legacy Web launcher, SDK, WIT package, and
  guest tests.
- `system`: self-contained CMake project for the native OOS process. Generic
  code is co-located by domain under `system/src/oos`; device backends,
  runtime integration, system assets/configuration, patches, packaging, and
  native tests live alongside it.
- `system/src/main.cpp` and `system/src/runtime.cpp`: production process entry
  point and event loop.
- `system/devices/nokia-2780-flip`: Android 10/HWC2 platform implementation.
- `system/devices/nokia-8110-4g`: Android 6/HWC1 platform implementation.
- `system/devices/local`: 240x320 SDL/llvmpipe test device with deterministic
  hardware mocks and configurable host-key mapping.
- `apps/sdk/wit/oos.wit`: versioned application interface source of truth.
- `system/src/oos/apps`: ZIP manifests, the SQLite registry, and WAMR/WPE
  launch-context preparation.
- `system/src/oos/storage`: atomic filesystem helpers plus per-app KV and
  SQLite storage.
- `apps/sdk/rust`: generated Rust bindings and reusable framework adapters.
- `apps/web-launcher`: retained Solid.js/WPE prototype for KaiOS web-app work.
- `Makefile`: repository-level entry points; language manifests stay inside
  the package they build.
- `scripts`: build, deployment, and test runners.
- `third_party`: local AOSP, Gecko, KaiOS, WAMR, and WPE checkouts, excluded
  from the main Git repository. Their pinned revisions are tracked separately.
- `tools`: independently buildable host utilities.

Production code must not depend on files under a device's `tests` directory.
Validated functionality moves into a public device library before it is used
by `system/src/main.cpp`.

See [docs/device-platform.md](docs/device-platform.md) for the standard API,
capability matrix, service initialization helpers, and new-device checklist.
See [docs/graphics.md](docs/graphics.md) for the portable GUI/canvas path,
batched GLES2 engine path, RGB565 policy, and host-composition boundary.
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
`WPE_BUILD_JOBS` optionally caps parallel compilation. Android uses its
device-profile default (or the available CPU count); the local build defaults
to at most six jobs because WebCore unified sources have high peak memory use.

WAMR builds locally by default. Set `OOS_WAMR_DISTROBOX=oos-debian12` when a
compatible LLVM is provided by that container. WAMR 2.4.4 is currently built
with LLVM 14. This setting is intentionally independent of `WPE_DISTROBOX`.

Build each WPE runtime profile, configure the corresponding Android target,
and compile Orange OS:

```sh
./scripts/build-wpe-sysroot.sh nokia-2780-flip
./scripts/build-wpe-sysroot.sh nokia-8110-4g
./scripts/fetch-wamr.sh
make native-app-aot
make system DEVICE=nokia-2780-flip
make system DEVICE=nokia-8110-4g
```

The WPE source archives and Android Cerbero checkout are pinned in
`third_party/versions.env`. `make fetch-wpe` downloads and verifies them.
Every platform consumes `system/config/wpe/features.conf`; device patches may
adapt toolchains, dependencies, or GPU-buffer ABIs but must not change WebKit
capabilities.

Build and run the local device through its isolated rootfs with:

```sh
make run-local
make run-web-local
```

Both commands first build the pinned WPE sysroot with the native install prefix
`/opt/oos`, package the WIT and Web launchers, then enter the rootfs through an
unprivileged bubblewrap user namespace. `proot` is the fallback when
bubblewrap is unavailable. The namespace uses a synthetic `/dev` and software
Mesa, so no physical host device nodes are exposed.
Local rootfs packaging also generates a GStreamer plugin registry for the
bound host `/usr`. The runner disables registry updates inside the read-only
namespace, avoiding a blocking plugin rescan on every WPE WebProcess startup.

The outputs are:

- `build/android-nokia-2780-flip/bin/oos`: production executable.
- `build/native-apps/launcher.wasm`: portable interpreter/debug Launcher.
- `build/native-apps/launcher.component.wasm`: standard Component Model
  Launcher for compatible runtimes and interface tooling.
- `build/native-apps/launcher.aot`: production ARMv7 AOT Launcher.
- `build/android-nokia-2780-flip/bin/tests/nokia-2780-flip/`: on-device tests.
- `build/android-nokia-8110-4g/bin/oos`: Nokia 8110 production executable.
- `build/wpe-sysroot/nokia-2780-flip`: isolated Android 10 WPE sysroot.
- `build/wpe-sysroot/nokia-8110-4g`: isolated Android 6 WPE sysroot.
- `build/wpe-sysroot/local-root`: local rootfs containing `/opt/oos`.

The KaiOS performance profile deliberately retains JavaScriptCore Baseline and
DFG JIT plus WebAssembly BBQ JIT. FTL/OMG remain disabled because WebKit does
not support those optimizing tiers on this 32-bit ARM target. The build script
checks the configured features and ARMv7 JIT objects before reporting success.
See [docs/wpe-runtime-profile.md](docs/wpe-runtime-profile.md) for the retained
runtime surface, disabled feature groups, and ARMv7 softfp JIT boundary.

For a production-only build, configure with:

```sh
make system DEVICE=nokia-2780-flip \
  CMAKE_ARGS=-DOOS_BUILD_DEVICE_TESTS=OFF
```

## Display Tests

WPE never owns the host display connection. On hardware, its isolated producer
transfers GPU buffers to the OOS host over the common surface transport. On
`local`, WPEBackend-fdo lends each SHM frame to `WpeSurfaceHost` in the OOS Web
runner. Native/WASM applications and received WPE surfaces are routed only
through the OOS compositor; only the selected device backend can access the
SDL window or hardware HWC, panel power, and backlights.
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

See `system/devices/nokia-2780-flip/README.md` for the hardware constraints, public
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
its shared libraries, helper processes, and versioned application ZIPs.
Generate both directory and tgz outputs with:

```sh
./scripts/package-oos-scaffold.sh --res-version 1.0.0 --tgz
./scripts/package-oos-res.sh 1.0.0 --activate --tgz
```

See [docs/deployment.md](docs/deployment.md) for the production
`/system/oos`, trial `/data/local/tmp/oos`, persistent `/data/oos`, upgrade,
mount, and lifecycle contract.

See [docs/applications.md](docs/applications.md) for OOS/KaiOS ZIP formats,
runtime dispatch, the application registry, and persistent storage layout.

See [docs/wasm-runtime.md](docs/wasm-runtime.md) for the WIT lifecycle and
device interfaces, WAMR/Component Model boundary, isolation model, egui
adapter, and cross-language application path.
