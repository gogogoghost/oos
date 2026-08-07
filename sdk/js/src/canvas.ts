/// <reference path="./oos-native.d.ts" />

import {
  configure as configureNative,
  create as createNative,
  destroy as destroyNative,
  submit2d,
  type NativeCanvas2dCommand,
} from "oos:canvas";
import { MeshRenderingContext } from "./mesh";
import { WebGLRenderingContext } from "./webgl";

export interface CanvasGeometry {
  x?: number;
  y?: number;
  width: number;
  height: number;
  z?: number;
  visible?: boolean;
}

export type CanvasContextName = "2d" | "mesh2d" | "webgl";

function byte(value: number): number {
  return Math.max(0, Math.min(255, Math.round(value)));
}

export function rgba(red: number, green: number, blue: number, alpha = 255): number {
  return (
    byte(red) |
    (byte(green) << 8) |
    (byte(blue) << 16) |
    (byte(alpha) << 24)
  ) >>> 0;
}

export function parseColor(value: string | number): number {
  if (typeof value === "number") return value >>> 0;
  const named: Record<string, number> = {
    transparent: 0,
    black: rgba(0, 0, 0),
    white: rgba(255, 255, 255),
    red: rgba(255, 0, 0),
    green: rgba(0, 128, 0),
    blue: rgba(0, 0, 255),
  };
  if (value in named) return named[value];
  const match = /^#([0-9a-f]{6})([0-9a-f]{2})?$/i.exec(value);
  if (!match) throw new TypeError(`unsupported canvas color: ${value}`);
  const rgb = Number.parseInt(match[1], 16);
  const alpha = match[2] ? Number.parseInt(match[2], 16) : 255;
  return rgba(rgb >> 16, rgb >> 8, rgb, alpha);
}

export class CanvasRenderingContext2D {
  fillStyle: string | number = "black";
  strokeStyle: string | number = "black";
  lineWidth = 1;
  fontSize = 14;
  private commands: NativeCanvas2dCommand[] = [];

  constructor(private readonly owner: Canvas) {}

  beginFrame(clear: string | number = "transparent"): void {
    this.commands.length = 0;
    this.commands.push({
      op: "clear",
      x: 0,
      y: 0,
      width: this.owner.width,
      height: this.owner.height,
      rgba: parseColor(clear),
    });
  }

  clearRect(x: number, y: number, width: number, height: number): void {
    if (x !== 0 || y !== 0 || width !== this.owner.width || height !== this.owner.height) {
      throw new RangeError("the OOS Canvas2D batch currently clears complete canvases");
    }
    this.commands.push({ op: "clear", x, y, width, height, rgba: 0 });
  }

  fillRect(x: number, y: number, width: number, height: number, radius = 0): void {
    this.commands.push({
      op: "fillRect",
      x,
      y,
      width,
      height,
      radius,
      rgba: parseColor(this.fillStyle),
    });
  }

  strokeRect(x: number, y: number, width: number, height: number, radius = 0): void {
    this.commands.push({
      op: "strokeRect",
      x,
      y,
      width,
      height,
      radius,
      lineWidth: this.lineWidth,
      rgba: parseColor(this.strokeStyle),
    });
  }

  fillText(text: string, x: number, baselineY: number, maxWidth = 0): void {
    this.commands.push({
      op: "fillText",
      text,
      x,
      y: baselineY,
      width: maxWidth,
      fontSize: this.fontSize,
      rgba: parseColor(this.fillStyle),
    });
  }

  pushClip(x: number, y: number, width: number, height: number): void {
    this.commands.push({ op: "pushClip", x, y, width, height });
  }

  popClip(): void {
    this.commands.push({ op: "popClip" });
  }

  flush(): void {
    submit2d(this.owner.handle, this.commands);
    this.commands.length = 0;
  }
}

export class Canvas {
  readonly handle: number;
  readonly context: CanvasContextName;
  x: number;
  y: number;
  width: number;
  height: number;
  z: number;
  visible: boolean;
  private context2d?: CanvasRenderingContext2D;
  private meshContext?: MeshRenderingContext;
  private webglContext?: WebGLRenderingContext;
  private destroyed = false;

  constructor(context: CanvasContextName, geometry: CanvasGeometry) {
    this.context = context;
    this.x = geometry.x ?? 0;
    this.y = geometry.y ?? 0;
    this.width = geometry.width;
    this.height = geometry.height;
    this.z = geometry.z ?? 0;
    this.visible = geometry.visible ?? true;
    this.handle = createNative({
      context,
      x: this.x,
      y: this.y,
      width: this.width,
      height: this.height,
      z: this.z,
      visible: this.visible,
    });
  }

  getContext(name: "2d"): CanvasRenderingContext2D;
  getContext(name: "mesh2d"): MeshRenderingContext;
  getContext(name: "webgl"): WebGLRenderingContext;
  getContext(name: CanvasContextName):
    | CanvasRenderingContext2D
    | MeshRenderingContext
    | WebGLRenderingContext
    | null;
  getContext(name: CanvasContextName):
    | CanvasRenderingContext2D
    | MeshRenderingContext
    | WebGLRenderingContext
    | null {
    this.assertLive();
    if (name !== this.context) return null;
    if (name === "2d") return (this.context2d ??= new CanvasRenderingContext2D(this));
    if (name === "mesh2d") return (this.meshContext ??= new MeshRenderingContext(this));
    return (this.webglContext ??= new WebGLRenderingContext(this));
  }

  configure(geometry: Partial<CanvasGeometry>): void {
    this.assertLive();
    this.x = geometry.x ?? this.x;
    this.y = geometry.y ?? this.y;
    this.width = geometry.width ?? this.width;
    this.height = geometry.height ?? this.height;
    this.z = geometry.z ?? this.z;
    this.visible = geometry.visible ?? this.visible;
    configureNative(this.handle, {
      x: this.x,
      y: this.y,
      width: this.width,
      height: this.height,
      z: this.z,
      visible: this.visible,
    });
  }

  destroy(): void {
    if (this.destroyed) return;
    destroyNative(this.handle);
    this.destroyed = true;
  }

  private assertLive(): void {
    if (this.destroyed) throw new Error("canvas has been destroyed");
  }
}

export function createCanvas(
  context: CanvasContextName,
  geometry: CanvasGeometry,
): Canvas {
  return new Canvas(context, geometry);
}
