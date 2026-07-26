# Orange OS

Orange OS (`oos`) is a native C++ system shell intended to replace B2G on
KaiOS devices. The production executable currently has an intentionally empty
entry point; device enablement and the WPE WebKit display path are being built
and validated independently before application logic is added.

## Repository Layout

- `app/main.cpp`: production `oos` process entry point.
- `core`: device-independent runtime modules. `oos_input` provides reusable
  Linux evdev key capture for the future production event loop. The network
  interfaces define reusable Wi-Fi, IP, and Bluetooth lifecycle APIs. The
  modem interface exposes Radio HAL state without leaking HIDL types. Hardware
  interfaces cover audio, vibration, power/flip lifecycle, camera/flash, and
  hardware video codecs.
- `devices/nokia-2780-flip/include/oos/nokia2780`: reusable public display
  interfaces for the validated Nokia 2780 implementation.
- `devices/nokia-2780-flip/src`: device display and WPE/HWC implementation.
- `devices/nokia-2780-flip/tests`: on-device smoke, lifecycle, animation, and
  investigation test programs. Test-only fixtures live under `tests/support`.
- `devices/nokia-8110-4g`: reserved device slot; no implementation yet.
- `scripts`: build, deployment, and test runners.
- `packaging/scaffold`: stable device-side bootstrap script templates.
- `third_party`: local AOSP, Gecko, KaiOS, and WPE checkouts, excluded from the
  main Git repository.
- `patches`: tracked compatibility changes for clean third-party checkouts.

Production code must not depend on files under a device's `tests` directory.
Validated functionality moves into a public device library before it is used
by `app/main.cpp`.

## Build Environment

Set the extracted Android system path in `.env`:

```sh
SYSTEM_DIR=/home/jax/tmp/system-stock-2780/system
```

Set `WPE_DISTROBOX=oos-debian12` to build WPE/Cerbero inside the Debian 12
Distrobox. Leave it unset to build in the current environment.
`WPE_BUILD_JOBS` optionally caps parallel compilation; it defaults to all
available CPU threads.

Build the WPE runtime, configure the Android target, and compile Orange OS:

```sh
./scripts/build-wpe-sysroot.sh
./scripts/configure-android.sh nokia-2780-flip
cmake --build build/android-nokia-2780-flip -j8
```

The outputs are:

- `build/android-nokia-2780-flip/bin/oos`: production executable.
- `build/android-nokia-2780-flip/bin/tests/nokia-2780-flip/`: on-device tests.

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

The lower-latency single-process validation keeps WebKit, the retained GPU
buffer, fb1, and the primary HWC client in one process:

```sh
./scripts/test-single-process-switch.sh
```

It preloads the page with both backlights off, then shows the cover for three
seconds, the WPE primary frame for three seconds, and the cover for three
seconds. The validated Nokia 2780 transition times are approximately 622-637ms
from primary to cover and 439ms from cover to primary.

Other repeatable tests are:

```sh
./scripts/test-primary-lifecycle.sh 3
./scripts/test-display-lifecycle.sh 3
./scripts/run-cover-test.sh secondary
./scripts/test-key-input.sh
./scripts/run-wpe-chroot.sh start
./scripts/run-wpe-chroot.sh stop
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
