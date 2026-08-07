/// <reference path="./oos-native.d.ts" />

import {
  graphicsLimits,
  pixelsPerPoint,
  submit,
  supportedTextureFormats,
  surfaceFormat,
  surfaceSize,
  textureFree,
  textureSet,
  type GraphicsLimits,
  type SurfaceSize,
} from "oos:graphics";
import type {
  NativeMeshCommand,
  NativeMeshVertex,
  NativeTextureDescriptor,
} from "oos:canvas";

export { TextureFlags, TextureFormat } from "./mesh";
export type { GraphicsLimits, SurfaceSize };

// Direct whole-surface mesh access for LVGL/egui-style ports. Mixed UI
// applications normally create a mesh2d Canvas instead.
export const rootGraphics = {
  surfaceSize,
  pixelsPerPoint,
  surfaceFormat,
  supportedTextureFormats,
  graphicsLimits,
  setTexture(
    texture: number,
    descriptor: NativeTextureDescriptor,
    pixels: ArrayBuffer | ArrayBufferView,
  ): void {
    textureSet(texture, descriptor, pixels);
  },
  deleteTexture(texture: number): void {
    textureFree(texture);
  },
  submit(
    vertices: NativeMeshVertex[],
    indices: Uint16Array,
    commands: NativeMeshCommand[],
    clearRgba = 0xff000000,
  ): void {
    submit(vertices, indices, commands, clearRgba >>> 0);
  },
};

export type RootGraphics = typeof rootGraphics;
