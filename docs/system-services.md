# System Services

`oos::services::SystemServiceHub` is the device-independent policy and state
broker used by trusted native applications through
`oos:platform/system-services@0.1.0`. It stores bounded JSON state in
`/data/system/services.sqlite3`.

The broker owns settings, accessibility state, alarms, notifications,
contacts, activities, system messages, audio policy, input-method policy, time
policy, and application metadata. Hardware access is intentionally separate:
Wi-Fi, Bluetooth, modem, power, display, camera, and audio use typed WIT
interfaces and explicit permissions.

The JSON request envelope is a stable internal ABI for extensible system policy
objects. It is not a Web compatibility bridge. Guest-language SDKs expose
typed wrappers and keep JSON parsing in the native host.

WAMR creates the broker only for an application granted the `system`
permission, so ordinary applications pay no database initialization cost.
Observers and event queues are also initialized lazily. Access qualifiers are
enforced by the broker: readonly cannot mutate, create-only cannot enumerate
or replace existing records, and system-message subscriptions must match an
application declaration or a managed API permission.
