# WAMR Native-App Runtime

## First-Version Scope

OOS keeps native applications resident in one process through WAMR 2.4.4.
Exactly one application session is active and receives input and frame
callbacks; background instances retain their isolated WASM/UI state and
dedicated compositor layer without rendering.
The host owns display lifecycle, EGL/GLES, HWC, key devices, clocks, and phone
services. A guest can only reach interfaces in the versioned
`oos:platform@0.1.0` WIT package.

The production SystemUI is a trusted, process-local LVGL component and does
not allocate a WAMR instance. The egui 0.35 Launcher remains the first SDK
integration app: it uses the reusable `oos-egui` adapter and compiles to
`wasm32-unknown-unknown` without JavaScript, a DOM, WebView, or WASI.

```text
evdev -> OOS key normalization -> oos:platform/lifecycle#event
                                  |
                              Wasm application
                                  |
        textures + indexed meshes or GLES2 command batch
                                  |
WIT Canonical ABI validation -> GLES2 -> RGB565 HWC target
```

## Application Lifecycle

Every app implements the WIT `lifecycle` interface:

- `init() -> result`
- `event(key-event)`
- `frame(monotonic-time-us) -> result<u32, error-code>`
- `shutdown()`

The generated core-Wasm export names are versioned, for example
`oos:platform/lifecycle@0.1.0#frame`. Applications use generated bindings and
must not depend on those lowered names directly.

`ApplicationSessionManager` applies the same lifecycle to built-in and WAMR
applications. Every resident module owns a `WasmApp` and an independent
compositor `GraphicsHost`, so linear memory, UI state, retained draw state, and
texture namespaces survive foreground switches without colliding. The manager
and every WIT import run on the OOS event-loop thread. A non-zero
return, trap, missing export, invalid pointer, invalid draw range, or resource
limit violation fails that app call instead of passing untrusted data to GLES.

The lifecycle execution stack and total linear-memory limit are host launch
policy, not manifest fields. The production C profile starts at a 512 KiB
lifecycle stack. Its one guest worker has a fixed 2 MiB auxiliary stack because
WAMR partitions that region from module linker metadata at build time. WAMR's
optional host-managed heap is disabled; allocation
through the generated WIT bindings uses the module's own linear memory and
exported `cabi_realloc`. Core
Wasm modules must declare one memory maximum no larger than 64 MiB; unbounded
or oversized core modules are rejected before WAMR loads them. Instantiation
also applies WAMR's 1,024-page cap and verifies the resulting byte size, so the
same 64 MiB policy covers AOT-only packages.

WAMR shared memory, thread manager, and lib-pthread are enabled for one bounded
guest worker in addition to the lifecycle thread. The worker may execute guest
code and atomics. ABI discovery, precise monotonic/epoch clocks, and the
coalescing main-thread wake call are explicitly worker-safe. Every other OOS
WIT import compares the current host thread to the session lifecycle thread and
traps misuse.
WAMR joins cluster threads during instance teardown before OOS releases host
resources.

## WIT Interface Package

`sdk/wit/oos.wit` is the single source of truth. It describes runtime/logging,
graphics, read-only system fonts, device identity and capabilities, device
services, application-private storage, user-visible device storage, and
lifecycle interfaces with WIT records, enums, flags, lists, strings, and
results. The host registers matching
versioned WAMR modules and implements the Canonical ABI lowering.

The portable graphics interface provides surface dimensions and format,
bounded A8/RGB565/RGBA4444/RGBA8888 texture updates, and indexed triangle
physical-pixel batches with clip rectangles. `pixels-per-point` lets GUI
adapters convert their logical coordinates once before submission. Vertices
contain physical pixel position, UV, and premultiplied RGBA color. The
validated GLES2 interface adds shaders, programs,
buffers, depth/stencil state, and one fixed-record command batch per frame for
game-engine backends. Both paths present the host's retained target, so the
guest never maps a framebuffer or receives a native GPU handle. See
[graphics.md](graphics.md) for the complete format and composition contract.

