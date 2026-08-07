# OmniJ2ME Platform Contract

This document records the OOS side of the OmniJ2ME integration. It complements
the application author's requirements without making OmniJ2ME-specific policy
part of unrelated device backends.

## Implemented Platform Decisions

- ABI 8 provides worker-safe microsecond monotonic time, millisecond Unix epoch
  time, and a coalescing main-thread wake primitive.
- Lifecycle `frame` returns the requested delay. The shell waits for the
  earliest application/SystemUI deadline and wakes on input or guest work.
- Managed audio accepts immutable dynamic byte sources. Source/player counts
  and encoded-byte budgets are discoverable, players retain closed sources,
  content sniffing tolerates legacy MIME metadata, and status has structured
  failure categories.
- The production C SDK links pinned picolibc, libm, compiler-rt, WAMR pthreads,
  and TLSF without WASI or unresolved imports. The worker auxiliary stack is a
  fixed, reported build-time partition; lifecycle stack and total memory remain
  host launch parameters.
- Compiler/game isolation uses manifest-declared package modules. CPU-core AOT
  is preferred over architecture AOT, then core Wasm. Each Wasm module owns its linear memory and worker;
  calls use a bounded operation plus byte request/response instead of nested
  application lifecycles.
- The guest LVGL adapter supports a premultiplied RGBA8888 transparent overlay
  that can be combined with a retained Java RGB565 framebuffer in one submit.

## Required Guest Architecture

The OmniJ2ME shell remains the application entry. It invokes manifest-declared
compiler and runtime modules through the message-oriented modules API. Modules
are instantiated lazily, retained until the application session closes, and do
not receive application lifecycle, input, or rendering callbacks. Compiler
output is persisted through shared application storage; `request-exit` closes
the complete application.

CPU-heavy work runs on the one guest worker. A bounded guest-owned command queue
bridges thread-affine graphics, audio, storage, and navigation calls to the
lifecycle thread. The worker may call precise clocks directly and must invoke
`wake-main-thread` after publishing commands. The lifecycle thread drains all
available commands on each wake.

OOS owns supported encoded-media decoding and the shared SoundFont. OmniJ2ME
queries `supported-formats`, sends JAR resource bytes through `source-create`,
and uses PCM only for raw/generated audio or genuinely unsupported codecs.
Programmatic MMAPI `MIDIControl` is not exposed by the current WIT contract and
must be reported as unavailable by the guest. It must not be approximated with
`play-tone`; a future implementation will use the shared OOS SoundFont service.

## Verification

`scripts/test-wasm-runtime.sh` verifies core-Wasm C/Rust applications, strict
imports, libc/libm, worker clocks and wakeups, frame delays, dynamic in-memory
MIDI, transparent LVGL compilation, worker thread-affinity traps, memory-policy
rejection, and synchronous package-module messaging.

`scripts/compile-native-app-aot.sh` derives the architecture/core target from
the required `.TARGET.aot` output name and applies the production bounds-checking
configuration. Device builds remain isolated under
`build/android-nokia-8110-4g` and `build/android-nokia-2780-flip`.
Core-Wasm and target-qualified AOT package modules use the normal `modules/`
package tree and one suffixless manifest base.

End-to-end OmniJ2ME acceptance still belongs to its application integration:
compiler and VM stack high-water measurement, large-JAR install/game PSS,
framebuffer publication barriers, RMS durability, real media corpus behavior,
and responsiveness under a saturated VM worker.
