# System UI And Application Surfaces

OOS keeps mechanisms, framework backends, and applications in separate source
trees:

```text
system/  hardware contracts, runtime, compositor, input routing, storage
sdk/     public WIT/Rust SDK plus reusable egui, LVGL, and ImGui backends
apps/    one directory per application
```

The production shell currently compiles two trusted applications into the OOS
process. `apps/launcher` owns the normal application surface and preserves the
240x298 LVGL phone UI. `apps/systemui` owns only global UI: the 22-pixel status
bar and the full content-area overlay used by notifications and the lock
screen. Neither application is implemented inside `system/`.

The compositor establishes the following fixed stacking order on a 240x320
device:

| Layer | Bounds | Z | Owner |
| --- | --- | ---: | --- |
| Application | `0,22 240x298` | 0 | Foreground app or Launcher |
| System overlay | `0,22 240x298` | 100 | SystemUI |
| Status bar | `0,0 240x22` | 200 | SystemUI |

Each layer receives its own `GraphicsHost`. Texture handles are translated to
compositor-owned handles, retained draw lists are clipped and offset into
physical display coordinates, and the physical display receives one merged
submission. Input reaches SystemUI first; an active modal overlay consumes it,
otherwise it is forwarded to the foreground application.

Adding an application means adding a new directory below `apps/` and depending
on the appropriate root SDK backend. Shared UI framework integration must be
added to `sdk/`, never copied into an application or placed in `system/`.
Rust/Wasm applications are independent Cargo projects; there is no root Cargo
workspace that must be updated when an application is added.
