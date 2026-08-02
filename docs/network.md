# Wi-Fi and Bluetooth

The networking implementation is a device-independent native C++ layer for
the long-running OOS process. It deliberately avoids Gecko and talks to the
stock services shipped on each target.

## Architecture

Wi-Fi commands use the `wlan0` Unix datagram control socket exposed by the
stock `wpa_supplicant`. `IpManager` controls `dhcpcd_wlan0` through Android init
properties and uses the stock `libnetutils.so` ABI for static IPv4 setup. The
Wi-Fi credential is sent only to supplicant and is never printed by the test
program.

Bluetooth uses the B2G daemon architecture. OOS creates an abstract Unix
`SOCK_SEQPACKET` listener, starts the service selected by
`Device::services()`, accepts its command and notification channels, then
registers the required Bluedroid modules. The implementation owns that daemon
instance and returns the adapter to the off state on shutdown. Nokia 2780 uses
the scanner-based `bluetoothd_socket1` protocol; Nokia 8110 uses the older
b2g48 GATT-client protocol exposed by `bluetoothd`.

The public APIs are:

- `oos::network::WifiManager`: status, scan, saved networks, open/WPA-PSK
  connection, select, disconnect/reconnect, forget, and save.
- `oos::device::ServiceProvider`: device-neutral Wi-Fi radio lifecycle and the
  manager operations consumed by Settings. Nokia 8110 uses the stock
  `libhardware_legacy.so` driver/supplicant lifecycle exported for Gecko;
  Android 10 uses `cmd wifi`. Both wait for the configured supplicant control
  endpoint before exposing the manager.
- `oos::network::IpManager`: current IPv4 route/DNS, DHCP, and static IPv4.
- `oos::network::BluetoothManager`: adapter lifecycle, classic discovery,
  pairing commands, HID/HFP/A2DP profile commands, BLE scanning, and a direct
  GATT connection lifecycle.

## Nokia 2780 Validation

Run the non-interactive recovery-safe test:

```sh
./scripts/test-network.sh smoke
```

It builds and pushes `/data/local/tmp/oos-network-test`, checks Wi-Fi state,
saved networks, IPv4 configuration and scan results, performs a disconnect and
reconnect cycle, applies the current address through the static-IP path and
restores DHCP, then runs classic and BLE discovery. It does not add or remove
networks, pair devices, or connect to arbitrary nearby Bluetooth devices.

The July 2026 Nokia 2780 tests completed with the current `YYY` association and
`10.0.0.100/24` address preserved. Repeated runs found five to eleven access
points, one to three classic Bluetooth devices, and eight to eleven BLE
advertisers. After cleanup,
`init.svc.bluetoothd_socket1` was `stopped` and `bluetooth.isEnabled` was
`false`.

Commands that require credentials or a tester-owned peer are intentionally
manual:

```sh
adb shell "su -c '/data/local/tmp/oos-network-test wifi connect SSID wpa PSK'"
adb shell "su -c '/data/local/tmp/oos-network-test ip static 10.0.0.50 24 10.0.0.1 10.0.0.1'"
adb shell "su -c '/data/local/tmp/oos-network-test ip dhcp'"
adb shell "su -c '/data/local/tmp/oos-network-test bluetooth pair AA:BB:CC:DD:EE:FF le'"
adb shell "su -c '/data/local/tmp/oos-network-test bluetooth profile-cycle AA:BB:CC:DD:EE:FF a2dp 5'"
adb shell "su -c '/data/local/tmp/oos-network-test bluetooth gatt-cycle AA:BB:CC:DD:EE:FF 5'"
```

The static-IP test interrupts DHCP. Always run `ip dhcp` after an experiment
unless the static configuration is intended to remain active. Supplying secrets
on a command line exposes them to process inspection and shell history; the
production UI must pass credentials through an in-process API instead.

## Required Daily-Use Surface

Settings now covers the daily Open/WPA-PSK Wi-Fi lifecycle: radio power,
scanning, connection, saved-network selection, disconnect, forget, and DHCP.
The layer is not yet a complete KaiOS connectivity replacement. Production OOS
still needs:

- asynchronous association/authentication events, retry policy, captive portal
  detection, persisted network priority, WPS, enterprise EAP, Passpoint, power
  saving, roaming, hotspot and tethering;
- netd resolver programming for per-network DNS and IPv6 provisioning instead
  of relying only on legacy Android properties;
- interactive Bluetooth PIN and SSP confirmation, durable bond state, local
  name/discoverability, remote service discovery, and reconnect policy;
- complete profile state machines and routing for HFP calls, A2DP/AVRCP audio,
  HID input, PAN networking, OBEX/file transfer, and MAP messages;
- complete GATT service discovery, characteristic/descriptor read and write,
  notifications, MTU handling, BLE advertising, and peripheral/server mode.

The raw request methods are synchronous only for daemon command acceptance.
Actual association, bond, profile, and GATT states arrive asynchronously on the
notification socket. The production OOS event loop must own one
`BluetoothManager` for its full lifetime and turn those notifications into a
persistent state model; short-lived CLI commands are only diagnostics.

## Nokia 8110 Validation

The Android 6 adapter uses `/data/misc/wifi/sockets/wlan0`, legacy
`dhcpcd_wlan0` service control, and `bluetoothd`. Wi-Fi status, scans,
disconnect/reconnect, DHCP/static-IP restoration, and classic discovery passed.
The corrected legacy BLE lifecycle registered a GATT client, discovered seven
advertisers in a five-second sample, stopped scanning, unregistered the client,
and stopped `bluetoothd` cleanly.

Shared tests resolve all endpoint differences through `oos/device/services.h`;
they no longer require an `OOS_BLUETOOTH_SERVICE` override.
