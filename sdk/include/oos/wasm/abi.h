#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OOS_WASM_ABI_VERSION 1u
#define OOS_GFX_MAX_TEXTURE_SIZE 2048u
#define OOS_GFX_MAX_TEXTURE_BYTES (16u * 1024u * 1024u)
#define OOS_GFX_MAX_VERTICES 65535u
#define OOS_GFX_MAX_INDICES 196605u
#define OOS_GFX_MAX_DRAW_COMMANDS 4096u

enum OosTextureFlags {
  OOS_TEXTURE_LINEAR = 1u << 0,
  OOS_TEXTURE_REPLACE = 1u << 1,
};

typedef struct OosGfxVertex {
  float position[2];
  float uv[2];
  uint8_t color[4];
} OosGfxVertex;

typedef struct OosGfxDrawCommand {
  uint32_t first_index;
  uint32_t index_count;
  uint32_t texture;
  float clip_min[2];
  float clip_max[2];
} OosGfxDrawCommand;

#ifdef __cplusplus
}

static_assert(sizeof(OosGfxVertex) == 20, "OosGfxVertex ABI changed");
static_assert(sizeof(OosGfxDrawCommand) == 28, "OosGfxDrawCommand ABI changed");
#endif
