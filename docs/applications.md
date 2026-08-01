# OOS Application Packages And State

OOS applications are ZIP packages executed by the built-in WAMR runtime. There
is no application type or runtime discriminator because OOS supports only this
format. Browser and KaiOS Web packages are not accepted.

## Package Format

An application ZIP has this layout:

```text
application.zip
├── manifest.json
├── entry.wasm
└── entry.aot          # optional, preferred when present
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
  --output application.zip
```

The ZIP reader validates central-directory records and CRCs, bounds expanded
size and entry count, and rejects encryption, ZIP64, absolute paths, and parent
traversal.

## Registry And Launch

The canonical package is stored at:

```text
/data/packages/<app-id>/<version>/<content-key>/application.zip
```

`/data/system/app-registry.sqlite3` records the active version, package digest,
permissions, roles, handlers, and lifecycle state. Launch preparation extracts
the preferred fixed entry to `/data/cache/aot/<content-key>/`; the ZIP remains
the installed source of truth.

The registry schema version is 3. Schema-1 and schema-2 registries are migrated
transactionally. Only their OOS/WAMR records are retained; obsolete runtime,
entrypoint, and per-app memory columns are removed.

## Runtime Memory Policy

Memory limits are not package metadata. OOS currently gives every application
a 128 KiB WAMR execution stack. The WAMR host-managed heap is disabled because
WIT data exchange uses the guest's exported `cabi_realloc`; application memory
comes from its normal Wasm linear memory. Central policy prevents packages from
reserving several extra MiB for every resident application. A future system
resource policy can vary these limits without changing the package format.

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
service; application-private storage never accepts arbitrary host paths.

Signing, process isolation, and SELinux policy remain deferred during hardware
bring-up. Requested permissions are still stored and enforced at the WIT host
boundary so stronger install policy can be added without changing the package
or database format.
