# OOS Native-App SDK

OOS application interfaces are defined in `sdk/wit/oos.wit`. That versioned WIT
package is the public contract; applications must not declare WAMR imports or
export lifecycle symbols by hand.

- `rust/oos-app` runs `wit-bindgen` for the `oos:platform/app@0.3.0` world and
  provides small Rust convenience wrappers.
- `rust/oos-egui` converts egui textures and meshes into the generated WIT
  graphics records, provides keypad input/system-font integration, and exposes
  user canvas textures for direct RGB565 updates.
- `system/src/oos/runtime/graphics_types.h` is the renderer's internal
  canonical-layout mirror. It is deliberately outside the SDK.

The world imports runtime, portable graphics, validated batched GLES2, system
font assets, device discovery, services, and storage, and exports the
application lifecycle interface. Query `device.get-access` before presenting a
privileged workflow: hardware capability and the application's permission grant
are separate facts. `wit-bindgen` dead-strips unused core-Wasm
imports, so an application pays only for the capabilities it calls.

`oos_app::set_status_bar_style(rgb, dark_icons)` configures the calling
application session's immersive status bar. The host restores that style when
the session returns to the foreground.

`font-assets.load` returns an owned font byte vector for a fixed semantic role.
It does not expose host paths. Rust egui applications can call
`oos_egui::install_system_fonts`; the WIT result allocation is moved into egui
without another Guest copy. `ui-proportional` is guaranteed by the OOS host
and resolves to its platform system font. Optional monospace and emoji roles
return `unavailable` until a device font configuration provides them.

Rust applications implement `oos_app::App` and use the generated export macro:

```rust
struct App;

impl oos_app::App for App {
    fn init() -> Result<(), oos_app::ErrorCode> { Ok(()) }
    fn event(_: oos_app::KeyEvent) {}
    fn frame(_: u64) -> Result<u32, oos_app::ErrorCode> { Ok(1000) }
    fn shutdown() {}
}

oos_app::bindings::export!(App with_types_in oos_app::bindings);
```

Other languages should generate guest bindings from `sdk/wit/oos.wit` and target
the `app` world. The runtime contract contains no Rust-specific types, WASI,
EGL, framebuffer, Binder, evdev, file descriptor, or HAL object.

The checked-in C bindings under `c/generated` are reproduced by
`scripts/generate-c-sdk.sh`. `scripts/build-c-app.sh OUTPUT SOURCE...` links the
pinned picolibc 1.8.9 libc/libm, compiler-rt, and the pinned thread-safe TLSF
allocator for `wasm32-unknown-unknown`; WASI is not enabled. The linker rejects
undefined symbols and `scripts/validate-wasm-imports.sh` permits only versioned
OOS WIT plus the explicit WAMR pthread/semaphore imports. Set
`OOS_WASM_MEMORY_BYTES` controls the encoded memory maximum and defaults to the
64 MiB OOS ceiling. The host supplies the 512 KiB WAMR lifecycle execution
stack. The SDK fixes the one supported guest auxiliary stack at 2 MiB because
WAMR partitions the linked region at module build time; it cannot be changed
when an application instance is created.
C applications implement the four generated lifecycle exports.
`c/lvgl/oos_lvgl_backend` uploads either RGB565 dirty rectangles or a
premultiplied RGBA8888 transparent overlay and returns draw records so an app
can combine its UI and framebuffer in one submission.

The host permits one guest worker in addition to the lifecycle thread. Precise
clocks, ABI discovery, and `wake-main-thread` are worker-safe; every other OOS
WIT service enforces lifecycle-thread affinity and traps a worker call. Core Wasm must encode a memory maximum no
larger than 64 MiB. ARMv7 AOT is capped and checked again at instantiation.

The Rust crates in `sdk/rust` are libraries and intentionally do not own a
workspace or lock file. Each application locks its complete dependency graph in
its own directory and can use these crates through a path dependency.

The production WAMR 2.4.4 runtime loads the generated Canonical ABI core Wasm
or ARMv7 AOT file. `build-native-apps.sh` also wraps the same module as a real
Component Model artifact for runtimes that support components. WAMR 2.4.4 does
not itself load component binaries.

Framework adapters belong under `sdk/`, not in an individual application.
The publishable Solid platform renderer lives in `js`, its publishable Vite
integration lives in `js-vite-plugin`, the Clay adapter lives in `c/clay`,
the Rust egui example lives in `rust/oos-egui`, and the LVGL adapters live in
`c/lvgl` and `cpp`.
Applications under `apps/` link only the adapter
they use. Future iced and GPU-backed LVGL adapters can emit the same
texture/mesh records. Software LVGL/J2ME ports can upload strided RGB565 dirty
rectangles.
2D/3D engine backends can translate a render queue into one validated GLES2
command batch per frame rather than introducing another device ABI. See
[graphics documentation](../docs/graphics.md) for the format and
host-composition contract.
