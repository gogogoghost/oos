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
  `omnij2me.localhost:8080` application origin.

Both OmniJ2ME modules compiled and instantiated through WAMR, with 11/11 and
40/62 import/export counts respectively. The application rendered and accepted
input for the full interactive test.

The ARM32 build and QEMU execution test are intentionally still pending. The
device build pipeline now builds and packages the same extension with the WAMR
interpreter and AOT loader enabled, but that target must not be marked validated
until its ELF is run under the Android 23 ARM userspace and then on hardware.
