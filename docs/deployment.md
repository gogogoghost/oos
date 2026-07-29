# OOS Runtime Deployment

The deployable OOS system is split into a stable bootstrap scaffold and a
versioned `res` runtime. In this context, `res` is the complete OOS runtime,
not web application content. It contains the `oos` executable, WPE WebKit,
shared libraries, WebKit helper processes, runtime data, and certificates.
It also contains WAMR native applications and the device boot splash; all are
upgraded atomically with the native runtime.

## Installed Layout

Production firmware stores the directory at `/system/oos`. Development and
trial installations use the identical layout at `/data/local/tmp/oos`.
Bootstrap scripts derive the installation path from their own location.

```text
oos/
├── activate-res.sh
├── bootstrap.conf
├── bootstrap.sh
├── init.sh
├── deinit.sh
├── gc-res.sh
├── start.sh
├── rootfs/
├── res -> res-1.0.0
├── res-1.0.0/
│   ├── packages/org.orangeos.launcher/application.zip
│   ├── bin/oos
│   ├── bin/oos-wpe
│   └── ...
└── res-1.0.1/
```

`/data/oos` is created with mode `0700` and bind-mounted at `rootfs/data` for
persistent state. It contains the application registry, canonical package
ZIPs, per-app WAMR/WebKit data, runtime caches, staging, temporary data, and
internal/removable media views. The selected version is bind-mounted at
`rootfs/opt/oos`.
The rootfs also receives `/system`, `/dev`, `/proc`, and `/sys`, plus `/vendor`
and Runtime APEX when those trees exist on the selected Android release.

All lifecycle scripts share one lock, so activation, initialization, start,
deinitialization, and res garbage collection cannot race. `start.sh` always
invokes `init.sh` itself so mount setup and chroot execution
occur in the same mount namespace. It runs as a foreground supervisor,
forwards termination signals, records the child PID, removes the PID file on
normal exit, and returns the real `oos` exit status. `deinit.sh` terminates a
running instance and unmounts in reverse order. Lazy unmount is disabled by
default; set `OOS_FORCE_LAZY_UNMOUNT=1` only for recovery.

## Build Packages

Build Android `oos` and the WPE sysroot first, then generate the scaffold:

```sh
./scripts/package-oos-scaffold.sh --res-version 1.0.0 --tgz
```

The default directory is `dist/nokia-2780-flip/oos`. The optional archive is
`dist/nokia-2780-flip/oos-scaffold-nokia-2780-flip.tgz` and contains a single
top-level `oos` directory.

Generate and optionally activate a runtime version in that directory:

```sh
./scripts/package-oos-res.sh 1.0.0 --activate --tgz
```

The res archive contains a single top-level `res-1.0.0` directory. The
generator copies target shared objects and runtime data while excluding the
WPE sysroot's headers, static libraries, pkg-config metadata, and host tools.
Each package contains a manifest, `SHA256SUMS`, and a `COMPLETE` marker.
Packaging also checks the dynamic dependency closure against the packaged
libraries and `NOKIA_2780_SYSTEM_DIR` from `.env`; unresolved SONAMEs stop the
build.

Both scripts accept `--device` and `--output`. Nokia 2780 and Nokia 8110
packages use separate scaffold directories, WPE sysroots, Android API metadata,
compiled-prefix links, and system-library dependency checks. For example:

```sh
./scripts/package-oos-scaffold.sh --device nokia-8110-4g \
  --res-version 1.0.0 --tgz
./scripts/package-oos-res.sh 1.0.0 --device nokia-8110-4g \
  --activate --tgz
```

## Trial Installation

Push and extract the two independent archives:

```sh
adb push dist/nokia-2780-flip/oos-scaffold-nokia-2780-flip.tgz \
  /data/local/tmp/
adb shell "su -c 'tar -xzf /data/local/tmp/oos-scaffold-nokia-2780-flip.tgz \
  -C /data/local/tmp'"

adb push dist/nokia-2780-flip/oos-res-nokia-2780-flip-1.0.0.tgz \
  /data/local/tmp/
adb shell "su -c 'tar -xzf \
  /data/local/tmp/oos-res-nokia-2780-flip-1.0.0.tgz \
  -C /data/local/tmp/oos'"
```

Activate, initialize, and start:

```sh
adb shell "su -c '/data/local/tmp/oos/activate-res.sh 1.0.0'"
adb shell "su -c '/data/local/tmp/oos/init.sh'"  # optional diagnostic step
adb shell "su -c '/data/local/tmp/oos/start.sh'"
```

`start.sh` performs idempotent initialization, so invoking `init.sh` manually
is not required. The production entry presents the 240x320 boot splash,
imports `/opt/oos/packages/org.orangeos.launcher/application.zip` into the
application registry on first boot, selects its AOT entry for WAMR, and
forwards physical navigation keys to it. The portable Wasm fallback stays in
the same ZIP. Selecting a registered KaiOS application starts the packaged
`/opt/oos/bin/oos-wpe` producer while the main OOS process retains display and
input ownership. Each Web app receives separate persistent WebKit data and
content-versioned cache directories.

## Upgrade

Extract the new res archive beside the existing version, stop the supervising
service, and unmount the old runtime before switching:

```sh
/system/oos/deinit.sh
/system/oos/activate-res.sh 1.0.1
/system/oos/start.sh
```

`activate-res.sh` rejects an active process or mounted res, verifies all file
hashes, checks the target device, and then replaces the `res` symlink. On a
Nokia 2780, reading and hashing the current 221 MB runtime takes roughly 20-30
seconds. When production `/system` is read-only, adding a res directory and
updating the link
must be performed by the firmware/OTA installation environment or after an
explicit writable remount; the runtime scripts do not remount `/system`.

`gc-res.sh` is dry-run by default and retains the active and previously active
versions. Run `gc-res.sh --apply` only after reviewing its list.

The local WPE build uses `/opt/oos` as its native install prefix and stages it
with `DESTDIR`, so user-namespace tests exercise the final paths directly.
Local packaging records the host GStreamer version and generates the plugin
registry consumed from `/opt/oos/share/gstreamer-1.0`; the namespace disables
runtime registry updates to keep WebProcess startup deterministic.
The current Android Cerbero outputs still embed their isolated build prefixes;
Android scaffold generation supplies the corresponding chroot-only
compatibility link until those profiles also move to a staged `/opt/oos`
install.
