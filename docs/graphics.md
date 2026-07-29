# WIT Graphics Contract

OOS exposes graphics at two levels. Both render into an application surface
owned by the host; neither gives an application an EGL display, GLES context,
native framebuffer, HWC layer, gralloc handle, or physical scanout buffer.

```text
egui / imgui / LVGL / software canvas        2D / 3D engine backend
                 |                                      |
     graphics textures + indexed meshes       validated GLES2 resources
                 |                              + one command batch/frame
                 +------------------+-------------------+
                                    |
                         per-app handle namespace
                                    |
                       host-owned GLES render target
                                    |
                    RGB565 HWC buffer or local SDL window
```

## Portable 2D Path

The `graphics` WIT interface is the preferred path for GUI frameworks. A frame
contains premultiplied vertices, `u16` indices, texture handles, and clip
rectangles. This matches the output model used by egui and imgui. An LVGL
display port, J2ME emulator, video decoder, or software game can instead keep
a host texture and update only dirty rectangles before drawing one quad.

`oos-egui::Renderer` implements egui texture deltas and mesh submission. It
retains its frame vectors across frames, maps egui sampler options without
discarding them, and scales logical egui points to physical surface pixels.
`CanvasTexture` maps a user texture into egui and supports direct strided
updates, including RGB565. The renderer returns platform and viewport output
to the application integration instead of silently swallowing clipboard, URL,
IME, accessibility, or viewport requests.
An egui `PaintCallback` is rejected explicitly because callback objects are
backend-specific; custom GPU work must be implemented against the OOS GLES2
interface and scheduled by an adapter rather than silently discarded.

## GLES2 Path

The `gles` WIT interface is a programmable GLES 2.0 rendering path for engine
backends. Shader, program, buffer, and texture resources have application-local
integer handles. A frame is submitted as one list of fixed 36-byte commands,
plus an aligned `list<u32>` containing uniform values. Supported commands cover
viewport and scissor state, blending, depth, stencil, color masks, culling,
line and polygon-offset raster state, texture and buffer binding, vertex
attributes, uniforms, and indexed or non-indexed drawing.

The host validates enum values, resource ownership, buffer ranges, command
limits, and frame boundaries before dispatching GLES. `begin-frame` always
binds the host application target and `end-frame` presents that target. Depth
and stencil storage on the phones is allocated lazily only when a submitted
batch uses it, so a normal GUI does not pay their memory or initialization
cost.

This is deliberately a WIT rendering protocol, not passthrough OpenGL. Engine
ports should put resource creation outside the frame loop and translate their
render queue into one command batch. New GLES features must be added as typed
or validated resource/command operations while preserving the host-owned
target and per-app namespace.

The command arguments are part of the ABI. Unlisted argument slots are zero:

| Opcode | Arguments |
| --- | --- |
| `begin-frame` | `a0` clear mask (color=1, depth=2, stencil=4), `a1` little-endian RGBA8, `a2` depth f32 bits, `a3` stencil value |
| `viewport` | `a0..a3` x, y, width, height |
| `scissor` | `a0` enabled, `a1..a4` x, y, width, height |
| `blend` | `a0` enabled, `a1..a4` source/destination RGB/alpha factors, `a5..a6` RGB/alpha equations, `a7` little-endian constant RGBA8 |
| `depth` | `a0` test enabled, `a1` write enabled, `a2` compare function |
| `color-mask` | `a0..a3` red, green, blue, alpha write enables |
| `stencil` | `a0` test enabled, `a1..a2` front/back write masks |
| `stencil-function` | `a0` face, `a1` compare function, `a2` reference, `a3` mask |
| `stencil-operation` | `a0` face, `a1` stencil-fail, `a2` depth-fail, `a3` pass operations |
| `raster` | `a0` line-width f32 bits, `a1` polygon offset enabled, `a2..a3` factor/unit f32 bits, `a4` dither enabled, `a5..a6` near/far depth-range f32 bits |
| `cull` | `a0` enabled, `a1` cull face, `a2` front-face winding |
| `use-program` | `a0` program handle |
| `bind-texture` | `a0` texture unit, `a1` texture handle |
| `bind-vertex-buffer` | `a0` buffer handle |
| `bind-index-buffer` | `a0` buffer handle |
| `vertex-attribute` | `a0` location, `a1` components, `a2` type, `a3` normalized, `a4` stride, `a5` byte offset, `a6` enabled |
| `uniform` | `a0` location, `a1` uniform type, `a2` array count, `a3` u32-word offset in submit data |
| `draw-arrays` | `a0` primitive, `a1` first vertex, `a2` count |
| `draw-elements` | `a0` primitive, `a1` count, `a2` unsigned index type, `a3` byte offset |
| `end-frame` | no arguments; must be the last command |

Boolean command values must be zero or one. Signed integers use their two's
complement bit pattern; floating-point values use IEEE-754 bits. The generated
WIT enums define every non-Boolean discriminant. The Rust `oos_app::gles2`
module provides inline builders for all rows in this table.

## Pixel Formats

The true Nokia composition target is RGB565, but forcing every source texture
to RGB565 would break common UI and game content. OOS therefore reports the
surface format separately and supports these upload formats:

| Format | Bytes/pixel | Intended use |
| --- | ---: | --- |
| `a8` | 1 | font and mask atlases; sampled RGB is supplied as white |
| `rgb565` | 2 | opaque canvas, LVGL/J2ME framebuffers, opaque sprites |
| `rgba4444` | 2 | lower-memory sprites and UI assets that need alpha |
| `rgba8888` | 4 | egui color atlases and assets needing full alpha/color precision |

Alpha-bearing formats and vertex colors are premultiplied. Packed 16-bit
pixels use little-endian byte order. Applications query
`supported-texture-formats` rather than assuming a format. Texture flags select
independent minification/magnification and mipmap filters, repeat or mirrored
repeat per axis, mipmap generation, and full replacement. Row stride is
explicit, so a padded RGB565 framebuffer can be uploaded without repacking.

The conversion from an RGBA texture sample to the RGB565 phone target happens
in the GPU render/composition path. There is no reason to expand an RGB565
canvas to RGBA8888 in guest memory first. Conversely, converting alpha assets
to RGB565 would lose information and often add a separate mask texture or CPU
blend, increasing rather than reducing total cost.

## Copy And Allocation Rules

Bulk texture, vertex, index, shader, buffer, command, and uniform inputs are
validated in WAMR linear memory and passed directly to the display/executor.
Strided textures are uploaded row by row without a repack allocation. Uniform
payloads use aligned `u32` words so GLES can consume their storage directly on
ARM. The only per-frame host copy is the bounded command-record translation
that replaces guest-local resource handles with process-global host handles;
its vector capacity is retained between frames.

The egui adapter casts its `Color32` atlas slice directly to canonical RGBA
bytes; it does not flatten the atlas into a second vector. Guest adapters
should likewise retain their vectors, upload only dirty texture
regions, prefer A8 for font masks and RGB565 for opaque software canvases, and
avoid recreating shaders, programs, and buffers in the frame callback.
