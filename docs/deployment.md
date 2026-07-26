# OOS Runtime Deployment

The deployable OOS system is split into a stable bootstrap scaffold and a
versioned `res` runtime. In this context, `res` is the complete OOS runtime,
not web application content. It contains the `oos` executable, WPE WebKit,
shared libraries, WebKit helper processes, runtime data, and certificates.
It also contains the compiled launcher and device boot splash under
`share/oos`; both are upgraded atomically with the native runtime.

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
├── start.sh
├── rootfs/
├── res -> res-1.0.0
├── res-1.0.0/
└── res-1.0.1/
```

`/data/oos` is created with mode `0700` and bind-mounted at `rootfs/data` for
persistent state. The selected version is bind-mounted at `rootfs/opt/oos`.
The rootfs also receives `/system`, `/vendor`, Runtime APEX, `/dev`, `/proc`,
and `/sys` mounts required by Android graphics, input, binder, and WebKit.

`start.sh` always invokes `init.sh` itself so mount setup and chroot execution
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
libraries and the stock system directory from `.env`; unresolved SONAMEs stop
the build.

Both scripts accept `--output`. Both currently support only
`nokia-2780-flip`; the device option is explicit so Nokia 8110 packaging can be
added without sharing incompatible rootfs or runtime files.

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
initializes WPE WebKit, loads the packaged launcher, and forwards physical
navigation keys to it.

The launcher is built as one self-contained HTML file because the device WPE
port does not reliably load `file://` CSS and JavaScript subresources from a
page supplied with `webkit_web_view_load_html()`. Startup logs include a
one-shot DOM health report. Set `OOS_WEBKIT_CONSOLE=1` only while diagnosing
JavaScript; the Qualcomm graphics stack emits high-volume warnings when the
WebKit console is continuously mirrored to standard output.

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

WPE 2.52.5 currently embeds the original build prefix for injected-bundle
lookup. The generated rootfs contains a chroot-only compatibility symlink from
that prefix to `/opt/oos`. A future WPE rebuild with `/opt/oos` as its native
install prefix can remove this compatibility path without changing the
external package layout.