The WIT `runtime` interface also exposes
`set-status-bar-style(background-rgb, icons)`. The color is strict
`0x00RRGGBB`; the icon theme is `light` or `dark`. The call updates retained
session chrome rather than SystemUI directly, so the value is restored on
activation and cannot leak from a hidden application.
`set-surface-mode(normal|immersive)` changes the retained compositor layer
between status-safe geometry and the complete physical display. The status bar
is hidden in immersive mode, `graphics.surface-size` changes synchronously, and
the mode is restored with the resident session. Lifecycle frame delays,
worker-safe clocks, dynamic media sources, and child runtimes use ABI 6.

The lifecycle result is the next requested frame delay in milliseconds. OOS
schedules the earliest application/SystemUI deadline, clamps idle waits to one
second, and uses input plus `eventfd` wakeups instead of a fixed 33 ms sleep.
Worker command queues call `wake-main-thread` after publishing work.

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
to a permission mask once at launch. Each privileged WIT call performs one mask
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

The `audio` interface reports the running build's real decoder set by canonical
MIME type. It provides immutable session-owned byte sources, asynchronous
managed playback, legacy MIDI/ringtone synthesis, modern/phone codec decoding,
and bounded PCM streams as described in [media.md](media.md). Applications must query this list
instead of inferring support from the phone model or Android API level.

The separate `device-storage` interface reads and writes user-visible internal
or removable media. WAMR writes directly from validated guest linear memory;
reads allocate the Canonical ABI result in guest memory and read file bytes into
that allocation without an intermediate host byte vector. It is exposed only
when a granted permission starts with `device-storage:`.

The separate unprivileged `font-assets` interface maps semantic roles to fixed
platform fonts; it never accepts a path from the Guest.
Loading allocates the Canonical ABI result once and reads directly into Guest
linear memory. The `ui-proportional` role resolves to the same system Roboto
font used by native OOS UI, so framework apps do not need to embed it in every
ZIP.

The `system-services` interface is a trusted JSON policy broker for SystemUI.
It is registered only for applications granted `system`; ordinary WAMR apps do
not instantiate its SQLite service. It manages software state and events, not
device drivers.

## Ephemeral Child Modules

An application package may contain named `modules/<name>.wasm` and
`modules/<name>.aot` files. The `subruntime` interface permits one child at a
time and prefers AOT. A child inherits the parent's storage and device
capabilities but owns an independent WAMR instance, worker cluster, stacks, and
linear memory. Creation and activation are deferred until the parent import has
returned. OOS then suspends the parent and routes lifecycle, input, graphics,
and device calls to the child on the normal event-loop thread. Child completion,
exit, failure, and cancellation are explicit states; resuming the parent never
re-enters it from a native callback. Destroy joins a live worker, tears down host
resources, and releases the entire child linear memory.

This is the recoverable-memory model for compiler/game workloads. Host stress
coverage checks deferred activation, event-loop thread affinity, completion,
stale handles, worker WIT traps, and `smaps_rollup` PSS recovery.

## WAMR And Components

WIT is the interface definition language for the Component Model, but the
pinned WAMR 2.4.4 VM does not implement a Component Model loader. Rust
`wit-bindgen` therefore lowers the world to standard Canonical ABI core-Wasm
imports and exports. WAMR runs that core module directly and `wamrc` compiles
the same module to ARMv7 AOT.

The build also runs `wasm-tools component new` and emits
`egui-demo.component.wasm`. That file is a real component for compatible
runtimes, composition tools, and host-binding generators; it is not passed to
WAMR. This preserves the phone's tested AOT path without inventing another
interface model.

## Execution And Isolation

Each native application ZIP contains `entry.wasm`, `entry.aot`, or both. AOT is
preferred when present, and AOT-only production packages avoid carrying a
redundant core Wasm module.

The optional `egui-demo.component.wasm` remains a build/tooling artifact until
the runtime has a Component Model loader; it is not required in the device
package.

AOT retains WebAssembly bounds checks and WAMR validation. WAMR's interpreter
remains available for portable `entry.wasm` packages; JIT is not part of the
native-app runtime. AOT is deterministic, needs no writable-executable memory
on the phone, and is available for this ARMv7 target. This decision is
independent from any browser engine or JavaScript runtime.

Module files are loaded with writable private `mmap` rather than copied into
anonymous malloc memory. Unmodified AOT pages stay file-backed and reclaimable;
multiple instances of the same module also share those physical pages.

