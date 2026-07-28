.DEFAULT_GOAL := help

DEVICE ?= nokia-2780-flip
VERSION ?=
CMAKE_ARGS ?=

.PHONY: help native-apps native-app-aot web-launcher verify-wit test-wasm \
	configure system package-scaffold package-res

help:
	@printf '%s\n' \
		'make native-apps       Build WIT core Wasm and Component artifacts' \
		'make native-app-aot    Build the ARMv7 WAMR AOT launcher' \
		'make web-launcher      Build the legacy Web launcher' \
		'make verify-wit        Validate generated WIT artifacts' \
		'make test-wasm         Run the WAMR host integration suite' \
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
