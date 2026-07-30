# ARM32 WPE WebAssembly Status

## Scope

OmniJ2ME exposes a deterministic JavaScriptCore miscompilation on the Nokia
8110 4G WPE profile. The failure has only been reproduced in the ARM32
`WasmBBQJIT32_64` path. The same compiler produces correct output on a 64-bit
host, but that does not prove every non-ARM32 JSC target is unaffected.

This is not an 8110-specific application or file-transport failure. The Nokia
2780 Flip also uses the ARM32 BBQ path and must be treated as potentially
affected until the same regression is run there.

## Reproduction Evidence

The original optimized compiler validates and instantiates. Its exported
compile function then either fails early or emits a structurally valid but
incorrect J2BC root map:

| Compiler build | Nokia 8110 result |
| --- | --- |
| Original `-O3 -flto` | early compiler failure reported as `manifest` |
| `-O3` without LTO | wrong J2BC root masks |
| `-O2` | wrong J2BC root masks |
| `-O1` | byte-identical to the host result |
| `-O2`, liveness function `optnone` | byte-identical to the host result |
| `-O2`, liveness bitsets changed to `uint32_t` | byte-identical to host |

The reduced one-class JAR makes each device run about 1.5 seconds:

| Result | Size | FNV-1a |
| --- | ---: | --- |
| Host / O1 / O2 `optnone` / O2 `uint32_t` | 13496 | `ccef553c` |
| Unmodified O2 on ARM32 | 13336 | `dc880111` |

For the complete test JAR, sections before `ROOT_MAP` are identical. Method
bytecode and all 17113 safepoint PCs are identical; only reference liveness
masks diverge. The corresponding source expression is:

```c
next_in = current_use[word] | (next_out[word] & ~current_def[word]);
```

Generic 32-bit and 64-bit liveness probes pass at O1 and O2. A second probe
with four live `uint64_t` values crossing Wasm calls also passes. The remaining
trigger is the optimized function's combination of high register pressure,
large branch tables, loop backedges, and 64-bit temporary values at control
flow joins.

## Runtime Policy

OOS adds `JSC_useEagerBBQCompilation` and enables it for WPE children. On
ARM32, module instantiation compiles all internal functions before resolving.
This improves timing predictability and reports JIT compilation failures at
load time, but it cannot detect silently incorrect machine code and does not
fix this issue by itself.

The only validated temporary workarounds are changes to the affected Wasm:

- compile with `-O1`;
- disable optimization on `j2me_post_analyze_liveness`; or
- implement that liveness bitset with 32-bit words.

Those workarounds are useful for diagnosis but cannot be required of arbitrary
third-party KaiOS applications. The engine fix remains open. Continue by
reducing the real optimized function to standalone WAT, then audit ARM32 BBQ
GPR-pair allocation across control-flow merges. Validate the final patch with
both the original packaged compiler and runtime Wasm on the 8110 and 2780.
