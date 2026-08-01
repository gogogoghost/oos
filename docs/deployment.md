# OOS Runtime Deployment

Deployment is split into a stable bootstrap scaffold and an atomic versioned
runtime resource (`res`). The resource contains the native `oos` executable,
shared application fonts, and licenses. The LVGL SystemUI and boot splash are embedded in
the `oos` executable. The resource contains no browser engine, JavaScript
runtime, or Web helper process.

## Installed Layout

Production firmware uses `/system/oos`; development uses the same layout under
`/data/local/tmp/oos`:

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
│   ├── bin/oos
│   ├── share/fonts/ui-proportional.otf
│   ├── share/licenses/oos/
│   ├── manifest.env
│   ├── SHA256SUMS
│   └── COMPLETE
└── res-1.0.1/
```

`/data/oos` is the persistent root. `init.sh` bind-mounts it at
`rootfs/data`, mounts the active resource at `rootfs/opt/oos`, and mounts the
stock `/system`, `/dev`, `/proc`, `/sys`, optional `/vendor`, and Runtime APEX.
Internal storage and the first mounted TF-card candidate are exposed under
`/data/media`; application databases and packages stay on `/data/oos`.

The scaffold is device-specific because HWC service names, audio readiness,
and removable-storage mount points differ between the Nokia 2780 Flip and
8110 4G. A resource package also records its target device and cannot be
activated by a mismatched scaffold.

## Build Packages

Build OOS before packaging:

```sh
make system DEVICE=nokia-2780-flip
./scripts/package-oos-scaffold.sh \
  --device nokia-2780-flip --res-version 1.0.0 --tgz
./scripts/package-oos-res.sh 1.0.0 \
  --device nokia-2780-flip --activate --tgz
```

Outputs are written below `dist/<device>/`. The scaffold and resource scripts
can also emit directories only. Packaging rejects unresolved ELF dependencies
against the selected stock system and strips the production executable.

For the 8110 use `--device nokia-8110-4g` and build that system target first.
Android 6 and Android 10 always use separate CMake directories and resource
packages.

## Trial Installation

For a tgz-based development installation:

```sh
adb push dist/nokia-2780-flip/oos-scaffold-nokia-2780-flip.tgz \
  /data/local/tmp/
adb push dist/nokia-2780-flip/oos-res-nokia-2780-flip-1.0.0.tgz \
  /data/local/tmp/
adb shell
su
mkdir -p /data/local/tmp/oos-install
cd /data/local/tmp/oos-install
tar -xzf ../oos-scaffold-nokia-2780-flip.tgz
tar -xzf ../oos-res-nokia-2780-flip-1.0.0.tgz
cd oos
./activate-res.sh 1.0.0
./init.sh
./start.sh
```

On a device where `adb shell` is already root, omit `su`. Archive extraction
must preserve the `res` symbolic link. `start.sh` calls `init.sh` itself, so the
explicit initialization above is useful for diagnosis but not mandatory.

`start.sh` validates checksums and the device ID, stops B2G, initializes the
selected display service, and enters the rootfs with `chroot`. Stock system
libraries take precedence in `LD_LIBRARY_PATH`; OOS does not carry a parallel
browser C++ runtime.

## Upgrade And Cleanup

Install a new `res-X.Y.Z` beside the old one, stop OOS, unmount the old runtime,
then activate and start the new version:

```sh
./deinit.sh
./activate-res.sh 1.0.1
./start.sh
```

Activation verifies `manifest.env`, `SHA256SUMS`, and `COMPLETE` before
switching the symlink. The previous resource is retained for rollback.
`./gc-res.sh` previews obsolete versions; `./gc-res.sh --apply` deletes only
versions that are neither active nor the recorded previous version.

All lifecycle operations use one lock. `deinit.sh` first terminates the tracked
OOS process, then unmounts paths in reverse order. Lazy unmount is disabled by
default and can be explicitly enabled with `OOS_FORCE_LAZY_UNMOUNT=1` for
recovery.
