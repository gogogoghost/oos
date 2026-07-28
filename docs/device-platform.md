# Device Platform Contract

OOS separates device-independent policy from stock Android transport details.
The production process includes only `oos/device/device.h` and
`oos/device/display.h`; it does not include Nokia, HWC, HIDL, Binder, or vendor
HAL headers.

## Layers

The platform boundary has three layers:

1. `oos::device::Device` identifies the handset, reports capabilities and
   service endpoints, and owns boot-critical display and key-input lifecycle.
2. The public Manager APIs under `system/src/oos/{hardware,network,modem}`
   define typed operations shared by every target.
3. A device directory supplies the display backend and any Manager
   implementations that differ from the reusable core implementation.

The selected device build exports the same CMake targets:

- `oos::platform`: `Device`, primary `Display`, and `KeyInput`;
- `oos::hardware`: audio, camera, power, and vibration backends;
- `oos::network`: Wi-Fi, IP, and Bluetooth backends;
- `oos::modem`: cellular modem backend.

Device-specific target names remain available for diagnostics, but production
and shared tests should link the standard aliases.

## Capabilities

`Device::capability()` never uses a Boolean because implementation and
validation are different facts:

- `unsupported`: the handset does not expose the feature;
- `planned`: the capability slot is reserved, but OOS has no usable backend;
- `implemented`: code exists, but the complete device lifecycle is not yet
  validated;
- `validated`: the current backend passed an on-device data-path test.

Callers may use only `implemented` and `validated` capabilities. Tests and
release manifests should preserve the distinction so an untested path cannot
silently become a production dependency.

## Standard Service Startup

Managers whose Android endpoint differs by handset use
`oos/device/services.h`. For example:

```cpp
auto device = oos::device::createDevice();

auto wifi = oos::device::createWifiManager(*device);
if (!oos::device::initializeService(*device, wifi)) {
  // Report wifi.lastError().
}

oos::network::BluetoothManager bluetooth;
if (!oos::device::initializeService(*device, bluetooth)) {
  // Nokia 2780 selects bluetoothd_socket1; Nokia 8110 selects bluetoothd.
}
```

The same helpers cover camera, power, vibration, and modem initialization.
Audio methods currently open short-lived streams directly. Hardware services
must remain lazy: reporting support for a service must not load its camera,
Radio, or media dependency graph during launcher startup.

## Current Matrix

| Feature | Nokia 2780 Flip | Nokia 8110 4G | Local |
| --- | --- | --- | --- |
| Primary display | validated, HWC2/HIDL | validated, HWC1 | validated, SDL/llvmpipe GLES |
| Secondary display | validated, mutually exclusive fb1 | unsupported | unsupported |
| Key input | validated evdev | validated evdev | validated, configured SDL map |
| Speaker/microphone | validated AAudio | validated OpenSL ES | deterministic mock |
| Camera/torch | validated HAL1 over HIDL | validated direct HAL1/sysfs | deterministic mock |
| Vibration | validated HIDL | validated legacy HAL | deterministic mock |
| Battery/wake locks | validated | validated | deterministic mock |
| Deep suspend | implemented, unplugged cycle pending | implemented, unplugged cycle pending | deterministic mock |
| RTC wake | validated | implemented, final lifecycle pending | deterministic mock |
| Wi-Fi/static IP | validated | validated | deterministic mock |
| Classic Bluetooth/BLE scan | validated | validated, b2g48 daemon protocol | deterministic mock |
| Modem read-only snapshot | validated Radio HIDL | validated RIL socket | deterministic mock |
| Hardware H.264 codec | validated | planned | deterministic mock |
| Location/sensors/FM | planned | planned | unsupported |
| NFC | unsupported | unsupported | unsupported |

The source of truth is each backend's `Device::capability()` implementation;
this table documents the current state rather than replacing runtime checks.

## Adding A Device

A new target must:

1. Add an isolated `system/devices/<device-id>` directory and a separate extracted
   system path in `.env`; never overwrite another target's sysroot setting.
2. Implement `oos::device::createDevice()` and `oos::device::Display` without
   exposing vendor types in public headers.
3. Populate `DeviceDescriptor`, `ServiceConfiguration`, and every capability
   state conservatively.
4. Reuse a core Manager implementation when its protocol matches. Otherwise,
   implement the same public class API in the device directory.
5. Export `oos::platform`, `oos::hardware`, `oos::network`, and `oos::modem`
   aliases as applicable.
6. Build and run `device_contract_test.cpp` and `service_contract_test.cpp`,
   then run feature data-path tests before changing a capability to
   `validated`.

Run both non-mutating contracts on the connected target with:

```sh
./scripts/test-device-contract.sh nokia-2780-flip
./scripts/test-device-contract.sh nokia-8110-4g
```

The script supports both a regular ADB shell with `su` and an already-root ADB
shell.

Exploratory probes stay under the device's `tests` directory. Once a path is
validated, reusable logic moves into the backend library and shared behavior
moves under `system/tests/device`.
