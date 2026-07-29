# OOS Application Packages And State

OOS stores every installable application as one ZIP file. The ZIP in
`/data/packages/<app-id>/<version>/<content-key>/application.zip` is the
canonical package. Runtime files are only materialized into content-addressed
caches when a VM needs an ordinary file for `mmap`. Application type and
launch policy live in SQLite rather than being inferred from an arbitrary
directory at boot.

## Supported Package Types

| Package | Manifest | Runtime | API profile |
| --- | --- | --- | --- |
| OOS native app | `oos-manifest.json` | WAMR core Wasm or ARMv7 AOT | `oos-wit-0.1` |
| KaiOS 2.5 app | `manifest.webapp` | WPE WebKit | `kaios-b2g48` |
| KaiOS 3 app | `manifest.webmanifest` | WPE WebKit | `kaios-v3` |

An OOS package declares its stable ID. KaiOS packages currently require the
installer to supply an ID because the manifest formats do not provide one
reliable package identity across firmware variants:

```sh
/opt/oos/bin/oos --install /data/tmp/example.zip
/opt/oos/bin/oos --install /data/tmp/kaios.zip --id org.example.kaios
/opt/oos/bin/oos --list-apps
/opt/oos/bin/oos --app org.orangeos.launcher
/opt/oos/bin/oos --app org.example.kaios
```

The Launcher package is built deterministically with:

```sh
./scripts/package-oos-wasm-app.sh \
  --manifest apps/launcher/oos-manifest.json \
  --wasm build/native-apps/launcher.wasm \
  --aot build/native-apps/launcher.aot \
  --output application.zip
```

The package reader uses the ZIP central directory, supports stored and
deflated entries, validates CRCs, bounds entry count and expanded size, and
rejects encrypted, ZIP64, absolute, and parent-traversal paths. Signing is
intentionally deferred; the current content key is for cache invalidation,
not authenticity.

## Registry And Launch Dispatch

`/data/system/app-registry.sqlite3` records applications, versions,
permissions, roles, handlers, lifecycle state, package kind, runtime kind, and
API profile. The active version points at the canonical ZIP. A launch first
resolves this record, then prepares one of two contexts:

- WAMR extracts only the selected AOT/Wasm entry to
  `/data/cache/aot/<content-key>/app.*` and maps it from there.
- WPE keeps the package zipped and serves requested HTML, CSS, JavaScript,
  image, font, and Wasm entries from `http://<app-id>.localhost`. The embedded
  loopback HTTP server is static-package transport only; it never exposes
  device files or privileged operations as HTTP routes.

WPE API shims must be selected from `api_profile`; a KaiOS 2.5 application
must not receive the KaiOS 3 bridge by accident. On Android, `oos` starts one
foreground `oos-wpe` producer, imports its GPU buffers through the surface
transport, and forwards normalized key events over the same connection. The
producer cannot access HWC, panel power, backlights, or evdev directly.

The injected bridge exposes immutable `__oosRuntime` identity, lifecycle,
key-name compatibility, and the selected KaiOS API profile. Implemented
privileged calls travel from `navigator.moz*` or `navigator.b2g` through a
WebKit message handler and a private OOS control socket. The OOS host performs
the operation; HTTP remains unaware of it. Unimplemented compatibility APIs
are explicit empty or `NotSupportedError` adapters selected by `api_profile`.

DeviceStorage is the first end-to-end adapter. Enumeration returns metadata
from `/data/media/internal` and `/data/media/removable`; file bytes are fetched
only when `FileReader` reads the selected file. The same host service is
described by WIT `device-storage`, so WPE and WAMR do not grow separate
filesystem implementations.

## Persistent Storage

The first user has separate runtime roots:

```text
/data/
├── system/app-registry.sqlite3
├── packages/<app-id>/<version>/<content-key>/application.zip
├── users/0/wasm/<app-id>/kv.sqlite3
├── users/0/wasm/<app-id>/db/<name>.sqlite3
├── users/0/web/<app-id>/data/
├── cache/aot/<content-key>/
├── cache/web/<app-id>/<content-key>/
├── media/internal/
└── media/removable/
```

WAMR guests use the versioned WIT storage interface for byte-valued KV and
prepared SQLite statements. It supports null, integer, float, text, and blob
parameters/results while keeping native database handles outside guest
memory. WPE creates a persistent `WebKitNetworkSession` per app with separate
data and cache directories, covering cookies, local storage, IndexedDB,
service-worker state, HTTP cache, and credentials according to WebKit policy.
The separate WIT `device-storage` interface covers user-visible internal and
removable media; application-private `storage` never accepts filesystem paths.

Internal storage and the first mounted TF-card candidate are bind-mounted as
media views. Database, profile, and registry files remain on `/data/oos`, so
removing a card cannot corrupt application state.

## Deferred Security

For the current bring-up phase, requested permissions are recorded with
`granted=1`, packages are marked `unverified`, and no signature, process
permission boundary, or SELinux policy is enforced. These columns and API
profiles are retained so policy can be added without changing package
identity or storage layout. This mode is suitable only for the explicitly
rooted development deployment.
