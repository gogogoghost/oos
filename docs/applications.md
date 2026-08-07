# OOS Application Packages And State

OOS applications are ZIP packages executed by either the built-in QuickJS or
WAMR runtime. The manifest chooses only an executable runtime; it does not
classify an application's UI. The same application can use Solid or Clay,
create Canvas2D/mesh/GLES canvases, or combine them in one scene. Browser and
KaiOS Web packages are not accepted.

## Package Format

An application ZIP has this layout:

```text
application.zip
├── manifest.json
├── app/
│   ├── main.mjs       # JavaScript entry, or
│   ├── main.wasm      # portable Wasm entry
│   ├── main.armv7a.aot       # optional architecture AOT entry
│   └── main.cortex-a53.aot   # optional CPU-core AOT entry
├── modules/           # optional manifest-declared JS/Wasm modules
│   ├── compiler.wasm
│   ├── compiler.armv7a.aot
│   └── helpers.mjs
└── assets/            # optional read-only application files
    └── ...
```

An application declares exactly one JS or Wasm entry. JavaScript paths name the
actual `.js` or `.mjs` file. Wasm paths are always suffixless artifact bases;
the runtime selects the best file. Complete minimal manifests are:

```json
{
  "schema": 1,
  "id": "cc.jaxy.oos.example",
  "name": "Example",
  "version": "1.0.0",
  "entry": {"runtime": "js", "path": "app/main.mjs"}
}
```

```json
{
  "schema": 1,
  "id": "cc.jaxy.oos.example",
  "name": "Example",
  "version": "1.0.0",
  "entry": {
    "runtime": "wasm",
    "path": "app/main"
  }
}
```

For the base `app/main`, resolution is strictly:

1. `app/main.<cpu-core>.aot`, such as `app/main.cortex-a53.aot`;
2. `app/main.<cpu-arch>.aot`, such as `app/main.armv7a.aot`;
3. `app/main.wasm` interpreted by WAMR.

Duplicate candidates are removed. Target names use lowercase ASCII letters,
digits, `-`, and `_`. AOT files without a target segment, such as `main.aot`,
are invalid. A package may be AOT-only, but it launches only on a matching
target; including `main.wasm` provides the portable fallback.

IDs use lowercase reverse-domain notation with at least three components, such
as `com.example.launcher` or `com.example.weather-app`. Each component starts
with a letter and may contain lowercase letters, digits, and internal hyphens.
The runtime-configured Launcher and SystemUI IDs must resolve to installed
packages with the required permissions. A role and requested permissions are
optional metadata; they do not select a different package or UI implementation:

```json
{
  "schema": 1,
  "id": "com.example.home",
  "name": "Example Home",
  "version": "0.1.0",
  "role": "launcher",
  "entry": {"runtime": "js", "path": "app/main.mjs"},
  "permissions": {"settings": {"access": "readonly"}}
}
```

The obsolete `format`, `package_kind`, `runtime_kind`, `api_profile`,
`entrypoint`, `fallback_entrypoint`, `memory`, and `ui` fields are rejected.
UI access follows imported APIs, just as browser code chooses elements or a
canvas by what it creates, rather than by an application-level mode.

Launcher, Settings, and SystemUI use this exact package format. Their current
entries are C++/LVGL compiled to Wasm, and their extra authority is expressed
only by manifest permissions. They are not linked into the OOS executable.

## JavaScript Build

The JavaScript SDK and build integration are normal npm packages. Application
repositories install `@oos/platform`, `@oos/vite-plugin`, `solid-js`, and Vite;
they do not clone QuickJS or depend on the OOS source tree to compile TSX.

```ts
// vite.config.ts
import { defineConfig } from "vite";
import oos from "@oos/vite-plugin";

export default defineConfig({ plugins: [oos()] });
```

`vite build` emits one browser-free `dist/main.mjs`. The plugin configures
Solid's universal renderer, externalizes native `oos:*` modules, and rejects
CSS, DOM rendering, browser assets, code splitting, and a missing `frame`
export. Package the result with the same runtime-neutral packager used for
Wasm:

```sh
./scripts/package-oos-app.sh \
  --manifest manifest.json \
  --js dist/main.mjs \
  --assets assets \
  --output application.zip
```

Create a deterministic package with:

```sh
./scripts/package-oos-app.sh \
  --manifest apps/tests/egui-demo/manifest.json \
  --wasm apps/tests/egui-demo/build/main.wasm \
  --aot armv7a=apps/tests/egui-demo/build/main.armv7a.aot \
  --aot cortex-a53=apps/tests/egui-demo/build/main.cortex-a53.aot \
  --module compiler=build/compiler.wasm \
  --module compiler=build/compiler.armv7a.aot \
  --module helpers=app/helpers.mjs \
  --assets path/to/assets \
  --output application.zip
```

Applications in this repository expose their own `package.sh`; running it from
any working directory keeps compiler output under that application's `build/`
or `target/` directory and writes `dist/application.zip`. Repository-level
scripts only invoke those application-owned commands.

The ZIP reader validates central-directory records and CRCs, bounds expanded
size and entry count, and rejects encryption, ZIP64, absolute paths, parent
traversal, and duplicate entries. Packaged assets are CRC-validated at install,
then exposed through the runtime-neutral asset service. Modules are declared by
name, runtime, and `path` base in `modules[]`; JS imports declared JS modules normally, JS
uses the restricted `WebAssembly` facade for Wasm, and both runtimes use the
message-oriented modules API for cross-runtime or Wasm-to-Wasm calls.
For AOT package modules, the packager derives the target from the final
`.TARGET.aot` portion of the source filename.

## Registry And Launch

The canonical package is stored at:

```text
/data/packages/<app-id>/<version>/<content-key>/application.zip
```

`/data/system/app-registry.sqlite3` records the active version, package digest,
permissions, roles, handlers, and lifecycle state. Launch preparation extracts
the validated `app/`, `assets/`, and `modules/` trees to
`/data/cache/apps/<content-key>/`; the ZIP remains the installed source of truth.
The repository passes a suffixless absolute base to the runtime, which performs
target selection immediately before loading.
The content-key cache prevents stale files from another package revision being
reused.

The registry schema version is 3. Because this package architecture predates a
public release, schema-1 and schema-2 registries are reset transactionally and
their applications must be reinstalled. No legacy runtime or manifest shape is
carried into schema 3.

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
session contract, whether its entry uses QuickJS, interpreted Wasm, or AOT. The
session owns the application instance and a dedicated compositor layer.
`ApplicationSessionManager::activate(id)` only hides the previous layer,
reveals the requested layer, and redirects input; it does not reconstruct an
existing application.

Consequently, a background application retains its complete UI tree, Wasm
linear memory, navigation model, cached data, and graphics resources. Only the
foreground session receives input and frame callbacks. Session switching never
branches on a particular application ID; every ID is registered from a package
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
├── users/0/apps/<app-id>/kv.sqlite3
├── users/0/apps/<app-id>/db/<name>.sqlite3
├── cache/apps/<content-key>/
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
