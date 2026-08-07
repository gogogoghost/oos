# OOS Applications

Every directory here is an application package. Launcher, Settings, SystemUI,
and the examples under `tests/` use the same manifest and ZIP format and are
installed through the same repository. Launcher, Settings, and SystemUI retain
their C++/LVGL implementations; their package scripts compile those sources as
Wasm guests that call the platform through WIT.

An application owns its `manifest.json` and `package.sh`. There is no
repository-level `apps/CMakeLists.txt`. Intermediate files stay in the
application's `build/` or `target/` directory and the final package is always
`dist/application.zip`, containing `manifest.json` plus its JS/Wasm entry and
optional modules/assets.

System behavior is granted by permissions, not by a separate package kind or
UI mode. SystemUI receives the `system-ui` permission, Settings receives the
management permissions it needs, and ordinary applications use the same
runtime and lifecycle. A JS entry may use Solid or a canvas; a Wasm entry may
use Clay, LVGL, egui, or raw graphics without declaring a UI category.
