# OOS Application Packages And State

OOS applications are ZIP packages executed by the built-in WAMR runtime. There
is no application type or runtime discriminator because OOS supports only this
format. Browser and KaiOS Web packages are not accepted.

## Package Format

An application ZIP has this layout:

```text
application.zip
├── manifest.json
├── entry.wasm         # optional when entry.aot is present
├── entry.aot          # optional, preferred when present
├── modules/           # optional ephemeral child runtimes
│   ├── compiler.wasm
│   └── runtime.aot    # preferred over runtime.wasm
└── assets/            # optional read-only application files
    └── ...
```

An application must contain `entry.wasm`, `entry.aot`, or both. OOS prefers
`entry.aot` when both are present. Production packages may contain only
`entry.aot` to avoid shipping the redundant core Wasm module. A complete minimal
manifest is:

```json
{
  "id": "cc.jaxy.oos.example",
  "name": "Example",
  "version": "1.0.0"
}
```

IDs use lowercase reverse-domain notation with at least three components, such
as `cc.jaxy.oos.launcher` or `com.example.weather`. A system role and requested
permissions are optional functional metadata:

```json
{
  "id": "cc.jaxy.oos.launcher",
  "name": "Orange OS Launcher",
  "version": "0.1.0",
  "role": "launcher",
  "permissions": {
    "settings": {"access": "readonly"}
  }
}
```

The obsolete `format`, `package_kind`, `runtime_kind`, `api_profile`, entrypoint,
and `memory` fields are rejected. Fixed paths prevent a Manifest from selecting
an incompatible executable or runtime.

Create a deterministic package with:

```sh
./scripts/package-oos-wasm-app.sh \
  --manifest apps/tests/egui-demo/manifest.json \
  --wasm build/native-apps/egui-demo.wasm \
  --aot build/native-apps/egui-demo.aot \
  --module compiler=build/compiler.wasm \
  --module runtime=build/runtime.aot \
  --assets path/to/assets \
  --output application.zip
```

The ZIP reader validates central-directory records and CRCs, bounds expanded
size and entry count, and rejects encryption, ZIP64, absolute paths, parent
traversal, and duplicate entries. Packaged assets are CRC-validated at install,
then exposed read-only through the `assets` WIT interface. Child modules use a
single safe filename component, are limited to 32 MiB each, and are extracted
into the same content-keyed cache. Applications resolve them by name through
the `subruntime` WIT API, never by a host path.

## Registry And Launch

The canonical package is stored at:

```text
/data/packages/<app-id>/<version>/<content-key>/application.zip
```

`/data/system/app-registry.sqlite3` records the active version, package digest,
permissions, roles, handlers, and lifecycle state. Launch preparation extracts
the preferred fixed entry and validated `assets/` and `modules/` trees to
`/data/cache/aot/<content-key>/`; the ZIP remains the installed source of truth.
The content-key cache prevents stale files from another package revision being
reused.

The registry schema version is 3. Schema-1 and schema-2 registries are migrated
transactionally. Only their OOS/WAMR records are retained; obsolete runtime,
entrypoint, and per-app memory columns are removed.

## Runtime Memory Policy

Memory limits are not package metadata. The production C build defaults to a
512 KiB lifecycle stack and a fixed 2 MiB worker stack. Host launch policy can
choose the lifecycle stack. Worker stack partitioning is fixed when the module
is linked, and OOS enforces one 64 MiB encoded/hard memory ceiling. Lower
32/48 MiB application settings are guest allocator or VM budgets, not host
instance limits. Stack regions count against the total linear-memory
budget. The WAMR host-managed heap is disabled because WIT data exchange uses the guest's exported
`cabi_realloc`; application memory comes from its normal Wasm linear memory.
Core modules must declare a maximum and all core/AOT instances are limited to
64 MiB.

## Application Sessions

Every launched application is represented by the same host-side application
session contract, whether it is a trusted process-local application or a WAMR
package. The session owns the application instance and a dedicated compositor
layer. `ApplicationSessionManager::activate(id)` only hides the previous layer,
reveals the requested layer, and redirects input; it does not reconstruct an
existing application.

Consequently, a background application retains its complete UI tree, Wasm
linear memory, navigation model, cached data, and graphics resources. Only the
foreground session receives input and frame callbacks. Session switching never
branches on a particular application ID; built-in and package IDs are entries
in the same factory registry.

OOS keeps a background session resident unless the application explicitly calls
`runtime.request-exit`. That request is consumed only after the lifecycle call
returns; OOS activates Launcher, invokes `shutdown()` once, and destroys the
session and all host resources. It does not silently evict an application
because doing so would violate exact state restoration. A future memory policy
must pair eviction with an explicit suspend/serialize contract rather than
treating reconstruction as restoration.

The command-line operations are:

```sh
/opt/oos/bin/oos --install /data/tmp/example.zip
/opt/oos/bin/oos --list-apps
/opt/oos/bin/oos --app cc.jaxy.oos.example
```

The package identity always comes from `manifest.json`; installers cannot
override it with a second application ID.

## Persistent Storage

The first user uses this layout:

```text
/data/
├── system/app-registry.sqlite3
├── packages/<app-id>/<version>/<content-key>/application.zip
├── users/0/wasm/<app-id>/kv.sqlite3
├── users/0/wasm/<app-id>/db/<name>.sqlite3
├── cache/aot/<content-key>/
├── staging/
├── tmp/
├── media/internal/
└── media/removable/
```

Guests access byte-valued KV and prepared SQLite operations through WIT. Native
database handles never enter guest memory. User-visible internal storage and
the first mounted TF card are exposed only through the `device-storage` WIT
service. `device-storage:read`, `:create`, and `:write` are independently
granted; application-private storage never accepts arbitrary host paths.

Signing, process isolation, and SELinux policy remain deferred during hardware
bring-up. Requested permissions are still stored and enforced at the WIT host
boundary so stronger install policy can be added without changing the package
or database format.
