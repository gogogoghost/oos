# KaiOS API Coverage

This audit uses the 34 API families in the official KaiOS Smart Feature Phone
3.0 device API index as its countable baseline. Standard DOM, CSS, ECMAScript,
and media interfaces are not counted individually. The legacy 2.5 index is
counted separately using its 22 named entries; its five Secure Element
interfaces are separate entries because that is how the official index lists
them.

Sources:

- <https://developer.kaiostech.com/docs/sfp-3.0/api/web-apis/>
- <https://developer.kaiostech.com/docs/api/web-apis>
- <https://developer.kaiostech.com/docs/getting-started/main-concepts/permissions/>

## Summary

| Classification | Families | Meaning |
| --- | ---: | --- |
| WPE engine API | 7 | Compiled into the shared WPE profile; hardware/provider behavior still needs device validation |
| OOS host API, partial | 6 | Calls cross the private control socket and use the same lazy device provider as WAMR |
| Absent | 21 | No object is injected because OOS cannot yet provide the documented semantics |

There are **21 of 34 families with no implementation** and six more with a
usable but incomplete host adapter. There is not yet a fully implemented
privileged KaiOS family. DeviceStorage implements 10 of its 16 documented methods:
`add`, `addNamed`, `appendNamed`, `delete`, `get`, `enumerate`, `available`,
`storageStatus`, `freeSpace`, and `usedSpace`. `enumerateEditable` still returns
read-only `File` objects. `getEditable`, `getRoot`, `format`, `mount`, and
`unmount` remain unsupported. Change events currently cover mutations initiated
by the same bridge, not changes made by another process.

## KaiOS 3 Matrix

| API family | Current state | Shared host/WIT work |
| --- | --- | --- |
| AlarmManager | Absent | Add persistent alarms WIT plus RTC scheduler and SystemMessage delivery |
| AppsManager | Absent in v3 | Add app registry/launch WIT and permission checks; KaiOS 3 removed legacy `mozApps` |
| Audio Channels API | Absent | Connect WPE policy to the existing `audio` WIT service |
| AudioChannelClient | Absent | Extend `audio` WIT with focus/channel ownership |
| AudioChannelManager | Absent | Extend `audio` WIT with volume/focus events |
| AudioContext | WPE engine | Validate GStreamer audio sink on every device |
| AudioVolumeManager | Absent | Add volume methods/events to `audio` WIT |
| Bluetooth | Host-backed, partial | Enable/disable, classic/LE discovery, pair/unpair, and cancel use `bluetooth` WIT provider; add full adapter/events |
| Cameras | Host-backed, partial | Enumeration and torch use `camera` WIT provider; add `MediaStream` camera sessions |
| Contacts | Absent | Add permission-scoped contacts WIT and persistent store |
| DataCallManager | Absent | Extend `modem`/`ip` WIT with data-call lifecycle |
| DeviceCapability | Host-backed, partial | The daemon-service `get()` returns selected `Device::capability()` states; add documented feature-value queries |
| Externalapi | Absent | Define supported activity/vendor operations before adding WIT |
| FmRadio | Absent | Add FM radio WIT behind the existing device feature bit |
| Geolocation | WPE engine | Supply and validate a host geolocation provider |
| GetDeviceStorage | Host-backed, partial | Finish editable handles, external change events, and permission modes |
| GetUserMedia | WPE engine | Bind camera/microphone providers and prompts to `camera`/`audio` WIT policy |
| HTMLMediaElement | WPE engine | Validate codecs, audio routing, visibility, and suspend behavior |
| InputMethod | Absent | Add focused-input/event protocol; WIT required for non-Web peers |
| MobileConnection | Host-backed, partial | Snapshot and radio power use `modem` WIT provider; add complete registration/SIM/call events |
| MozSpeakerManager | Absent | Add route/speaker selection to `audio` WIT |
| MozTCPSocket | Absent | Prefer capability-scoped WASI sockets or add asynchronous socket WIT |
| Notification | WPE engine | Add OOS notification UI, persistence, click, and system-message delivery |
| PowerManager | Absent | KaiOS 3 exposes a daemon service, not `navigator.b2g`; add the service/session protocol before adapting the existing `power` WIT controls |
| powerSupplyManager | Host-backed, partial | With the `powersupply` permission, battery snapshots use `power`; add continuous charger events |
| ServiceWorker | WPE engine | Validate restart, offline cache, quota, and background policy |
| Settings | Absent | Add typed, permission-scoped settings WIT and observer events |
| SystemMessage | Absent | Add host event queue shared by WPE and WAMR |
| TcpSocket | Absent | Share the selected socket contract with MozTCPSocket |
| Telephony | Absent | Add call-control/event WIT over the modem service |
| TimeService | Absent | Add privileged clock/timezone WIT and policy |
| VirtualCursor | Absent | Implement in the input/compositor layer; expose controls through WIT if needed |
| WebActivity | Absent | Add handler resolution/launch/result WIT over the app registry |
| XMLHttpRequest | WPE engine | Validate TLS roots, proxy policy, and offline errors |

## KaiOS 2.5 Delta

| Classification | Entries | APIs |
| --- | ---: | --- |
| WPE engine API | 4 | AudioContext, Clients, Geolocation, MediaSource |
| OOS host API, partial | 5 | BatteryManager, Bluetooth, Data Store, Device Storage, WiFi Information |
| Absent | 13 | Alarm, LargeText, mozHasPendingMessage, mozSetMessageHandler, Network Stats, NFC, SEChannel, SEManager, SEReader, SEResponse, SESession, SpeedDial, VolumeManager |

The legacy profile exposes only its own B2G 2.5 names. Beyond the 22-entry
public index, it currently has partial host adapters for PowerSupply,
vibration, wake locks, device capabilities, Wi-Fi management, camera
enumeration/torch, MobileConnection, and `mozApps.getSelf()`.
DataStore implements the complete record method surface for stores declared in
`datastores-owned`: `get`, `add`, `put`, `remove`, `clear`, `getLength`,
revision checks, local change events, and `sync` cursors. State is persisted by
the same application-private `storage` WIT backend used by WAMR. Cross-app
`datastores-access`, global ownership arbitration, and cross-process change
broadcasts remain absent. WebSMS, voicemail, and cell broadcast also stay
absent until their backing service and event semantics exist. Missing APIs are
not represented by success-returning empty objects.

## Implementation Order

1. Build a permission-aware host event channel. Alarms, notifications,
   telephony, settings observers, storage changes, and activities all depend on
   reliable asynchronous delivery and application wake-up.
2. Finish the partial adapters: camera/media sessions, Bluetooth and modem
   events, battery events, Wi-Fi state events, and wake-lock ownership cleanup.
3. Add new shared WIT contracts for alarms, settings, contacts, applications /
   activities, notifications, and telephony. Implement each once in the host,
   then add thin WPE and WAMR adapters.
4. Finish lower-priority or hardware-specific APIs: FM radio, NFC/SE,
   NetworkStats, input method, virtual cursor, and vendor Externalapi methods.

Every new privileged operation must terminate in an OOS host service. WPE may
adapt DOMRequest/Promise/event semantics and WAMR may lower Canonical ABI data,
but neither runtime should own a second device implementation.
