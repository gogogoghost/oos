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

`scripts/apply-third-party-patches.sh` applies the small compatibility patch
recorded in `patches/` after a clean Gecko checkout is placed here.
