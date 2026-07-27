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

The Nokia 8110 4G build additionally uses matching Android 6.0.1 r3 ABI
headers:

- `aosp-frameworks-native-android6`
- `aosp-system-core-android6`
- `aosp-hardware-libhardware-android6`
- `aosp-libpng-android6`
- `aosp-system-media-android6`
- `aosp-bionic-android6`
- `aosp-hardware-ril-android6`

The sparse `kaios-gecko-b2g48` checkout is reference source for the Nokia
8110's legacy bluetoothd protocol. It is not compiled into OOS, but its pinned
revision makes the protocol analysis reproducible. Obsolete experimental
`frameworks/av` AudioTrack code is not a build dependency; the production 8110
audio backend uses OpenSL ES.

`scripts/apply-third-party-patches.sh` applies the small compatibility patch
recorded in `patches/` after a clean Gecko checkout is placed here.

Pinned downloadable revisions are recorded in `third_party/versions.env`.
The WAMR checkout itself is ignored like the larger Android and WPE trees.

The WPE sysroot is generated under `build/wpe-sysroot/` and is not committed.
