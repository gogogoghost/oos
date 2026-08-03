#pragma once

#include <cstdint>

// Internal mirror of the canonical layouts defined by sdk/wit/oos.wit.
// Guest applications use generated WIT bindings and never include this header.
#define OOS_WASM_ABI_VERSION 5u
#define OOS_GFX_MAX_TEXTURE_SIZE 2048u
#define OOS_GFX_MAX_TEXTURE_BYTES (16u * 1024u * 1024u)
#define OOS_GFX_MAX_VERTICES 65535u
#define OOS_GFX_MAX_INDICES 196605u
#define OOS_GFX_MAX_DRAW_COMMANDS 4096u
#define OOS_GLES_MAX_BUFFER_BYTES (16u * 1024u * 1024u)
#define OOS_GLES_MAX_SHADER_BYTES (64u * 1024u)
#define OOS_GLES_MAX_COMMANDS 4096u
#define OOS_GLES_MAX_COMMAND_DATA_WORDS (16u * 1024u)

enum OosTextureFormat : uint32_t {
  OOS_TEXTURE_A8 = 0,
  OOS_TEXTURE_RGB565 = 1,
  OOS_TEXTURE_RGBA4444 = 2,
  OOS_TEXTURE_RGBA8888 = 3,
};

constexpr uint32_t OOS_TEXTURE_FORMAT_MASK =
    (1u << OOS_TEXTURE_A8) | (1u << OOS_TEXTURE_RGB565) |
    (1u << OOS_TEXTURE_RGBA4444) | (1u << OOS_TEXTURE_RGBA8888);

constexpr uint32_t oosTextureBytesPerPixel(uint32_t format) {
  switch (format) {
  case OOS_TEXTURE_A8:
    return 1;
  case OOS_TEXTURE_RGB565:
  case OOS_TEXTURE_RGBA4444:
    return 2;
  case OOS_TEXTURE_RGBA8888:
    return 4;
  default:
    return 0;
  }
}

enum OosTextureFlags {
  OOS_TEXTURE_LINEAR_MINIFICATION = 1u << 0,
  OOS_TEXTURE_LINEAR_MAGNIFICATION = 1u << 1,
  OOS_TEXTURE_REPLACE = 1u << 2,
  OOS_TEXTURE_REPEAT_X = 1u << 3,
  OOS_TEXTURE_REPEAT_Y = 1u << 4,
  OOS_TEXTURE_MIRRORED_REPEAT_X = 1u << 5,
  OOS_TEXTURE_MIRRORED_REPEAT_Y = 1u << 6,
  OOS_TEXTURE_MIPMAPS = 1u << 7,
  OOS_TEXTURE_LINEAR_MIPMAPS = 1u << 8,
};

constexpr uint32_t OOS_TEXTURE_FLAGS_MASK =
    OOS_TEXTURE_LINEAR_MINIFICATION | OOS_TEXTURE_LINEAR_MAGNIFICATION |
    OOS_TEXTURE_REPLACE | OOS_TEXTURE_REPEAT_X | OOS_TEXTURE_REPEAT_Y |
    OOS_TEXTURE_MIRRORED_REPEAT_X | OOS_TEXTURE_MIRRORED_REPEAT_Y |
    OOS_TEXTURE_MIPMAPS | OOS_TEXTURE_LINEAR_MIPMAPS;

struct OosGfxVertex {
  float position[2];
  float uv[2];
  uint8_t color[4];
};

struct OosGfxDrawCommand {
  uint32_t first_index;
  uint32_t index_count;
  uint32_t texture;
  float clip_min[2];
  float clip_max[2];
};

enum OosGlesOpcode : uint32_t {
  OOS_GLES_BEGIN_FRAME = 0,
  OOS_GLES_VIEWPORT = 1,
  OOS_GLES_SCISSOR = 2,
  OOS_GLES_BLEND = 3,
  OOS_GLES_DEPTH = 4,
  OOS_GLES_COLOR_MASK = 5,
  OOS_GLES_STENCIL = 6,
  OOS_GLES_STENCIL_FUNCTION = 7,
  OOS_GLES_STENCIL_OPERATION = 8,
  OOS_GLES_RASTER = 9,
  OOS_GLES_CULL = 10,
  OOS_GLES_USE_PROGRAM = 11,
  OOS_GLES_BIND_TEXTURE = 12,
  OOS_GLES_BIND_VERTEX_BUFFER = 13,
  OOS_GLES_BIND_INDEX_BUFFER = 14,
  OOS_GLES_VERTEX_ATTRIBUTE = 15,
  OOS_GLES_UNIFORM = 16,
  OOS_GLES_DRAW_ARRAYS = 17,
  OOS_GLES_DRAW_ELEMENTS = 18,
  OOS_GLES_END_FRAME = 19,
};

enum OosGlesClearMask : uint32_t {
  OOS_GLES_CLEAR_COLOR = 1u << 0,
  OOS_GLES_CLEAR_DEPTH = 1u << 1,
  OOS_GLES_CLEAR_STENCIL = 1u << 2,
};

enum OosGlesBufferUsage : uint32_t {
  OOS_GLES_STATIC_DRAW = 0,
  OOS_GLES_DYNAMIC_DRAW = 1,
  OOS_GLES_STREAM_DRAW = 2,
};

enum OosGlesShaderStage : uint32_t {
  OOS_GLES_VERTEX_SHADER = 0,
  OOS_GLES_FRAGMENT_SHADER = 1,
};

