# OOS Native-App SDK

OOS application interfaces are defined in `wit/oos.wit`. That versioned WIT
package is the public contract; applications must not declare WAMR imports or
export lifecycle symbols by hand.

- `rust/oos-app` runs `wit-bindgen` for the `oos:platform/app@0.1.0` world and
  provides small Rust convenience wrappers.
- `rust/oos-egui` converts egui textures and meshes into the generated WIT
  graphics records.
- `core/include/oos/runtime/graphics_types.h` is the renderer's internal
  canonical-layout mirror. It is deliberately outside the SDK.

The world imports runtime, graphics, device discovery, audio, camera, power,
vibration, Wi-Fi/IP, Bluetooth, modem, and codec interfaces, and exports the
application lifecycle interface. `wit-bindgen` dead-strips unused core-Wasm
imports, so an application pays only for the capabilities it calls.

Rust applications implement `oos_app::App` and use the generated export macro:

```rust
struct App;

impl oos_app::App for App {
    fn init() -> Result<(), oos_app::ErrorCode> { Ok(()) }
    fn event(_: oos_app::KeyEvent) {}
    fn frame(_: u64) -> Result<(), oos_app::ErrorCode> { Ok(()) }
    fn shutdown() {}
}

oos_app::bindings::export!(App with_types_in oos_app::bindings);
```

Other languages should generate guest bindings from `wit/oos.wit` and target
the `app` world. The runtime contract contains no Rust-specific types, WASI,
EGL, framebuffer, Binder, evdev, file descriptor, or HAL object.

The production WAMR 2.4.4 runtime loads the generated Canonical ABI core Wasm
or ARMv7 AOT file. `build-native-apps.sh` also wraps the same module as a real
Component Model artifact for runtimes that support components. WAMR 2.4.4 does
not itself load component binaries.

Framework adapters belong under `sdk/`, not in an individual application. An
iced adapter can emit the same texture/mesh records. An LVGL adapter can use
bindings generated from the same WIT rather than introducing another device
ABI.
