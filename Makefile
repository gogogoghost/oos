.DEFAULT_GOAL := help

DEVICE ?= nokia-2780-flip
VERSION ?=
CMAKE_ARGS ?=
CCACHE_DIR ?= $(CURDIR)/build/ccache
export CCACHE_DIR

.PHONY: help native-apps native-app-aot verify-wit test-wasm \
	configure-local local local-rootfs run-local configure system \
	package-scaffold package-res

help:
	@printf '%s\n' \
		'make native-apps       Build WIT core Wasm and Component artifacts' \
		'make native-app-aot    Build the ARMv7 WAMR AOT egui demo' \
		'make verify-wit        Validate generated WIT artifacts' \
		'make test-wasm         Run the WAMR host integration suite' \
		'make local             Build OOS for the local device' \
		'make local-rootfs      Prepare the local /opt/oos rootfs' \
		'make run-local         Run the LVGL SystemUI in a user namespace' \
		'make configure         Configure system build for DEVICE' \
		'make system            Build the configured system target' \
		'make package-scaffold  Package scaffold using VERSION and DEVICE' \
		'make package-res       Package runtime resources using VERSION and DEVICE'

native-apps:
	./scripts/build-native-apps.sh

native-app-aot:
	./scripts/build-native-app-aot.sh

verify-wit: native-apps
	./scripts/verify-wit-interfaces.sh

test-wasm:
	CCACHE_DISABLE=1 ./scripts/test-wasm-runtime.sh

configure-local:
	cmake -S system -B build/local \
		-DBUILD_NOKIA_2780_FLIP=OFF \
		-DBUILD_NOKIA_8110_4G=OFF \
		-DBUILD_LOCAL=ON \
		-DOOS_BUILD_DEVICE_TESTS=ON $(CMAKE_ARGS)

local: configure-local
	cmake --build build/local

local-rootfs: local
	./scripts/package-local-rootfs.sh

run-local: local-rootfs
	./scripts/run-local-rootfs.sh

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
