# Settings Application

`apps/settings` is the built-in LVGL Settings application. It uses the same
palette, typography, flat rows, reserved selection border, and softkey layout
as Launcher. The host owns its lifecycle and switches between Launcher and
Settings without coupling either application to the other's implementation.

The root list is divided into Network, Apps, and Device sections:

- Wi-Fi provides radio on/off, access-point scanning, Open and WPA-PSK
  connection, reconnecting saved networks, disconnect, and confirmed forget.
  Association and DHCP run off the UI thread so navigation and animation stay
  responsive. Bluetooth and SIM manager remain explicit placeholder pages.
- Manage apps lists built-in and installed applications. Package applications
  expose metadata and a confirmed uninstall action; built-in applications are
  read-only.
- Storage reads total, used, and available bytes from the persistent data
  volume. Device information reports the active platform descriptor.
- Status bar toggles the clock, network indicators, and numeric battery level.
  Values are atomically stored in `/data/system/status-bar.json` and observed
  by SystemUI in the same frame loop.

## Retained navigation

Settings uses a retained page stack instead of rebuilding the previous route
when Back is pressed. Opening a child page hides the parent's LVGL content tree
and stores its complete page runtime: row model, selected object, scroll offset,
input state, page-specific list caches, transient notification, and softkey
labels. Back deletes only the current child and restores the exact parent object
tree and runtime snapshot.

Hardware operations and repository mutations are application-level state. A
completed scan, connection change, preference write, or uninstall explicitly
rebuilds the affected current page while retaining its selection and scroll
offset. Ordinary navigation never reloads data. History is bounded naturally by
the Settings route depth, and all retained trees are released together when the
application shuts down.

Preview Settings directly on the local device with:

```sh
make local-rootfs
./scripts/run-local-rootfs.sh --builtin settings
```
