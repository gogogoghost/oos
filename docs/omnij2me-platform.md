# OmniJ2ME Platform Contract

This document records the OOS side of the OmniJ2ME integration. It complements
the application author's requirements without making OmniJ2ME-specific policy
part of unrelated device backends.

## Implemented Platform Decisions

- ABI 7 provides worker-safe microsecond monotonic time, millisecond Unix epoch
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
- Recoverable compiler/game memory uses one ephemeral child WAMR instance.
  Packages place children in `modules/`; AOT is preferred over core Wasm.
  Children inherit capabilities/storage but own their linear memory and worker.
- The guest LVGL adapter supports a premultiplied RGBA8888 transparent overlay
  that can be combined with a retained Java RGB565 framebuffer in one submit.

## Required Guest Architecture

The OmniJ2ME shell remains the parent application. It runs the compiler child
only during installation and the runtime child only while a game is active.
Compiler output is persisted before the compiler is destroyed. Game exit
destroys only the runtime child and returns to the resident library shell;
`request-exit` closes the complete application.

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
rejection, deferred child activation, completion state, event-loop affinity,
stale handles, and PSS recovery near its pre-test baseline.

`scripts/compile-native-app-aot.sh` applies the same ARMv7 bounds-checking AOT
configuration used by production packages. Device builds remain isolated under
`build/android-nokia-8110-4g` and `build/android-nokia-2780-flip`.
Core-Wasm child test modules and ARMv7 AOT children likewise use separate
`subruntime-host-modules` and `subruntime-armv7-modules` output directories.

End-to-end OmniJ2ME acceptance still belongs to its application integration:
compiler and VM stack high-water measurement, large-JAR install/game PSS,
framebuffer publication barriers, RMS durability, real media corpus behavior,
and responsiveness under a saturated VM worker.
