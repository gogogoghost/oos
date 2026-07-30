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
and sends each opaque GPU handle once over `SOCK_SEQPACKET`. Later frames refer
to that connection-local allocation by `buffer_id`; the OOS host retains the
import until the producer disconnects and acknowledges every frame only after
composition. This avoids per-frame FD transfer and gralloc registration while
preserving producer backpressure. WPE's release/frame-complete replies use the
per-EGLTarget channel so WebKit receives them on its compositor thread, as
required by `ThreadedCompositor`.

The processes deliberately have separate library paths: WPE uses its isolated
sysroot and the host uses the stock graphics stack. This prevents C++ ABI and
common-SONAME collisions on both Android 6 and Android 10.

WPE code must never open HWC, write a framebuffer, switch displays, or control
a backlight. Only the OOS host owns those capabilities.

The local path preserves the same ownership rule. WebKit's Web process remains
separate, while the OOS Web runner owns both the WPE host adapter and the OOS
compositor:

```text
WPEWebProcess -> WPEBackend-fdo SHM -> OOS WpeSurfaceHost
              -> OOS Compositor -> local llvmpipe GLES -> SDL window
```

WPEBackend-fdo lends the ARGB8888 buffer to OOS for the duration of the frame
callback. `WpeSurfaceHost` does not allocate or copy an intermediate image; it
passes the borrowed pointer synchronously through the compositor. The local
display uploads it into one persistent llvmpipe texture, then returns the SHM
buffer to WPE after presentation. That single bounded pixel upload is the
necessary copy in the deliberately GPU-independent local backend. WPE never
opens the SDL/Wayland display or bypasses OOS composition.

`package-local-rootfs.sh` generates a GStreamer plugin registry from the same
host `/usr` tree that is bound into the namespace. `run-local-rootfs.sh` uses
that registry with updates disabled because the rootfs is read-only. Repackage
the rootfs after changing host GStreamer plugins; otherwise WebKit would have
to synchronously rescan them during WebProcess startup.

Native/WASM applications also use this compositor. The initial interface
accepts one opaque full-screen surface; surface placement, z-order, opacity,
and multi-surface transactions remain OOS policy and can be expanded without
changing WPE or device HAL ownership.

Registered web applications keep their original package as
`/data/packages/<id>/<version>/<content-key>/application.zip`. The WPE runner
serves one requested ZIP entry at a time from the loopback-only
`http://<id>.localhost:8080` origin,
so installing or launching does not expand a complete application tree. Each
app receives a persistent `WebKitNetworkSession` rooted at
`/data/users/0/web/<id>` and a content-versioned cache below
`/data/cache/web/<id>`. Package type selects `kaios-b2g48` or `kaios-v3`.
The HTTP listener serves only package resources. Privileged KaiOS calls use an
injected WebKit message handler and a separate inherited control socket to the
OOS host. DeviceStorage, power, vibration, camera, device-capability, managed
system services, and KaiOS 2.5 application-owned DataStore operations use this
path. Wi-Fi/IP, Bluetooth, and mobile-network requests are rejected at this
boundary; future
capabilities must follow the same pattern instead of adding HTTP endpoints.
The bridge preserves the API-version boundary: 2.5 uses the legacy navigator
names, 3.0 uses `navigator.b2g` where KaiOS defines it, and 3.0
DeviceCapability is exposed through its documented daemon-service factory.
KaiOS 2.5 DataStore records reuse the application-private WIT storage backend;
KaiOS 3 does not receive the legacy `getDataStores` entry point.
The native service contract is also represented in WIT so WPE adapters and
WAMR imports converge on the same host implementation.

The runner serializes requests on a worker thread and completes WebKit replies
on the GLib main context. The OOS process services the channel on its own thread.
Long device discovery calls cannot stall key dispatch or frame composition.

On Android the formal process boundary is `oos` plus one foreground
`oos-wpe`. The host creates the surface socket, launches the producer from the
resolved registry record, acknowledges every imported frame after composition,
and sends key events in the reverse direction. Closing the app or stopping OOS
terminates the producer and removes the runtime socket.

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
| Local | `linux-x86_64-jit` | SHM ARGB8888 | SDL/llvmpipe GLES | `build/wpe-sysroot/local-root/opt/oos` |

The 8110 compatibility library implements the small AHardwareBuffer ABI used
by WPE over Android 6 gralloc0/native handles. It preserves GPU allocation,
IPC handle transfer, and EGLImage import; it does not copy pixels through the
CPU. Resource packaging takes this shim from the current Android device build,
not an older WPE sysroot copy, because it also supplies Android 23 ashmem
compatibility used by WebKit shared memory.

Each device mapping lives in `system/config/wpe/devices/<device>.env`. Build with:

```sh
./scripts/build-wpe-sysroot.sh nokia-2780-flip
./scripts/build-wpe-sysroot.sh nokia-8110-4g
./scripts/build-wpe-local.sh
```

## Shared KaiOS Capability Profile

`system/config/wpe/features.conf` is the only WPE feature source for local and
Android builds. It retains the browser capabilities used by KaiOS 2.5/3
applications, including HTML audio/video, AudioContext, MediaSource,
getUserMedia/WebRTC, MediaRecorder, notifications, geolocation, fullscreen,
WebGL, and WebAssembly. XR, gamepad, PDF.js, WebDriver, and OOS-owned browser
shell facilities remain disabled.

The Android Cerbero recipe receives this list through a build environment
bridge. Device patches contain only toolchain, dependency, Bionic, JIT codegen,
or GPU-buffer compatibility changes. Both build paths verify every option in
the generated CMake cache and reject mismatches in `cmakeconfig.h`.

This profile controls WebKit compile-time APIs. KaiOS-specific privileged
objects such as `navigator.moz*` and `navigator.b2g` still require an OOS
permission and JavaScript bridge; compiling WPE does not claim those objects
are implemented.

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

The current ARMv7 profiles set `JSC_useEagerBBQCompilation=true`. Module
instantiation therefore waits until every internal function has compiled with
BBQ. This trades startup latency for stable execution latency and makes JIT
compilation failures visible before application code runs. ARMv7 retains BBQ
as its only executable WebAssembly tier; loop OSR entrypoints remain disabled.

FTL and WebAssembly OMG are disabled because WebKit does not support those
optimizing tiers on this 32-bit ARM target. The C-loop interpreter is disabled,
so accidentally losing JIT support fails the build instead of shipping a slow
fallback.

Android shared-library boundaries use the base AAPCS softfp ABI. JSC-generated
ARMv7 code uses `aapcs-vfp` only for its internal operation calls and matching
function-pointer wrappers. Public and system ABI boundaries remain softfp.

The eager mode changes compilation timing, not generated-code correctness. A
known ARM32 BBQ miscompilation found with OmniJ2ME is tracked in
[wpe-arm32-wasm.md](wpe-arm32-wasm.md).

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

The shared fixture is `system/tests/web/assets/hello.html`. The process entry
points are `system/tests/web/wpe_hello_producer.cpp` and
`system/tests/web/wpe_surface_host_test.cpp`; both use the common transport in
`system/src/oos/compositor/surface_transport.c`.
