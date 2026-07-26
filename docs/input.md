# Key Input

OOS reads physical keys through the Linux evdev API. The reusable
`oos_input` library is independent of WPE and device display code. It scans
`/dev/input/event*`, queries each node with `EVIOCGBIT`, and watches every
device that advertises `EV_KEY` through one epoll descriptor.

Do not identify hardware by an `eventN` number in production code. Those
numbers are probe-order dependent. `KeyInput::initialize()` discovers the
current mapping and exposes the kernel path and device name with every event.

## Public API

Include `oos/input/key_input.h` and link `oos_input`. A long-running caller
owns one `oos::input::KeyInput` instance:

1. Call `initialize()` after `/dev` is available in the OOS rootfs.
2. Register `fileDescriptor()` with the native event loop and call `poll(0)`
   when it is readable, or call blocking `poll()` from a dedicated thread.
3. Handle `Pressed`, `Released`, and `Repeated` without assuming that every
   key generates repeats.
4. Let the destructor release descriptors, or call `shutdown()` explicitly.

`KeyInputOptions::grab_devices` requests `EVIOCGRAB` for each discovered key
device. Enable it only when OOS has taken ownership from B2G. The diagnostic
test leaves grabbing disabled so it does not interfere with other processes.

## Nokia 2780 Flip

The stock kernel currently reports:

| Kernel name | Observed node | Controls |
| --- | --- | --- |
| `matrix_keypad` | `/dev/input/event1` | digits, D-pad, menu, back, call, OK, option, info, star, pound |
| `qpnp_pon` | `/dev/input/event0` | power, volume down |
| `soc:gpio_keys` | `/dev/input/event3` | volume up, programmable key |
| `hall_sensor` | `/dev/input/event2` | flip state as key code 249 |
| `msm8952-snd-card-mtp Button Jack` | `/dev/input/event5` | headset media and volume buttons |

The observed node column is diagnostic information, not an API guarantee.
The headset jack's separate `event4` node exposes only `EV_SW` and is
intentionally excluded from key capture.

## On-device Test

Build and start the WPE visualizer:

```sh
./scripts/configure-android.sh nokia-2780-flip
cmake --build build/android-nokia-2780-flip -j8 \
  --target oos_test_nokia_2780_key_input
./scripts/test-key-input.sh
```

The primary display shows the numeric Linux key code, symbolic name,
pressed/released/repeated state, kernel device name, and current event path.
The same information is written to `/data/local/tmp/oos-wpe/hello.log`.

Stop the test with:

```sh
./scripts/run-wpe-chroot.sh stop
```
