.DEFAULT_GOAL := help

DEVICE ?= nokia-2780-flip
VERSION ?=
CMAKE_ARGS ?=
CCACHE_DIR ?= $(CURDIR)/build/ccache
export CCACHE_DIR

.PHONY: help native-apps native-app-aot web-launcher verify-wit test-wasm \
	fetch-wpe wpe-local wamr-web wamr-web-local configure-local local local-rootfs run-local \
	run-web-local configure system package-scaffold package-res

help:
	@printf '%s\n' \
		'make native-apps       Build WIT core Wasm and Component artifacts' \
		'make native-app-aot    Build the ARMv7 WAMR AOT launcher' \
		'make web-launcher      Build the legacy Web launcher' \
		'make verify-wit        Validate generated WIT artifacts' \
		'make test-wasm         Run the WAMR host integration suite' \
		'make fetch-wpe         Fetch and verify pinned WPE sources' \
		'make wpe-local         Build the pinned local WPE sysroot' \
		'make wamr-web DEVICE=  Build the device WAMR WebProcess extension' \
		'make wamr-web-local    Build the WAMR WebAssembly WebProcess extension' \
		'make local             Build OOS for the local device' \
		'make local-rootfs      Prepare the local /opt/oos rootfs' \
		'make run-local         Run the WIT launcher in a user namespace' \
		'make run-web-local     Run the Web launcher in a user namespace' \
		'make configure         Configure system build for DEVICE' \
		'make system            Build the configured system target' \
		'make package-scaffold  Package scaffold using VERSION and DEVICE' \
		'make package-res       Package runtime resources using VERSION and DEVICE'

native-apps:
	./scripts/build-native-apps.sh

native-app-aot:
	./scripts/build-native-app-aot.sh

web-launcher:
	./scripts/build-launcher.sh

verify-wit: native-apps
	./scripts/verify-wit-interfaces.sh

test-wasm:
	CCACHE_DISABLE=1 ./scripts/test-wasm-runtime.sh

fetch-wpe:
	./scripts/fetch-wpe.sh all

wpe-local:
	./scripts/build-wpe-local.sh

wamr-web-local: wpe-local
	./scripts/build-wamr-web.sh local

wamr-web:
	./scripts/build-wamr-web.sh "$(DEVICE)"

configure-local: native-apps web-launcher wamr-web-local
	cmake -S system -B build/local \
		-DBUILD_NOKIA_2780_FLIP=OFF \
		-DBUILD_NOKIA_8110_4G=OFF \
		-DBUILD_LOCAL=ON \
		-DOOS_LOCAL_WPE=ON \
		-DOOS_BUILD_DEVICE_TESTS=ON $(CMAKE_ARGS)

local: configure-local
	cmake --build build/local

local-rootfs: local
	./scripts/package-local-rootfs.sh

run-local: local-rootfs
	./scripts/run-local-rootfs.sh native

run-web-local: local-rootfs
	./scripts/run-local-rootfs.sh web

configure:
	./scripts/configure-android.sh "$(DEVICE)" $(CMAKE_ARGS)

system: configure
	cmake --build "build/android-$(DEVICE)"

package-scaffold:
	@test -n "$(VERSION)" || { echo 'VERSION is required' >&2; exit 2; }
	./scripts/package-oos-scaffold.sh "$(VERSION)" --device "$(DEVICE)"

package-res:
	@test -n "$(VERSION)" || { echo 'VERSION is required' >&2; exit 2; }
	./scripts/package-oos-res.sh "$(VERSION)" --device "$(DEVICE)"
