# Orange OS

Orange OS (`oos`) is a native C++ system shell intended to replace B2G on
KaiOS devices. The production executable currently has an intentionally empty
entry point; device enablement and the WPE WebKit display path are being built
and validated independently before application logic is added.

## Repository Layout

- `app/main.cpp`: production `oos` process entry point.
- `devices/nokia-2780-flip/include/oos/nokia2780`: reusable public display
  interfaces for the validated Nokia 2780 implementation.
- `devices/nokia-2780-flip/src`: device display and WPE/HWC implementation.
- `devices/nokia-2780-flip/tests`: on-device smoke, lifecycle, animation, and
  investigation test programs. Test-only fixtures live under `tests/support`.
- `devices/nokia-8110-4g`: reserved device slot; no implementation yet.
- `scripts`: build, deployment, and test runners.
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

Build the WPE runtime, configure the Android target, and compile Orange OS:

```sh
./scripts/build-wpe-sysroot.sh
./scripts/configure-android.sh nokia-2780-flip
cmake --build build/android-nokia-2780-flip -j8
```

The outputs are:

- `build/android-nokia-2780-flip/bin/oos`: production executable.
- `build/android-nokia-2780-flip/bin/tests/nokia-2780-flip/`: on-device tests.

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
./scripts/run-wpe-chroot.sh start
./scripts/run-wpe-chroot.sh stop
```

See `devices/nokia-2780-flip/README.md` for the hardware constraints, public
API ownership rules, and individual test targets.
