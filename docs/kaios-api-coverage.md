# KaiOS API Coverage

This matrix uses the 34 families in the official KaiOS 3.0 device API index
and the 22 entries in the KaiOS 2.5 index. It describes the OOS bridge, not
standard Web APIs that WPE already supplies.

Sources:

- <https://developer.kaiostech.com/docs/sfp-3.0/api/web-apis/>
- <https://developer.kaiostech.com/docs/api/web-apis>
- <https://developer.kaiostech.com/docs/getting-started/main-concepts/permissions/>

## Safety Policy

KaiOS Web applications cannot change Wi-Fi, Bluetooth, or mobile-network
state. Their version-correct objects are injected when the manifest grants the
permission, but every method and writable state property returns or throws
`NotSupportedError`. The host independently rejects all WPE Wi-Fi, IP,
Bluetooth, modem snapshot, and radio-power calls with `ENOTSUP` before a device
provider is created. The native WIT device interfaces remain available only to
trusted OOS applications with their explicit permissions.

Hardware that OOS has no portable implementation for follows the same rule:
NFC, Secure Element, FM radio, raw TCP sockets, telephony, data calls, and
vendor External API expose their expected surface but never fake success.

## KaiOS 3 Matrix

| API family | OOS status |
| --- | --- |
| AlarmManager | Persistent alarms, list/add/remove, and queued `alarm` system messages |
| AppsManager | Installed-app read API; install, update, enable, clear, and uninstall are explicitly unsupported |
| Audio Channels API | WPE media support plus host-managed channel policy |
| AudioChannelClient | Channel acquire/abandon requests enter the host policy store |
| AudioChannelManager | Version-correct manager surface; physical route events await SystemUI |
| AudioContext | WPE engine |
| AudioVolumeManager | Volume up/down/show requests are queued for SystemUI; no direct mixer control |
| Bluetooth | Present when granted; all operations return `NotSupportedError` |
| Cameras | Host enumeration and torch; media sessions depend on WPE `getUserMedia` providers |
| Contacts | Persistent contacts, find/cursors, groups, speed dial, ICE, and blocked-number operations |
| DataCallManager | Present when granted; all operations return `NotSupportedError` |
| DeviceCapability | Host device capability values through `lib_devicecapability` |
| Externalapi | Present when granted; all operations return `NotSupportedError` |
| FmRadio | Present when granted; all operations return `NotSupportedError` |
| Geolocation | WPE engine; device provider validation remains device-specific |
| GetDeviceStorage | Host-backed file, enumeration, mutation, and space APIs; editable handles/mount/format are unsupported |
| GetUserMedia | WPE engine and device media providers |
| HTMLMediaElement | WPE engine |
| InputMethod | Composition, key injection, selection, deletion, and focus surface plus host policy state |
| MobileConnection | Present when granted; all operations return `NotSupportedError` |
| MozSpeakerManager | Compatibility surface; direct speaker routing is not allowed |
| MozTCPSocket | Present when granted; connection creation returns `NotSupportedError` |
| Notification | Host persistence and SystemUI click/close event path; visible UI belongs to SystemUI |
| PowerManager | Read-only compatibility values; screen, reboot, power-off, and reset controls are unsupported |
| powerSupplyManager | Host battery/charger snapshot; continuous device events remain provider-specific |
| ServiceWorker | WPE engine |
| Settings | Persistent get/getBatch/set/clear and observer delivery through the host event queue |
| SystemMessage | Persistent subscription/event queue and document adapter; worker wake-up still needs engine integration |
| TcpSocket | Daemon factory is present when granted and rejects connection creation |
| Telephony | Daemon factory is present when granted and rejects call control |
| TimeService | Clock reads and managed time/timezone requests; never calls host `settimeofday` directly |
| VirtualCursor | Local key-driven cursor compatibility surface |
| WebActivity | Persistent pending/status/cancel flow; SystemUI selects a declared handler and writes the result |
| XMLHttpRequest | WPE engine |

## KaiOS 2.5 Matrix

| API | OOS status |
| --- | --- |
| Alarm | Persistent `mozAlarms` and `alarm` system-message delivery |
| AudioContext | WPE engine |
| BatteryManager | Host snapshot |
| Bluetooth | Present when granted; all operations return `NotSupportedError` |
| Clients | WPE engine |
| Data Store | Complete app-owned record/revision/cursor API; cross-app stores remain registry work |
| Device Storage | Same host service as KaiOS 3 and WIT |
| Geolocation | WPE engine/provider-dependent |
| LargeText | Managed accessibility state and lazy change subscription |
| MediaSource | WPE engine |
| mozHasPendingMessage | Backed by the persistent host event queue |
| mozSetMessageHandler | Backed by the persistent host event queue |
| Network Stats | Present when granted; all methods return `NotSupportedError` |
| NFC | Present when granted; all methods return `NotSupportedError` |
| SEChannel / SEManager / SEReader / SEResponse / SESession | `seManager` is present when granted and reader creation returns `NotSupportedError` |
| SpeedDial | Persistent contact management service |
| VolumeManager | Up/down/show requests are queued for SystemUI |
| WiFi Information | Present when granted; all methods and the `enabled` setter return `NotSupportedError` |

The bridge also supplies the common KaiOS 2.5 `mozSettings`, `mozContacts`,
`mozApps.getSelf`, `MozActivity`, wake lock, vibration, power-supply, camera,
input-method, and device-capability compatibility surfaces. Manifest permission
access modes, message subscriptions, and activity handlers are normalized and
persisted in the OOS application registry.

## Remaining Engine Work

OOS now has host ownership for every software-manageable missing family. Work
that cannot be completed by an API shim alone is deliberately visible:

1. SystemUI must render notifications, arbitrate activities, present volume and
   settings UI, and submit decisions through `system-services` WIT.
2. WPE must deliver queued SystemMessage events into terminated/restarted
   ServiceWorkers. The current document adapter does not pretend to provide
   background worker wake-up.
3. Camera/microphone, geolocation, charger, codec, and media behavior still
   needs validation on each real device while keeping the shared WPE feature
   profile identical.
4. Unsupported hardware/network families need a real OOS provider and policy
   decision before their explicit errors may be replaced.
