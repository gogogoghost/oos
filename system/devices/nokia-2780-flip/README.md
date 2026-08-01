# Nokia 2780 Flip

This target runs on the Nokia 2780 Flip's KaiOS 3 / Android API 29 base and
uses the stock vendor graphics services. The shared runtime contains no
Nokia-specific control flow; it obtains hardware through
`oos::device::Device` and the standard manager interfaces.

## Display

`oos_nokia_2780_display` owns panel power and switching:

- `nokia2780_prepare_primary()` prepares HWC2 and disables fb1.
- `nokia2780_show_cover_rgb565()` switches to the cover and keeps fb1 open.
- `nokia2780_show_cover_rgb565_after_primary_off()` and
  `nokia2780_hide_cover()` support a long-lived process with one HWC client.

The primary display is 240x320 RGB565. The OOS compositor renders native WAMR
application commands through GLES into the HWC2 client target. A black preroll
and settled first submission prevent uninitialized panel memory from becoming
visible.

The cover exposes 128x160 visible RGB565 pixels through fb1 with a 128x320
virtual framebuffer. The driver requires the descriptor to remain open while
content is visible. The stock property `ro.kaios.display.ext_fb_dev=fb1` does
not represent an HWC external display or GPU zero-copy path.

Keeping fb0 and fb1 active together causes the primary panel ESD check to reset
the panel, so the public manager enforces mutual exclusion. Validated
single-process transitions take approximately 622-637 ms primary-to-cover and
439 ms cover-to-primary. True simultaneous output requires kernel or vendor
HWC support.

## Other Backends

- Input discovers every `EV_KEY` node by capability and covers the keypad,
  power/volume controls, headset buttons, and flip hall sensor.
- Modem uses the stock `IRadio/slot1` HIDL service through the common
  `oos::modem::ModemManager` contract.
- Hardware services provide PCM audio, vibration, battery/flip state, power
  lifecycle, HAL1 still capture, torch, and Qualcomm H.264 probes.
- Network services provide the shared Wi-Fi, IPv4, Bluetooth, and BLE APIs.

See `docs/input.md`, `docs/modem.md`, `docs/hardware.md`, and
`docs/network.md` for the device-independent contracts.

## Tests

Device tests live under `tests` and are never production dependencies:

- `oos_test_nokia_2780_primary_gl`: EGL/HWC animation.
- `oos_test_nokia_2780_cover_secondary`: labeled cover fixture.
- `oos_test_nokia_2780_cover_green`: solid RGB565 cover fixture.
- `oos_test_nokia_2780_modem_headless`: read-only radio/baseband probe.
- `oos_test_nokia_2780_hardware_headless`: audio, vibration, power, camera,
  flash, and codec probe.
- `oos_test_nokia_2780_key_input`: timed headless evdev event logger.
- `oos_test_nokia_2780_device_contract`: descriptor/capability contract.
- `oos_test_nokia_2780_service_contract`: common Manager link contract.

The production `oos` target itself is the integration test for boot splash,
WAMR/egui Launcher composition, and key forwarding.
