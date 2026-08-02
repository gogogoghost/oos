# System UI And Application Surfaces

OOS keeps mechanisms, framework backends, and applications in separate source
trees:

```text
system/  hardware contracts, runtime, compositor, input routing, storage
sdk/     public WIT/Rust SDK plus reusable egui, LVGL, and ImGui backends
apps/    one directory per application
```

The production shell currently compiles three trusted applications into the
OOS process. `apps/launcher` and `apps/settings` each retain an independent
240x298 application surface. The generic application session manager makes
only the foreground surface visible. `apps/systemui` owns only global UI: the
22-pixel status bar and the full content-area overlay used by notifications and
the lock screen. None of these applications is implemented inside `system/`.

System-owned LVGL and ImGui surfaces use the platform system font. On Android,
Roboto is the primary face and DroidSansFallback supplies missing CJK glyphs
on demand. FontAwesome is embedded only as a small subset containing OOS icon
codepoints; framework text fonts are not embedded in the runtime.

The compositor establishes the following fixed stacking order on a 240x320
device:

| Layer | Bounds | Z | Owner |
| --- | --- | ---: | --- |
| Application sessions | `0,22 240x298` | 0 | One layer per resident app; one visible |
| System overlay | `0,22 240x298` | 100 | SystemUI |
| Status bar | `0,0 240x22` | 200 | SystemUI |

Each session layer receives its own `GraphicsHost`. Texture handles are translated to
compositor-owned handles, retained draw lists are clipped and offset into
physical display coordinates, and the physical display receives one merged
submission. Input reaches SystemUI first; an active modal overlay consumes it,
otherwise it is forwarded through the session manager to the foreground
application. Hidden sessions receive neither input nor frame callbacks.

## Immersive Status Bar

Every application session retains a status-bar background color and light/dark
icon theme. SystemUI applies only the active session's appearance to the status
root, clock, cellular bars, radio label, Wi-Fi, charging, and battery elements.
Switching applications restores the target session's last value; a background
application cannot restyle the foreground status bar.

Launcher uses the top-level canvas color. Settings applies its AppBar surface
color whenever a page is shown, so the 22-pixel SystemUI layer visually
continues the application surface instead of forming a separate strip.

Adding an application means adding a new directory below `apps/` and depending
on the appropriate root SDK backend. Shared UI framework integration must be
added to `sdk/`, never copied into an application or placed in `system/`.
Rust/Wasm applications are independent Cargo projects; there is no root Cargo
workspace that must be updated when an application is added.