enum OosGlesPrimitive : uint32_t {
  OOS_GLES_POINTS = 0,
  OOS_GLES_LINES = 1,
  OOS_GLES_LINE_STRIP = 2,
  OOS_GLES_TRIANGLES = 3,
  OOS_GLES_TRIANGLE_STRIP = 4,
  OOS_GLES_TRIANGLE_FAN = 5,
};

enum OosGlesVertexType : uint32_t {
  OOS_GLES_F32 = 0,
  OOS_GLES_U8 = 1,
  OOS_GLES_I8 = 2,
  OOS_GLES_U16 = 3,
  OOS_GLES_I16 = 4,
};

enum OosGlesBlendFactor : uint32_t {
  OOS_GLES_BLEND_ZERO = 0,
  OOS_GLES_BLEND_ONE = 1,
  OOS_GLES_BLEND_SRC_COLOR = 2,
  OOS_GLES_BLEND_ONE_MINUS_SRC_COLOR = 3,
  OOS_GLES_BLEND_DST_COLOR = 4,
  OOS_GLES_BLEND_ONE_MINUS_DST_COLOR = 5,
  OOS_GLES_BLEND_SRC_ALPHA = 6,
  OOS_GLES_BLEND_ONE_MINUS_SRC_ALPHA = 7,
  OOS_GLES_BLEND_DST_ALPHA = 8,
  OOS_GLES_BLEND_ONE_MINUS_DST_ALPHA = 9,
  OOS_GLES_BLEND_CONSTANT_COLOR = 10,
  OOS_GLES_BLEND_ONE_MINUS_CONSTANT_COLOR = 11,
  OOS_GLES_BLEND_CONSTANT_ALPHA = 12,
  OOS_GLES_BLEND_ONE_MINUS_CONSTANT_ALPHA = 13,
  OOS_GLES_BLEND_SRC_ALPHA_SATURATE = 14,
};

enum OosGlesBlendEquation : uint32_t {
  OOS_GLES_BLEND_ADD = 0,
  OOS_GLES_BLEND_SUBTRACT = 1,
  OOS_GLES_BLEND_REVERSE_SUBTRACT = 2,
};

enum OosGlesCompareFunction : uint32_t {
  OOS_GLES_COMPARE_NEVER = 0,
  OOS_GLES_COMPARE_LESS = 1,
  OOS_GLES_COMPARE_EQUAL = 2,
  OOS_GLES_COMPARE_LESS_EQUAL = 3,
  OOS_GLES_COMPARE_GREATER = 4,
  OOS_GLES_COMPARE_NOT_EQUAL = 5,
  OOS_GLES_COMPARE_GREATER_EQUAL = 6,
  OOS_GLES_COMPARE_ALWAYS = 7,
};

enum OosGlesCullFace : uint32_t {
  OOS_GLES_CULL_FRONT = 0,
  OOS_GLES_CULL_BACK = 1,
  OOS_GLES_CULL_FRONT_AND_BACK = 2,
};

enum OosGlesFrontFace : uint32_t {
  OOS_GLES_FRONT_CCW = 0,
  OOS_GLES_FRONT_CW = 1,
};

enum OosGlesStencilOperation : uint32_t {
  OOS_GLES_STENCIL_KEEP = 0,
  OOS_GLES_STENCIL_ZERO = 1,
  OOS_GLES_STENCIL_REPLACE = 2,
  OOS_GLES_STENCIL_INCREMENT_CLAMP = 3,
  OOS_GLES_STENCIL_DECREMENT_CLAMP = 4,
  OOS_GLES_STENCIL_INVERT = 5,
  OOS_GLES_STENCIL_INCREMENT_WRAP = 6,
  OOS_GLES_STENCIL_DECREMENT_WRAP = 7,
};

enum OosGlesUniformType : uint32_t {
  OOS_GLES_UNIFORM_I1 = 0,
  OOS_GLES_UNIFORM_F1 = 1,
  OOS_GLES_UNIFORM_F2 = 2,
  OOS_GLES_UNIFORM_F3 = 3,
  OOS_GLES_UNIFORM_F4 = 4,
  OOS_GLES_UNIFORM_MAT2 = 5,
  OOS_GLES_UNIFORM_MAT3 = 6,
  OOS_GLES_UNIFORM_MAT4 = 7,
};

// Fixed-width command record used by the batched WIT GLES submission. Float
// arguments are passed as IEEE-754 bits. Variable uniform payloads reference
// the submission's aligned u32 data list by word offset.
struct OosGlesCommand {
  uint8_t opcode;
  uint8_t reserved[3];
  uint32_t args[8];
};

struct OosGlesCapabilities {
  uint32_t major_version;
  uint32_t minor_version;
  uint32_t max_texture_size;
  uint32_t max_texture_units;
  uint32_t max_vertex_attributes;
  uint32_t max_varying_vectors;
  uint32_t max_vertex_uniform_vectors;
  uint32_t max_fragment_uniform_vectors;
  uint32_t depth_bits;
  uint32_t stencil_bits;
  uint32_t max_buffer_bytes;
  uint32_t max_commands;
  uint32_t max_command_data_words;
};

static_assert(sizeof(OosGfxVertex) == 20, "WIT graphics vertex layout changed");
static_assert(sizeof(OosGfxDrawCommand) == 28,
              "WIT graphics draw-command layout changed");
static_assert(sizeof(OosGlesCommand) == 36, "WIT GLES command layout changed");
static_assert(sizeof(OosGlesCapabilities) == 52,
              "WIT GLES capabilities layout changed");
