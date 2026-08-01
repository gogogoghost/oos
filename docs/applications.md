# OOS Application Packages And State

OOS supports one application type: an `oos-wasm-v1` ZIP executed by WAMR.
There is no browser runtime and KaiOS Web packages are not accepted.

## Package Format

An application ZIP has this layout:

```text
application.zip
├── oos-manifest.json
├── aot/armv7/wamr-2.4.4/app.aot
└── module/app.wasm
```

The AOT entry is preferred on the ARMv7 phones. `module/app.wasm` is the
portable fallback and is also useful for host testing. A minimal manifest is:

```json
{
  "format": 1,
  "id": "org.example.app",
  "name": "Example",
  "version": "1.0.0",
  "package_kind": "oos-wasm-v1",
  "runtime_kind": "wamr",
  "api_profile": "oos-wit-0.1",
  "entrypoint": "aot/armv7/wamr-2.4.4/app.aot",
  "fallback_entrypoint": "module/app.wasm",
  "memory": {
    "stack_bytes": 131072,
    "heap_bytes": 4194304
  }
}
```

`package_kind` and `runtime_kind` must have exactly the values above. An
unknown runtime is rejected rather than silently dispatched. Package IDs,
versions, entry paths, memory bounds, and requested permissions are validated
before installation.

Create a deterministic package with:

```sh
./scripts/package-oos-wasm-app.sh \
  --manifest apps/launcher/oos-manifest.json \
  --wasm build/native-apps/launcher.wasm \
  --aot build/native-apps/launcher.aot \
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

`/data/system/app-registry.sqlite3` records the active version, runtime,
entrypoints, WIT profile, memory limits, permissions, roles, handlers, and
lifecycle state. Launch preparation extracts only the selected AOT/Wasm entry
to `/data/cache/aot/<content-key>/app.aot` or `app.wasm`; the ZIP remains the
installed source of truth.

The schema version is 2. When a schema-1 registry is opened, records whose
package/runtime are not `oos-wasm-v1`/`wamr` are removed transactionally. Old
Web profile/cache files are not reused and may be removed separately after an
upgrade rollback is no longer required.

The command-line operations are:

```sh
/opt/oos/bin/oos --install /data/tmp/example.zip
/opt/oos/bin/oos --list-apps
/opt/oos/bin/oos --app org.example.app
```

The package identity always comes from `oos-manifest.json`; installers cannot
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
