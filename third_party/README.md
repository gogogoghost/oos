# Third-Party Sources

Large source checkouts in this directory are intentionally ignored by the main
repository. Their reproducible revisions are recorded in `versions.env`.

The executable application runtime is:

- `wasm-micro-runtime`: pinned WAMR 2.4.4, fetched and revision-checked by
  `scripts/fetch-wamr.sh`.

The native system UI and development-tool UI backends are:

- `lvgl`: pinned LVGL 9.5.0, used by the trusted process-local SystemUI.
- `imgui`: pinned Dear ImGui 1.92.9b, exposed through the same OOS graphics
  host for diagnostics and native tools.

Fetch and revision-check both UI frameworks with
`scripts/fetch-ui-frameworks.sh`. Neither checkout is copied into the main
repository.

The Nokia 2780 Flip build also uses Android 10-era AOSP headers and hardware
interfaces from these local trees:

- `aosp-frameworks-native`
- `aosp-hardware-interfaces`
- `aosp-hardware-libhardware`
- `aosp-system-core`
- `aosp-system-libfmq`
- `aosp-system-libhidl`
- `gecko-b2g`
- `kaios-hidl-gen`

The Nokia 8110 4G build uses matching Android 6.0.1 r3 ABI headers:

- `aosp-frameworks-native-android6`
- `aosp-system-core-android6`
- `aosp-hardware-libhardware-android6`
- `aosp-libpng-android6`
- `aosp-system-media-android6`
- `aosp-bionic-android6`
- `aosp-hardware-ril-android6`

The sparse `kaios-gecko-b2g48` tree is reference source for the 8110's legacy
Bluetooth daemon protocol. It is not linked into OOS. Likewise, the Android 10
Gecko checkout supplies reproducible HWC/HIDL reference definitions and a
small compatibility header patch; it is not a browser shipped by OOS.

OOS has no WPE WebKit, libwpe, WPE backend, Cerbero, or JavaScript-engine build
dependency. Stale local checkouts of those projects are ignored and can be
removed without changing an OOS build.
