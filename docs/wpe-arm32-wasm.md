# ARM32 WebAssembly Runtime Status

## Runtime Decision

WPE WebKit is built with `ENABLE_WEBASSEMBLY=OFF` on every OOS device. JSC
continues to provide modern JavaScript with its Baseline and DFG tiers, while a
WebProcess extension supplies the browser `WebAssembly` API through the pinned
WAMR 2.4.4 runtime. This keeps the WebAssembly implementation identical across
local and ARM32 profiles and removes JSC's ARM32 Wasm code generator from the
runtime.

The previous JSC policy used eager BBQ compilation and disabled Wasm loop OSR.
It was removed together with its source patches and environment options. Those
options changed compilation timing but could not detect or prevent silently
incorrect generated code.

## Reason For Migration

OmniJ2ME exposed a reproducible JSC `WasmBBQJIT32_64` miscompilation on the
Nokia 8110 4G. The optimized compiler module instantiated successfully, but
generated incorrect J2BC root-map liveness masks. `-O1`, disabling optimization
for the liveness function, or changing its bitsets from 64-bit to 32-bit made
the output match the host. Requiring those application-specific workarounds
would not provide general KaiOS compatibility.

The reduced test established that HTML transport, the application package,
and the JavaScript glue were not the cause. The failure was specific to the
JSC ARM32 Wasm execution path, so OOS no longer ships that path.

## WAMR Web Bridge

`system/runtime/wamr-web` installs a default-world WebProcess extension before
application scripts run. It provides `Module`, `Instance`, `Memory`,
`validate`, `compile`, `instantiate`, and both streaming helpers. Function and
fixed-memory imports are connected through WAMR's C API.

Linear memory is allocated as a JSC ArrayBuffer first. WAMR's allocator then
uses the same backing address, so typed-array access in JavaScript and guest
loads/stores do not require a byte-buffer copy. The current profile requires
`initial == maximum`; `grow()` fails explicitly. This matches the two
Emscripten modules in the compatibility fixture and avoids relocating a JSC
ArrayBuffer behind application views.

The JavaScript identity cache holds `WebAssembly.Memory` wrappers weakly. When
the application drops its last instance, export, and memory references, the
bridge removes the WAMR instance from its process-wide store and immediately
deinstantiates it. This releases the linear-memory backing without waiting for
the WebProcess to exit, which is required for failed loads, application
switches, and repeated module instantiation on low-memory devices.

The bridge currently covers the core numeric function and memory surface used
by KaiOS Emscripten applications. Browser-facing Table, Global, reference
values, BigInt i64 conversion, memory growth, and custom-section contents
remain explicit follow-up work. Unsupported imports fail with a LinkError
instead of returning success-shaped placeholders.

## Validation

The local x86 profile has been validated in interpreter mode through:

- a deterministic API test covering synchronous and asynchronous module
  creation, JS function imports, and zero-copy imported-memory identity;
- a 32 MiB pre-allocation regression for JSC native-class lifetime;
- OmniJ2ME 97.16's 321,699-byte compiler module and 1,372,428-byte runtime
  module; and
- installation and interactive play of a 240x320 JAR through the real
  packaged KaiOS application path before the port-free origin refactor.

Both OmniJ2ME modules compiled and instantiated through WAMR, with 11/11 and
40/62 import/export counts respectively. The application rendered and accepted
input for the full interactive test.

The port-free `http://omnij2me.localhost/` resource handler requires a fresh
WPE build and end-to-end smoke test before it can be included in this validation
record.

The Android 23 ARMv7 runtime is validated on the Nokia 8110 4G with both
OmniJ2ME modules. Packages retain the original Wasm bytes while publishing AOT
artifacts in a WAMR-versioned ARMv7 namespace. Runtime lookup uses the raw Wasm
SHA-256, so byte buffers supplied directly to `WebAssembly.compile()` receive
the same cache behavior as URL-loaded modules. Cache misses fall back to the
interpreter without changing the browser API. Compiler and runtime modules load
from AOT, instantiate their JavaScript imports, install a JAR, and run the game
on device.

## Trusted-application performance profile

Set `OOS_WAMR_WEB_UNSAFE_FAST=ON` to build the opt-in performance profile. It
uses the target CPU and ABI declared by the device WPE configuration and stores
artifacts in a separate `no-bounds` namespace. Build cache entries from the raw
Wasm files so runtime lookup remains SHA-256 based:

```sh
OOS_WAMR_WEB_UNSAFE_FAST=ON \
  ./scripts/build-webassembly-aot-cache.sh nokia-8110-4g \
  compiler.wasm runtime.wasm
```

The Nokia 8110 profile emits ARMv7A code scheduled for Cortex-A7 with VFPv4
and hardware division. NEON is disabled because LLVM vectorization through
WAMR 2.4.4 can turn valid unaligned Wasm accesses into faulting ARM loads.

This profile removes Wasm memory, native-stack, and auxiliary-stack checks. A
malformed module can corrupt or terminate the WebProcess instead of producing a
WebAssembly trap, so it is only suitable while OOS treats installed packages as
trusted. Leave the variable unset to retain the checked runtime and AOT cache.
