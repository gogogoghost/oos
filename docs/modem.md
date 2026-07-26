# Modem and Telephony

The Nokia 2780 modem implementation is a native C++ Radio HAL client intended
for the future long-running OOS process. It uses the standard Android
`android.hardware.radio@1.0::IRadio/slot1` HIDL interface exposed by the stock
Qualcomm `qcrild`; it does not depend on Gecko, B2G, `api-daemon`, or direct
access to Qualcomm diagnostic and SMD device nodes.

## Architecture

`oos::modem::ModemManager` owns the Radio HAL proxy, response callback,
indication callback, request serials, and binder callback thread pool. The
public header contains only C++ value types, so the production application does
not need to expose HIDL types outside the Nokia 2780 adapter.

The current reusable API provides:

- Radio HAL discovery and callback registration for a named slot;
- a read-only snapshot of SIM state, baseband version, device identity, signal
  strength, voice/data registration, operator, radio technology, current calls,
  active data calls, preferred network type, and radio capability;
- per-request timeout and Radio HAL error reporting;
- an explicit radio-power method for later lifecycle integration. The smoke
  test never calls it.

The HIDL 1.0 contract is the compatibility baseline because the device exposes
Radio 1.0 through 1.4 and the 1.0 methods are sufficient for initial bring-up.
Production support should add the 1.2 and 1.4 response/indication interfaces
before relying on richer LTE signal, cell identity, and data-call structures.

## Nokia 2780 Validation

Run the no-SIM, read-only test with:

```sh
./scripts/test-modem.sh smoke
```

It builds and pushes `/data/local/tmp/oos-modem-test`, queries the HAL, and then
checks that `vendor.qcrild` is still running and the peripheral modem remains
`ONLINE`. It does not change radio power, network selection, calls, SMS, APNs,
or packet-data state. The diagnostic CLI masks device identifiers in terminal
output; production code must treat the raw API fields as private data.

The July 2026 Nokia 2780 no-SIM test validated an online Qualcomm baseband,
Radio state `ON`, card state `ABSENT`, emergency network search, current signal
structures, empty call/data-call lists, preferred network type, and radio
capability. Twelve of thirteen requests succeeded. `getHardwareConfig` returned
the HAL's expected `REQUEST_NOT_SUPPORTED`; no request timed out. Repeated runs
left qcrild running and the modem online.

The diagnostic binary also contains an intentionally explicit mutation command:

```sh
adb shell "su -c '/data/local/tmp/oos-modem-test power on'"
adb shell "su -c '/data/local/tmp/oos-modem-test power off'"
```

Do not use `power off` remotely unless another recovery path is available. OOS
must centralize radio power ownership so UI, suspend, airplane mode, and
emergency-call policy cannot race each other.

## SIM-Dependent Work

The current device has no SIM, so the following daily-use functionality remains
unimplemented or unvalidated rather than being inferred from successful HAL
transport:

- SIM insertion/removal, PIN/PUK and network lock, IMSI/ICCID, SIM contacts,
  voicemail records, and SIM Toolkit sessions;
- automatic/manual network selection, roaming policy, emergency registration,
  cell information, NITZ time, and loss/recovery behavior;
- outgoing, incoming, held, multiparty, emergency, and supplementary-service
  calls, plus audio routing and IMS/VoLTE integration;
- SMS submit/delivery reports, incoming GSM/CDMA SMS, SIM storage, SMSC, and
  cell broadcast;
- APN and data-profile provisioning, PDP activation/deactivation, IPv4/IPv6
  interface/route/DNS setup through netmgr/netd, roaming data, tethering, and
  recovery after modem restart;
- USSD/MMI, persistent unsolicited-event state, death-recipient reconnection,
  request cancellation, wake-lock acknowledgement across every indication,
  and multi-SIM policy.

These features should be implemented against Radio HIDL and the existing
Qualcomm `netmgrd`/IMS services, then tested with a disposable SIM and a known
carrier account. Raw `/dev/smd*`, DIAG, and QMI endpoints are not an appropriate
first integration layer: qcrild already owns them and exposes the supported
vendor behavior through HIDL.

Nokia 8110 support remains a separate device adapter. Its older RIL generation,
service names, binder transport, IMS stack, and data-network integration must
be audited from that device's stock image before sharing the Nokia 2780 backend.
