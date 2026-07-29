# System Services

`oos::services::SystemServiceHub` is the device-independent policy and state
broker for privileged compatibility APIs. It stores bounded JSON state in
`/data/system/services.sqlite3`; Web applications access it through the private
WPE control socket, while trusted native SystemUI applications use the
`oos:platform/system-services@0.1.0` WIT interface.

The broker owns these management domains:

| Service | Application operations | SystemUI operations |
| --- | --- | --- |
| `settings` | permission-scoped get, batch get, set, remove, clear | all operations and observer events |
| `accessibility` | read LargeText state | write managed accessibility state |
| `alarms` | per-app add, list, remove | list all alarms |
| `notifications` | per-app add, list, update, remove | list all and publish click/close messages |
| `contacts` | shared permission-scoped CRUD and metadata records | the same store with system authority |
| `activities` | per-app start, status, cancel | list pending requests and write resolved/rejected records by owner |
| `system-messages` | subscribe and consume targeted events | publish targeted events |
| `audio-policy` | channel state and volume UI requests | read policy and pending requests |
| `input-method` | permission-scoped input policy state | system input policy |
| `time` | permission-scoped clock read and managed requests | system time policy |
| `applications` | permission-scoped installed-app list | list apps and declared activity handlers |

The generic JSON payload is intentional: KaiOS dictionaries and vendor fields
are open and versioned. Device access is intentionally absent. The broker does
not call Wi-Fi, Bluetooth, modem, power, display, or other hardware managers.
Those remain separate typed WIT interfaces and require explicit native OOS
permissions.

WAMR only instantiates the broker for an application granted the `system`
permission, so ordinary native apps pay no database or registration cost.
WPE event polling starts lazily only after an API registers an observer,
message handler, or notification event consumer.

Access qualifiers are enforced by the broker: `readonly` cannot mutate shared
state, `createonly` can add/set but cannot enumerate or replace existing
records, and system-message subscriptions must match a manifest message or the
permission of the associated managed API.
