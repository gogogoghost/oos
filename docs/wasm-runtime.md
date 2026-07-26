# WAMR Native-App Runtime

## First-Version Scope

OOS keeps up to three native applications resident in one process through
WAMR 2.4.4. Exactly one is active and receives input and frame callbacks;
background instances retain their isolated WASM/UI state without rendering.
The host owns display lifecycle, EGL/GLES, HWC, key devices, clocks, and future
phone services. A guest can only reach explicitly registered functions in the
`oos` import module.

The production Launcher is the first native app. It uses egui 0.35 through the
reusable `oos-egui` adapter and compiles to `wasm32-unknown-unknown`. It does
not use JavaScript, a DOM, WebView, or WASI.

```text
evdev -> OOS key normalization -> oos_app_event
                                  |
                              egui Launcher
                                  |
                 vertices + indices + textures
                                  |
WAMR pointer/limit validation -> GLES2 -> RGB565 HWC target
```

## Application Lifecycle

Every app must export:

- `oos_app_init() -> i32`
- `oos_app_event(code, action, time_low, time_high)`
- `oos_app_frame(time_low, time_high) -> i32`
- `oos_app_shutdown()`

`NativeAppManager` loads, activates, removes, and shuts down a maximum of three
resident modules. All instances share one process-level WAMR runtime and GPU
host but have separate linear memory and texture namespaces. The manager runs
on the OOS event-loop thread because WAMR threads are disabled. A non-zero
return, trap, missing export, invalid pointer, invalid draw range, or resource
limit violation fails that app call instead of passing untrusted data to GLES.

The default guest heap is 4 MiB plus the module's initial linear memory. Future
app manifests may request a different bounded budget; silently granting a
large per-app heap is not acceptable on this device.

## ABI v1

ABI v1 provides surface dimensions, wall-clock minutes, bounded logging,
RGBA8 texture create/update/free, and indexed triangle batches with clip
rectangles. Vertices contain logical pixel position, UV, and premultiplied
RGBA color. The host converts those commands to GLES and presents its retained
RGB565 HWC buffer, so the guest never maps a framebuffer or receives a GPU
handle.

Limits are part of the ABI: 2048-pixel texture dimensions, 16 MiB per upload,
65,535 vertices, 196,605 indices, and 4,096 draw commands per submission. Full
texture replacement and partial atlas updates are distinct flags so framework
font atlases can grow safely.

## Execution And Isolation

The res package contains both forms:

- `launcher.aot`: ARMv7 AOT output, used in production.
- `launcher.wasm`: portable bytecode, retained for debugging and ABI tests.

AOT retains WebAssembly bounds checks and WAMR validation but removes the
continuous interpreter cost on the Nokia 2780. JIT is deliberately not part
of the native-app runtime: AOT is deterministic, needs no writable-executable
memory on the phone, and is available for this ARMv7 target. This decision is
independent from JavaScriptCore JIT retained for WPE/KaiOS applications.

Module files are loaded with writable private `mmap` rather than copied into
anonymous malloc memory. Unmodified AOT pages stay file-backed and reclaimable;
multiple instances of the same module also share those physical pages.

WASI, libc imports, threads, SIMD, cross-module imports/dependency linking,
and direct dynamic library access are disabled. Independent app instances do
not import from each other. Future permissions such as networking, audio,
modem, storage, notifications, and clipboard must be added as narrow OOS host
capabilities and associated with an application manifest. WAMR memory safety
does not replace that permission layer.

## Framework Compatibility

egui is implemented first and validates the complete route from framework UI
to phone GPU. `oos-egui` handles texture deltas, tessellation, index rebasing,
clip rectangles, and ABI submissions, leaving Launcher with application UI
logic only.

iced is next: its custom renderer/backend can target the same host texture and
mesh ABI. The adapter must live in the SDK and avoid depending on wgpu because
the device graphics stack is GLES-oriented.

LVGL will use the C ABI. Its display and input ports can target OOS without
exposing Linux devices. Initial compatibility may upload dirty rectangles as
textures; a later draw-unit adapter can preserve more vector work on the GPU.
This is a different performance path but does not require a second device
display implementation.

## Build And Test

Fetch the pinned runtime, build the Launcher, and run the host integration
test:

```sh
./scripts/fetch-wamr.sh
./scripts/build-native-app-aot.sh
./scripts/test-wasm-runtime.sh
```

The host integration test loads three Launcher instances through
`NativeAppManager`, verifies the resident limit, switches input/rendering to
the active instance, and checks texture cleanup. The Android build also emits
`oos_test_nokia_2780_wasm_multi_app`; it cycles three independent Launcher
states on the physical display for suspend/resume and memory validation.

WAMR uses local LLVM by default. Set `OOS_WAMR_DISTROBOX` only when its LLVM
toolchain lives in a Distrobox; this is separate from `WPE_DISTROBOX`.

## Nokia 2780 Validation

The exact egui Launcher module was validated through both execution engines
on the phone. With the original 24 MiB heap and current 30 FPS loop, the
interpreter used about 78.1% of one CPU with 58,972 KiB RSS. ARMv7 AOT used
about 6.2% of one CPU with 62,212 KiB RSS. AOT therefore reduced sampled CPU
by roughly 92%.

After setting the default heap to 4 MiB and using file-backed module mappings,
one production Launcher measured 42,000 KiB RSS, 36,573 KiB PSS, and 17,024
KiB anonymous memory. Three simultaneously resident egui instances measured
81,148 KiB RSS, 60,295 KiB PSS, and 40,916 KiB anonymous memory while switching
perfectly among three independent UI states once per second. `MemAvailable`
was 171,052 KiB. This three-instance test maps the same AOT file and therefore
shares its clean file pages; three unrelated apps will occupy more clean code
pages, so 60 MiB PSS is a best case rather than a universal promise.

These are steady-state `top`, `smaps_rollup`, and `/proc/meminfo` samples, not
a formal benchmark. System capacity decisions must use PSS and `MemAvailable`,
not `MemFree` or a sum of per-process RSS. Static-screen invalidation and a
host font service remain future memory and power optimizations.
