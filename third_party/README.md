# Third-Party Sources

This directory contains local checkout dependencies and is intentionally not
tracked by the main repository. The current Nokia 2780 Flip build needs these
Android 10-era checkouts:

- `aosp-frameworks-native`
- `aosp-hardware-interfaces`
- `aosp-hardware-libhardware`
- `aosp-system-core`
- `aosp-system-libfmq`
- `aosp-system-libhidl`
- `gecko-b2g`
- `kaios-hidl-gen`
- `wasm-micro-runtime` (WAMR 2.4.4; fetched and revision-checked by
  `scripts/fetch-wamr.sh`)
- `wpewebkit` (fixed at upstream `wpewebkit-2.52.5`)
- `wpe-android` and `wpe-android-cerbero` (Android dependency build reference)

`scripts/apply-third-party-patches.sh` applies the small compatibility patch
recorded in `patches/` after a clean Gecko checkout is placed here.

Pinned downloadable revisions are recorded in `third_party/versions.env`.
The WAMR checkout itself is ignored like the larger Android and WPE trees.

The WPE sysroot is generated under `build/wpe-sysroot/` and is not committed.
