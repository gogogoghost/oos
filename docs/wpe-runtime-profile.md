# WPE Runtime Profiles

WPE provides KaiOS web-application compatibility. It is a frame producer, not
the OOS window manager. The OOS host owns the only physical display connection,
EGL composition target, layer policy, HWC session, panel power, and backlight.

The runtime path is:

```text
WPEWebProcess -> WpeSurfaceHost (producer process)
              -> AHardwareBuffer Unix transport
              -> OOS host process -> Compositor -> device Display backend
              -> RGB565 scanout -> HWC -> panel
```

`WpeSurfaceHost` is part of the WPE producer. It waits for the acquire fence
and sends the opaque GPU handle over `SOCK_SEQPACKET`. The OOS host imports the
handle into `oos::compositor::Compositor` and acknowledges it only after
composition. WPE then sends release/frame-complete. The processes deliberately
have separate library paths: WPE uses its isolated sysroot and the host uses
the stock graphics stack. This prevents C++ ABI and common-SONAME collisions
on both Android 6 and Android 10.

WPE code must never open HWC, write a framebuffer, switch displays, or control
a backlight. Only the OOS host owns those capabilities.

Native/WASM applications also use this compositor. The initial interface
accepts one opaque full-screen surface; surface placement, z-order, opacity,
and multi-surface transactions remain OOS policy and can be expanded without
changing WPE or device HAL ownership.

## Profile Model

Android version alone is not a sufficient compatibility key. A WPE engine
profile is identified by:

- Android/Bionic API level;
- CPU architecture and Android ABI;
- JavaScriptCore JIT and WebAssembly tiers;
- C++ runtime SONAME/ABI;
- kernel primitives required by WebKit helper processes and JIT mappings.

The display adapter is selected separately by:

- producer buffer ABI (AHardwareBuffer or a compatibility transport);
- gralloc generation and accepted pixel formats;
- EGL native-buffer import support;
- HWC generation and panel lifecycle requirements.

Current validated build mappings are:

| Device | Engine profile | Buffer adapter | Scanout backend | Sysroot |
| --- | --- | --- | --- | --- |
| Nokia 2780 Flip | `android29-armv7-jit` | AHardwareBuffer | HWC2/HIDL | `build/wpe-sysroot/nokia-2780-flip` |
| Nokia 8110 4G | `android23-armv7-jit` | gralloc0 AHardwareBuffer compatibility ABI | HWC1 | `build/wpe-sysroot/nokia-8110-4g` |

The 8110 compatibility library implements the small AHardwareBuffer ABI used
by WPE over Android 6 gralloc0/native handles. It preserves GPU allocation,
IPC handle transfer, and EGLImage import; it does not copy pixels through the
CPU.

Each device mapping lives in `config/wpe/devices/<device>.env`. Build with:

```sh
./scripts/build-wpe-sysroot.sh nokia-2780-flip
./scripts/build-wpe-sysroot.sh nokia-8110-4g
```

The API 23 profile defaults to eight compiler jobs because 32 simultaneous
WebCore unified sources exceed the available practical memory even with swap.
Set `WPE_BUILD_JOBS` explicitly to override the device default. This controls
parallelism only; it does not change profile flags or output identity.

## Build Isolation

Android 6 and Android 10 builds must never share target state. Each profile
has its own:

- Cerbero config and target cache;
- extracted source/work directory;
- downloaded source cache and logs;
- install prefix and profile manifest.

The host-only Cerbero bootstrap tools may be shared. The build script rejects
an API 23 mapping that points at the API 29 source key, writes a pending
manifest before the build, and publishes `.oos-wpe-profile` only after feature
verification succeeds. A failed build therefore cannot masquerade as a usable
sysroot.

## Required Runtime Features

Both current profiles retain and verify:

- JavaScriptCore Baseline JIT;
- JavaScriptCore DFG JIT;
- WebAssembly with the ARMv7 BBQ JIT;
- accelerated compositing, EGL, GLES, and WebGL;
- WPE's Android GPU-buffer producer backend.

FTL and WebAssembly OMG are disabled because WebKit does not support those
optimizing tiers on this 32-bit ARM target. The C-loop interpreter is disabled,
so accidentally losing JIT support fails the build instead of shipping a slow
fallback.

Android shared-library boundaries use the base AAPCS softfp ABI. JSC-generated
ARMv7 code uses `aapcs-vfp` only for its internal operation calls and matching
function-pointer wrappers. Public and system ABI boundaries remain softfp.

## New Device Admission

A new handset receives WPE support only after all of these pass:

1. Select an existing engine profile only when API, ABI, C++ runtime, kernel,
   and JIT requirements match; otherwise create a new isolated profile.
2. Implement or select a GPU-buffer adapter that can transfer producer buffers
   into OOS without a CPU pixel copy.
3. Implement `device::Display::presentSurface()` so the OOS compositor imports
   the buffer into its own scanout target. Do not add a WPE-owned display path.
4. Build the shared WPE producer and OOS compositor-host Hello World tests for
   that device.
5. On hardware, verify visible HTML/CSS output, DOM JavaScript PASS,
   WebAssembly PASS, executable JIT mappings in `WPEWebProcess`, clean buffer
   release, and screen shutdown after the test.

The shared fixture is `tests/web/assets/hello.html`. The process entry points
are `tests/web/wpe_hello_producer.cpp` and
`tests/web/wpe_surface_host_test.cpp`; both use the common transport in
`core/src/surface_transport.c`.
