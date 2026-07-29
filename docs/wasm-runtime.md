# WAMR Native-App Runtime

## First-Version Scope

OOS keeps up to three native applications resident in one process through
WAMR 2.4.4. Exactly one is active and receives input and frame callbacks;
background instances retain their isolated WASM/UI state without rendering.
The host owns display lifecycle, EGL/GLES, HWC, key devices, clocks, and phone
services. A guest can only reach interfaces in the versioned
`oos:platform@0.1.0` WIT package.

The production Launcher is the first native app. It uses egui 0.35 through the
reusable `oos-egui` adapter and compiles to `wasm32-unknown-unknown`. It does
not use JavaScript, a DOM, WebView, or WASI.

```text
evdev -> OOS key normalization -> oos:platform/lifecycle#event
                                  |
                              egui Launcher
                                  |
        textures + indexed meshes or GLES2 command batch
                                  |
WIT Canonical ABI validation -> GLES2 -> RGB565 HWC target
```

## Application Lifecycle

Every app implements the WIT `lifecycle` interface:

- `init() -> result`
- `event(key-event)`
- `frame(monotonic-time-us) -> result`
- `shutdown()`

The generated core-Wasm export names are versioned, for example
`oos:platform/lifecycle@0.1.0#frame`. Applications use generated bindings and
must not depend on those lowered names directly.

`NativeAppManager` loads, activates, removes, and shuts down a maximum of three
resident modules. All instances share one process-level WAMR runtime and GPU
host but have separate linear memory and texture namespaces. The manager runs
on the OOS event-loop thread because WAMR threads are disabled. A non-zero
return, trap, missing export, invalid pointer, invalid draw range, or resource
limit violation fails that app call instead of passing untrusted data to GLES.

The default guest heap is 4 MiB plus the module's initial linear memory. Future
app manifests may request a different bounded budget; silently granting a
large per-app heap is not acceptable on this device.

## WIT Interface Package

`apps/sdk/wit/oos.wit` is the single source of truth. It describes runtime/logging,
graphics, device identity and capabilities, audio, camera, power, vibration,
Wi-Fi/IP, Bluetooth, modem, codec, application-private storage, user-visible
device storage, and lifecycle interfaces with WIT records, enums, flags, lists,
strings, and results. The host registers matching
versioned WAMR modules and implements the Canonical ABI lowering.

The portable graphics interface provides surface dimensions and format,
bounded A8/RGB565/RGBA4444/RGBA8888 texture updates, and indexed triangle
batches with clip rectangles. Vertices contain logical pixel position, UV, and
premultiplied RGBA color. The validated GLES2 interface adds shaders, programs,
buffers, depth/stencil state, and one fixed-record command batch per frame for
game-engine backends. Both paths present the host's retained target, so the
guest never maps a framebuffer or receives a native GPU handle. See
[graphics.md](graphics.md) for the complete format and composition contract.

Limits are part of the interface: 2048-pixel texture dimensions, 16 MiB per
upload, 65,535 vertices, 196,605 indices, and 4,096 draw commands per
submission. Full texture replacement and partial atlas updates are distinct
flags so framework font atlases can grow safely.

Device identity and capability discovery are backed by the selected production
`Device`. All optional service interfaces are registered so generated apps can
link and perform capability fallback. The first hardware WIT call lazily creates
the target `ServiceProvider`; local uses deterministic test data and Android
delegates to the existing audio, camera, power, vibration, Wi-Fi/IP, Bluetooth,
modem, and codec managers. No HAL manager is initialized at app startup. A
runtime created without a `Device` returns the typed WIT `unavailable` error.
For repository-launched applications, granted manifest permissions are reduced
to a seven-bit mask once at launch. Each privileged WIT call performs one mask
test before the lazy provider is reached and returns `permission-denied` when
the capability was not granted. Raw `--module` hardware diagnostics explicitly
retain unrestricted service access because they have no application manifest.

The `storage` interface provides persistent byte-valued KV plus bounded
prepared SQLite statements. Database handles and statement cursors stay in
the native host; guests bind/read null, integer, float, text, and blob values
through validated WIT buffers. Each application receives only its configured
`/data/users/0/wasm/<app-id>` storage root. The app identity and launch-time
permission mask are carried into every runtime instance.
The KaiOS 2.5 application-owned DataStore adapter uses this same host storage
implementation for Web applications; it is an API-shape adapter, not a second
database service.

The separate `device-storage` interface reads and writes user-visible internal
or removable media. WAMR writes directly from validated guest linear memory;
reads allocate the Canonical ABI result in guest memory and read file bytes into
that allocation without an intermediate host byte vector. It is exposed only
when a granted permission starts with `device-storage:`.

## WAMR And Components

WIT is the interface definition language for the Component Model, but the
pinned WAMR 2.4.4 VM does not implement a Component Model loader. Rust
`wit-bindgen` therefore lowers the world to standard Canonical ABI core-Wasm
imports and exports. WAMR runs that core module directly and `wamrc` compiles
the same module to ARMv7 AOT.

The build also runs `wasm-tools component new` and emits
`launcher.component.wasm`. That file is a real component for compatible
runtimes, composition tools, and host-binding generators; it is not passed to
WAMR. This preserves the phone's tested AOT path without inventing another
interface model.

## Execution And Isolation

Each native application ZIP can contain both forms:

- `aot/armv7/wamr-2.4.4/app.aot`: used in production.
- `module/app.wasm`: portable core Wasm fallback and diagnostic form.

The optional `launcher.component.wasm` remains a build/tooling artifact until
the runtime has a Component Model loader; it is not required in the device
package.

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
not import from each other. Service providers must be narrow OOS capabilities
associated with an application manifest. WAMR memory safety does not replace
that permission layer.

## Framework Compatibility

egui is implemented first and validates the complete route from framework UI
to phone GPU. `oos-egui::Renderer` retains its frame buffers and handles
texture deltas, tessellation, index rebasing, clip rectangles, and WIT
submissions, leaving Launcher with application UI logic only. Its
`CanvasTexture` accepts direct RGB565 and other supported formats for emulator
or software-rendered content embedded in a UI.

iced is next: its custom renderer/backend can target the same WIT texture and
mesh interface. The adapter must live in the SDK and avoid depending on wgpu
because the device graphics stack is GLES-oriented.

LVGL can use C guest bindings generated from the same WIT. Its display and
input ports can target OOS without exposing Linux devices. It can upload
RGB565 dirty rectangles directly; a draw-unit adapter can instead emit the
portable indexed-mesh path. imgui maps its vertices, indices, texture IDs, and
clip rectangles to the same records. Engines that need custom shaders use the
batched GLES2 interface while the host continues to own composition.

## Build And Test

Fetch the pinned runtime, build the Launcher, and run the host integration
test:

```sh
./scripts/fetch-wamr.sh
make native-app-aot
make test-wasm
make verify-wit
```

The host integration test loads three Launcher instances through
`NativeAppManager`, verifies the resident limit, switches input/rendering to
the active instance, and checks texture cleanup. It then loads a generated WIT
smoke guest that imports every device-service interface and validates
Canonical ABI error lifting. The interface verifier rejects legacy `oos_*`
imports and checks both core-Wasm and component lifecycle exports. The Android
build also emits `oos_test_nokia_2780_wasm_multi_app`; it cycles three
independent Launcher states on the physical display for suspend/resume and
memory validation.

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
