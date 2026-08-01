# Key Input

OOS reads physical keys through Linux evdev. The reusable `oos_input` library
scans `/dev/input/event*`, queries `EVIOCGBIT`, and watches all devices that
advertise `EV_KEY` through one epoll descriptor.

Production code must not identify hardware by an `eventN` number because probe
order changes. `KeyInput::initialize()` discovers the current mapping and each
event includes the kernel path and device name.

## Public API

Include `oos/input/key_input.h` and link `oos_input`:

1. Initialize after `/dev` is mounted into the OOS rootfs.
2. Register `fileDescriptor()` with the event loop and call `poll(0)` when
   readable, or use blocking `poll()` on a dedicated thread.
3. Handle pressed, released, and repeated events independently.
4. Call `shutdown()` or let the owning instance release all descriptors.

`KeyInputOptions::grab_devices` requests `EVIOCGRAB`. Production OOS enables
exclusive ownership after B2G stops; diagnostics should leave it disabled.

## Device Topology

The Nokia 2780 currently exposes a matrix keypad, `qpnp_pon`, GPIO keys, a flip
hall sensor (observed code 249), and headset button nodes. The Nokia 8110 uses a
matrix keypad, two slider hall sources, `qpnp_pon`, GPIO keys, and separate
headset button/switch nodes. Capability discovery, not these observed names,
is the API contract.

Both phones use the same normalized OOS key ABI. The WAMR host forwards events
to the active native guest through WIT. The local backend implements the same
allocation-free `KeyInputSource` contract over SDL and reads its mapping from
`system/devices/local/config/keymap.conf`.

Both device builds expose the same headless diagnostic source through their
device-specific target:

```sh
cmake --build build/android-nokia-2780-flip -j24 \
  --target oos_test_nokia_2780_key_input
cmake --build build/android-nokia-8110-4g -j24 \
  --target oos_test_nokia_8110_key_input
```

On both devices, the built-in LVGL SystemUI is the end-to-end validation for
evdev discovery, normalized WIT delivery, and UI focus handling.
