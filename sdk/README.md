# OOS Native-App SDK

The SDK exposes capabilities instead of Android or Linux device access.
Applications compile to `wasm32-unknown-unknown` and import the versioned
`oos` module. They do not receive WASI, EGL, framebuffer, Binder, evdev, or
filesystem access.

- `include/oos/wasm/abi.h`: language-neutral ABI layout and limits.
- `rust/oos-app`: low-level Rust bindings.
- `rust/oos-egui`: reusable egui renderer adapter used by Launcher.

Framework adapters belong under `sdk/`, not in an individual application.
The planned iced adapter will emit the same texture/mesh command stream. The
planned LVGL adapter will expose a C-facing display/input port and translate
LVGL output into bounded OOS graphics submissions.
