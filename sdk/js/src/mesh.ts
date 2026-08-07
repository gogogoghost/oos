import {
  submitMesh,
  textureFree,
  textureSet,
  type NativeMeshCommand,
  type NativeMeshVertex,
  type NativeTextureDescriptor,
} from "oos:canvas";

export const TextureFormat = {
  A8: 0,
  RGB565: 1,
  RGBA4444: 2,
  RGBA8888: 3,
} as const;

export const TextureFlags = {
  LinearMinification: 1 << 0,
  LinearMagnification: 1 << 1,
  Replace: 1 << 2,
  RepeatX: 1 << 3,
  RepeatY: 1 << 4,
  MirroredRepeatX: 1 << 5,
  MirroredRepeatY: 1 << 6,
  Mipmaps: 1 << 7,
  LinearMipmaps: 1 << 8,
} as const;

export type MeshVertex = NativeMeshVertex;
export type MeshDrawCommand = NativeMeshCommand;
export type TextureDescriptor = NativeTextureDescriptor;

export interface MeshCanvasOwner {
  readonly handle: number;
}

export class MeshRenderingContext {
  constructor(private readonly owner: MeshCanvasOwner) {}

  setTexture(
    texture: number,
    descriptor: TextureDescriptor,
    pixels: ArrayBuffer | ArrayBufferView,
  ): void {
    textureSet(this.owner.handle, texture, descriptor, pixels);
  }

  deleteTexture(texture: number): void {
    textureFree(this.owner.handle, texture);
  }

  submit(
    vertices: MeshVertex[],
    indices: Uint16Array,
    commands: MeshDrawCommand[],
    clearRgba = 0xff000000,
  ): void {
    submitMesh(this.owner.handle, vertices, indices, commands, clearRgba >>> 0);
  }
}