WASI, SIMD, cross-module imports/dependency linking, and direct dynamic library
access are disabled. The only libc-shaped imports are WAMR's pthread contract;
the C SDK supplies its allocator inside the guest. Independent app instances do
not import from each other. Service providers must be narrow OOS capabilities
associated with an application manifest. WAMR memory safety does not replace
that permission layer.

## Framework Compatibility

egui is the first guest framework and validates the complete route from framework UI
to phone GPU. `oos-egui::Renderer` retains its frame buffers and handles
texture deltas, tessellation, index rebasing, clip rectangles, and WIT
submissions, leaving Launcher with application UI logic only. Its
`CanvasTexture` accepts direct RGB565 and other supported formats for emulator
or software-rendered content embedded in a UI. `oos-egui::Input` preserves key
press, repeat, and release state and builds correctly scaled `RawInput` values.
The renderer returns non-painting egui output for the application/SystemUI
integration to handle explicitly.

iced is next: its custom renderer/backend can target the same WIT texture and
mesh interface. The adapter must live in the SDK and avoid depending on wgpu
because the device graphics stack is GLES-oriented.

The built-in LVGL display and keypad ports target `GraphicsHost` without
exposing Linux devices. LVGL renders into two 32-row RGB565 buffers, uploads
only invalidated rectangles, and presents one GPU-composited quad. The
process-local Dear ImGui backend translates textures, vertices, `u16` indices,
texture IDs, and clip rectangles directly into the same host records. Both
backends are device-independent and therefore run unchanged on the 2780,
8110, and local target. The C guest LVGL adapter lowers partial RGB565 updates
through WIT and exposes its quad rather than forcing a second submit. Its
overlay mode renders ARGB8888, converts dirty rows to premultiplied RGBA8888,
and keeps the root transparent above a retained game texture. Resize replaces
the texture, updates LVGL resolution, and invalidates the full active screen.
Engines
that need custom shaders use the
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

The host integration test loads three egui demo instances through
`NativeAppManager`, verifies the resident limit, switches input/rendering to
the active instance, and checks texture cleanup. It then loads a generated WIT
smoke guest that imports every device-service interface and validates
Canonical ABI error lifting. It also executes a C shared-memory pthread guest,
checks repeated worker join, clocks and wakeups, rejects an unbounded-memory
module, and compiles that same guest to ARMv7 AOT. It also covers dynamic MIDI
bytes, child failure recovery, live-worker teardown, and PSS recovery. The
interface verifier rejects legacy `oos_*`
imports and checks both core-Wasm and component lifecycle exports. The Android
build also emits `oos_test_nokia_2780_wasm_multi_app`; it cycles three
independent Launcher states on the physical display for suspend/resume and
memory validation.

WAMR uses local LLVM by default. Set `OOS_WAMR_DISTROBOX` only when its LLVM
toolchain can live in a dedicated Distrobox without affecting device builds.

## Nokia 2780 Validation

The exact egui Launcher module was validated through both execution engines
on the phone. With the original 24 MiB heap and current 30 FPS loop, the
interpreter used about 78.1% of one CPU with 58,972 KiB RSS. ARMv7 AOT used
about 6.2% of one CPU with 62,212 KiB RSS. AOT therefore reduced sampled CPU
by roughly 92%.

After setting the default heap to 4 MiB and using file-backed module mappings,
one egui test Launcher measured 42,000 KiB RSS, 36,573 KiB PSS, and 17,024
KiB anonymous memory. Three simultaneously resident egui instances measured
81,148 KiB RSS, 60,295 KiB PSS, and 40,916 KiB anonymous memory while switching
perfectly among three independent UI states once per second. `MemAvailable`
was 171,052 KiB. This three-instance test maps the same AOT file and therefore
shares its clean file pages; three unrelated apps will occupy more clean code
pages, so 60 MiB PSS is a best case rather than a universal promise.

These are steady-state `top`, `smaps_rollup`, and `/proc/meminfo` samples, not
a formal benchmark. System capacity decisions must use PSS and `MemAvailable`,
not `MemFree` or a sum of per-process RSS. Static-screen invalidation remains a
future memory and power optimization.
