# KaiOS API Coverage

This audit uses the 34 API families in the official KaiOS Smart Feature Phone
3.0 device API index as its countable baseline. Standard DOM, CSS, ECMAScript,
and media interfaces are not counted individually. KaiOS 2.5-only deltas are
listed separately because the legacy documentation and permission table do not
define one stable, non-overlapping API index.

Sources:

- <https://developer.kaiostech.com/docs/sfp-3.0/api/web-apis/>
- <https://developer.kaiostech.com/docs/api/web-apis>
- <https://developer.kaiostech.com/docs/getting-started/main-concepts/permissions/>

## Summary

| Classification | Families | Meaning |
| --- | ---: | --- |
| WPE engine API | 7 | Compiled into the shared WPE profile; hardware/provider behavior still needs device validation |
| OOS host API, partial | 1 | `GetDeviceStorage` crosses the private control socket and uses the shared host service |
| Non-functional compatibility shell | 21 | An object/name exists but returns empty data, `null`, or `NotSupportedError` |
| Absent | 5 | The documented family is not exposed at its KaiOS 3 entry point |

There are therefore **26 of 34 families without a functional implementation**.
There is not yet a fully implemented privileged KaiOS family: DeviceStorage is
the first partial one. It now implements 10 of its 16 documented methods:
`add`, `addNamed`, `appendNamed`, `delete`, `get`, `enumerate`, `available`,
`storageStatus`, `freeSpace`, and `usedSpace`. `enumerateEditable` still returns
read-only `File` objects. `getEditable`, `getRoot`, `format`, `mount`, and
`unmount` remain unsupported. Change events currently cover mutations initiated
by the same bridge, not changes made by another process.

## KaiOS 3 Matrix

| API family | Current state | Shared host/WIT work |
| --- | --- | --- |
| AlarmManager | Empty shell | Add persistent alarms WIT plus RTC scheduler and SystemMessage delivery |
| AppsManager | Global legacy shell; v3 entry point incomplete | Add app registry/launch WIT and permission checks |
| Audio Channels API | Metadata shell | Connect WPE policy to the existing `audio` WIT service |
| AudioChannelClient | Empty shell | Extend `audio` WIT with focus/channel ownership |
| AudioChannelManager | `volumeControlChannel` only | Extend `audio` WIT with volume/focus events |
| AudioContext | WPE engine | Validate GStreamer audio sink on every device |
| AudioVolumeManager | Absent | Add volume methods/events to `audio` WIT |
| Bluetooth | Disabled shell | Connect to existing `bluetooth` WIT provider and add async events |
| Cameras | Empty list shell | Connect to existing `camera` WIT provider and media streams |
| Contacts | Empty legacy shell; v3 entry point incomplete | Add permission-scoped contacts WIT and persistent store |
| DataCallManager | Empty shell | Extend `modem`/`ip` WIT with data-call lifecycle |
| DeviceCapability | Always false/undefined | Map manifest permissions and `device` WIT capabilities |
| Externalapi | Empty shell | Define supported activity/vendor operations before adding WIT |
| FmRadio | Disabled shell | Add FM radio WIT behind the existing device feature bit |
| Geolocation | WPE engine | Supply and validate a host geolocation provider |
| GetDeviceStorage | Host-backed, partial | Finish editable handles, external change events, and permission modes |
| GetUserMedia | WPE engine | Bind camera/microphone providers and prompts to `camera`/`audio` WIT policy |
| HTMLMediaElement | WPE engine | Validate codecs, audio routing, visibility, and suspend behavior |
| InputMethod | Empty shell | Add focused-input/event protocol; WIT required for non-Web peers |
| MobileConnection | Empty shell | Connect to `modem` WIT and add registration/SIM events |
| MozSpeakerManager | Absent | Add route/speaker selection to `audio` WIT |
| MozTCPSocket | `open()` returns null | Prefer capability-scoped WASI sockets or add asynchronous socket WIT |
| Notification | WPE engine | Add OOS notification UI, persistence, click, and system-message delivery |
| PowerManager | Fake in-page locks | Connect to existing `power` WIT provider with per-app lock ownership |
| powerSupplyManager | Legacy shell; v3 entry point incomplete | Map battery/charger events to `power` WIT |
| ServiceWorker | WPE engine | Validate restart, offline cache, quota, and background policy |
| Settings | Empty in-memory shell | Add typed, permission-scoped settings WIT and observer events |
| SystemMessage | Handler registry only; never dispatches | Add host event queue shared by WPE and WAMR |
| TcpSocket | Absent | Share the selected socket contract with MozTCPSocket |
| Telephony | Absent | Add call-control/event WIT over the modem service |
| TimeService | Absent | Add privileged clock/timezone WIT and policy |
| VirtualCursor | Disabled shell | Implement in the input/compositor layer; expose controls through WIT if needed |
| WebActivity | Explicit `NotSupportedError` | Add handler resolution/launch/result WIT over the app registry |
| XMLHttpRequest | WPE engine | Validate TLS roots, proxy policy, and offline errors |

## KaiOS 2.5 Delta

The legacy profile additionally needs explicit coverage for DataStore,
NetworkStats, NFC, secure-element channels, SpeedDial, VolumeManager, Wi-Fi
information/management, WebSMS, voicemail, cell broadcast, BatteryManager, and
wake locks. Some names currently exist as shells and some have partial overlap
with existing OOS WIT interfaces, but none of these legacy privileged families
is end-to-end complete.

## Implementation Order

1. Build a permission-aware host event channel. Alarms, notifications,
   telephony, settings observers, storage changes, and activities all depend on
   reliable asynchronous delivery and application wake-up.
2. Connect existing host services to both adapters: `power`, `vibrator`,
   `audio`, `camera`, `wifi`, `ip`, `bluetooth`, and `modem`. Their WIT shapes
   already exist, but production WAMR currently has no per-app service provider
   and WPE still exposes shells.
3. Add new shared WIT contracts for alarms, settings, contacts, applications /
   activities, notifications, and telephony. Implement each once in the host,
   then add thin WPE and WAMR adapters.
4. Finish lower-priority or hardware-specific APIs: FM radio, NFC/SE,
   NetworkStats, input method, virtual cursor, and vendor Externalapi methods.

Every new privileged operation must terminate in an OOS host service. WPE may
adapt DOMRequest/Promise/event semantics and WAMR may lower Canonical ABI data,
but neither runtime should own a second device implementation.
