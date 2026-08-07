/// <reference path="./oos-native.d.ts" />

import {
  attributeLocation,
  bufferFree,
  bufferSet,
  bufferWrite,
  capabilities,
  programFree,
  programSet,
  shaderFree,
  shaderSet,
  submit,
  textureFree,
  textureSet,
  uniformLocation,
  type NativeGlesCapabilities,
  type NativeGlesCommand,
} from "oos:gles";

interface WebGLCanvasOwner {
  readonly handle: number;
  readonly width: number;
  readonly height: number;
}

class Handle {
  deleted = false;
  constructor(readonly id: number) {}
}

export class WebGLBuffer extends Handle {}

export class WebGLTexture extends Handle {
  flags = (1 << 0) | (1 << 1) | (1 << 2);
}

export class WebGLShader extends Handle {
  source = "";
  compiled = false;
  constructor(id: number, readonly type: number) {
    super(id);
  }
}

export class WebGLProgram extends Handle {
  vertex?: WebGLShader;
  fragment?: WebGLShader;
  linked = false;
}

export class WebGLUniformLocation {
  constructor(readonly value: number, readonly program: WebGLProgram) {}
}

interface AttributeState {
  components: number;
  type: number;
  normalized: boolean;
  stride: number;
  offset: number;
  enabled: boolean;
}

const floatScratch = new ArrayBuffer(4);
const floatScratchView = new DataView(floatScratch);

function floatBits(value: number): number {
  if (!Number.isFinite(value)) throw new RangeError("WebGL values must be finite");
  floatScratchView.setFloat32(0, value, true);
  return floatScratchView.getUint32(0, true);
}

function byteLength(value: ArrayBuffer | ArrayBufferView): number {
  return value.byteLength;
}

function requireLive<T extends Handle>(value: T | null, name: string): T {
  if (!value || value.deleted) throw new TypeError(`${name} is null or deleted`);
  return value;
}

export class WebGLRenderingContext {
  readonly DEPTH_BUFFER_BIT = 0x0100;
  readonly STENCIL_BUFFER_BIT = 0x0400;
  readonly COLOR_BUFFER_BIT = 0x4000;
  readonly POINTS = 0x0000;
  readonly LINES = 0x0001;
  readonly LINE_STRIP = 0x0003;
  readonly TRIANGLES = 0x0004;
  readonly TRIANGLE_STRIP = 0x0005;
  readonly TRIANGLE_FAN = 0x0006;
  readonly ZERO = 0;
  readonly ONE = 1;
  readonly SRC_ALPHA = 0x0302;
  readonly ONE_MINUS_SRC_ALPHA = 0x0303;
  readonly ARRAY_BUFFER = 0x8892;
  readonly ELEMENT_ARRAY_BUFFER = 0x8893;
  readonly STREAM_DRAW = 0x88e0;
  readonly STATIC_DRAW = 0x88e4;
  readonly DYNAMIC_DRAW = 0x88e8;
  readonly FLOAT = 0x1406;
  readonly UNSIGNED_BYTE = 0x1401;
  readonly BYTE = 0x1400;
  readonly UNSIGNED_SHORT = 0x1403;
  readonly SHORT = 0x1402;
  readonly VERTEX_SHADER = 0x8b31;
  readonly FRAGMENT_SHADER = 0x8b30;
  readonly COMPILE_STATUS = 0x8b81;
  readonly LINK_STATUS = 0x8b82;
  readonly TEXTURE_2D = 0x0de1;
  readonly TEXTURE0 = 0x84c0;
  readonly RGBA = 0x1908;
  readonly NEAREST = 0x2600;
  readonly LINEAR = 0x2601;
  readonly REPEAT = 0x2901;
  readonly CLAMP_TO_EDGE = 0x812f;
  readonly MIRRORED_REPEAT = 0x8370;
  readonly TEXTURE_MAG_FILTER = 0x2800;
  readonly TEXTURE_MIN_FILTER = 0x2801;
  readonly TEXTURE_WRAP_S = 0x2802;
  readonly TEXTURE_WRAP_T = 0x2803;
  readonly BLEND = 0x0be2;
  readonly CULL_FACE = 0x0b44;
  readonly DEPTH_TEST = 0x0b71;
  readonly SCISSOR_TEST = 0x0c11;
  readonly STENCIL_TEST = 0x0b90;
  readonly MAX_TEXTURE_SIZE = 0x0d33;
  readonly MAX_TEXTURE_IMAGE_UNITS = 0x8872;
  readonly MAX_VERTEX_ATTRIBS = 0x8869;

  readonly capabilities: NativeGlesCapabilities;
  private nextHandle = 1;
  private commands: NativeGlesCommand[] = [];
  private words: number[] = [];
  private clearMask = 1;
  private clearRgba = 0;
  private clearDepthValue = 1;
  private clearStencilValue = 0;
  private arrayBuffer: WebGLBuffer | null = null;
  private indexBuffer: WebGLBuffer | null = null;
  private textureUnit = 0;
  private boundTextures = new Map<number, WebGLTexture>();
  private attributes = new Map<number, AttributeState>();
  private scissorBox: [number, number, number, number] = [0, 0, 0, 0];

  constructor(private readonly owner: WebGLCanvasOwner) {
    this.capabilities = capabilities(owner.handle);
    this.beginFrame();
  }

  beginFrame(): void {
    this.commands.length = 0;
    this.words.length = 0;
    this.clearMask = 1;
    this.clearRgba = 0;
    this.clearDepthValue = 1;
    this.clearStencilValue = 0;
    this.viewport(0, 0, this.owner.width, this.owner.height);
  }

  flush(): void {
    const begin: NativeGlesCommand = {
      op: 0,
      a0: this.clearMask,
      a1: this.clearRgba,
      a2: floatBits(this.clearDepthValue),
      a3: this.clearStencilValue >>> 0,
    };
    submit(this.owner.handle, [begin, ...this.commands, { op: 19 }], new Uint32Array(this.words));
    this.beginFrame();
  }

  createBuffer(): WebGLBuffer {
    return new WebGLBuffer(this.allocateHandle());
  }

  deleteBuffer(buffer: WebGLBuffer | null): void {
    if (!buffer || buffer.deleted) return;
    bufferFree(this.owner.handle, buffer.id);
    buffer.deleted = true;
    if (this.arrayBuffer === buffer) this.arrayBuffer = null;
    if (this.indexBuffer === buffer) this.indexBuffer = null;
  }

  bindBuffer(target: number, buffer: WebGLBuffer | null): void {
    const handle = buffer ? requireLive(buffer, "buffer").id : 0;
    if (target === this.ARRAY_BUFFER) {
      this.arrayBuffer = buffer;
      if (handle) this.command(13, handle);
    } else if (target === this.ELEMENT_ARRAY_BUFFER) {
      this.indexBuffer = buffer;
      if (handle) this.command(14, handle);
    } else {
      throw new RangeError("unsupported WebGL buffer target");
    }
  }

  bufferData(
    target: number,
    source: ArrayBuffer | ArrayBufferView | number,
    usage: number,
  ): void {
    const buffer = this.boundBuffer(target);
    const nativeUsage = this.bufferUsage(usage);
    if (typeof source === "number") {
      if (!Number.isInteger(source) || source <= 0)
        throw new RangeError("buffer size must be a positive integer");
      bufferSet(this.owner.handle, buffer.id, source, nativeUsage, new Uint8Array());
    } else {
      bufferSet(this.owner.handle, buffer.id, byteLength(source), nativeUsage, source);
    }
  }

  bufferSubData(target: number, offset: number, data: ArrayBuffer | ArrayBufferView): void {
    if (!Number.isInteger(offset) || offset < 0)
      throw new RangeError("buffer offset must be non-negative");
    bufferWrite(this.owner.handle, this.boundBuffer(target).id, offset, data);
  }

  createShader(type: number): WebGLShader {
    if (type !== this.VERTEX_SHADER && type !== this.FRAGMENT_SHADER)
      throw new RangeError("unsupported shader type");
    return new WebGLShader(this.allocateHandle(), type);
  }

  shaderSource(shader: WebGLShader, source: string): void {
    requireLive(shader, "shader").source = String(source);
    shader.compiled = false;
  }

  compileShader(shader: WebGLShader): void {
    requireLive(shader, "shader");
    if (!shader.source) throw new TypeError("shader source is empty");
    shaderSet(
      this.owner.handle,
      shader.id,
      shader.type === this.VERTEX_SHADER ? 0 : 1,
      shader.source,
    );
    shader.compiled = true;
  }

  getShaderParameter(shader: WebGLShader, parameter: number): boolean {
    requireLive(shader, "shader");
    if (parameter !== this.COMPILE_STATUS) throw new RangeError("unsupported shader parameter");
    return shader.compiled;
  }

  getShaderInfoLog(shader: WebGLShader): string {
    requireLive(shader, "shader");
    return "";
  }

  deleteShader(shader: WebGLShader | null): void {
    if (!shader || shader.deleted) return;
    shaderFree(this.owner.handle, shader.id);
    shader.deleted = true;
  }

  createProgram(): WebGLProgram {
    return new WebGLProgram(this.allocateHandle());
  }

  attachShader(program: WebGLProgram, shader: WebGLShader): void {
    requireLive(program, "program");
    requireLive(shader, "shader");
    if (shader.type === this.VERTEX_SHADER) program.vertex = shader;
    else program.fragment = shader;
    program.linked = false;
  }

  linkProgram(program: WebGLProgram): void {
    requireLive(program, "program");
    const vertex = requireLive(program.vertex ?? null, "vertex shader");
    const fragment = requireLive(program.fragment ?? null, "fragment shader");
    if (!vertex.compiled || !fragment.compiled)
      throw new Error("attached shaders must be compiled before linking");
    programSet(this.owner.handle, program.id, vertex.id, fragment.id);
    program.linked = true;
  }

  getProgramParameter(program: WebGLProgram, parameter: number): boolean {
    requireLive(program, "program");
    if (parameter !== this.LINK_STATUS) throw new RangeError("unsupported program parameter");
    return program.linked;
  }

  getProgramInfoLog(program: WebGLProgram): string {
    requireLive(program, "program");
    return "";
  }

  useProgram(program: WebGLProgram | null): void {
    const live = requireLive(program, "program");
    if (!live.linked) throw new Error("program is not linked");
    this.command(11, live.id);
  }

  deleteProgram(program: WebGLProgram | null): void {
    if (!program || program.deleted) return;
    programFree(this.owner.handle, program.id);
    program.deleted = true;
  }

  getAttribLocation(program: WebGLProgram, name: string): number {
    return attributeLocation(this.owner.handle, requireLive(program, "program").id, name);
  }

  getUniformLocation(program: WebGLProgram, name: string): WebGLUniformLocation | null {
    const live = requireLive(program, "program");
    const value = uniformLocation(this.owner.handle, live.id, name);
    return value < 0 ? null : new WebGLUniformLocation(value, live);
  }

  createTexture(): WebGLTexture {
    return new WebGLTexture(this.allocateHandle());
  }

  deleteTexture(texture: WebGLTexture | null): void {
    if (!texture || texture.deleted) return;
    textureFree(this.owner.handle, texture.id);
    texture.deleted = true;
    for (const [unit, bound] of this.boundTextures) {
      if (bound === texture) this.boundTextures.delete(unit);
    }
  }

  activeTexture(texture: number): void {
    const unit = texture - this.TEXTURE0;
    if (!Number.isInteger(unit) || unit < 0 || unit >= this.capabilities.maxTextureUnits)
      throw new RangeError("texture unit is out of range");
    this.textureUnit = unit;
  }

  bindTexture(target: number, texture: WebGLTexture | null): void {
    if (target !== this.TEXTURE_2D) throw new RangeError("only TEXTURE_2D is supported");
    if (!texture) throw new TypeError("unbinding textures is not supported inside a frame");
    const live = requireLive(texture, "texture");
    this.boundTextures.set(this.textureUnit, live);
    this.command(12, this.textureUnit, live.id);
  }

  texParameteri(target: number, parameter: number, value: number): void {
    if (target !== this.TEXTURE_2D) throw new RangeError("only TEXTURE_2D is supported");
    const texture = this.boundTexture();
    if (parameter === this.TEXTURE_MIN_FILTER) {
      texture.flags = value === this.LINEAR ? texture.flags | 1 : texture.flags & ~1;
    } else if (parameter === this.TEXTURE_MAG_FILTER) {
      texture.flags = value === this.LINEAR ? texture.flags | 2 : texture.flags & ~2;
    } else if (parameter === this.TEXTURE_WRAP_S) {
      texture.flags = this.wrapFlags(texture.flags, value, 3, 5);
    } else if (parameter === this.TEXTURE_WRAP_T) {
      texture.flags = this.wrapFlags(texture.flags, value, 4, 6);
    } else {
      throw new RangeError("unsupported texture parameter");
    }
  }

  texImage2D(
    target: number,
    level: number,
    internalFormat: number,
    width: number,
    height: number,
    border: number,
    format: number,
    type: number,
    pixels: ArrayBuffer | ArrayBufferView | null,
  ): void {
    this.validateTextureArguments(target, level, internalFormat, border, format, type);
    const data = pixels ?? new Uint8Array(width * height * 4);
    textureSet(this.owner.handle, this.boundTexture().id, {
      format: 3,
      x: 0,
      y: 0,
      width,
      height,
      rowStride: width * 4,
      flags: this.boundTexture().flags | 4,
    }, data);
  }

  texSubImage2D(
    target: number,
    level: number,
    x: number,
    y: number,
    width: number,
    height: number,
    format: number,
    type: number,
    pixels: ArrayBuffer | ArrayBufferView,
  ): void {
    this.validateTextureArguments(target, level, format, 0, format, type);
    const texture = this.boundTexture();
    textureSet(this.owner.handle, texture.id, {
      format: 3,
      x,
      y,
      width,
      height,
      rowStride: width * 4,
      flags: texture.flags & ~4,
    }, pixels);
  }

  viewport(x: number, y: number, width: number, height: number): void {
    this.command(1, x, y, width, height);
  }

  scissor(x: number, y: number, width: number, height: number): void {
    this.scissorBox = [x, y, width, height];
  }

  clearColor(red: number, green: number, blue: number, alpha: number): void {
    const channel = (value: number): number =>
      Math.max(0, Math.min(255, Math.round(value * 255)));
    this.clearRgba = (channel(red) | (channel(green) << 8) |
      (channel(blue) << 16) | (channel(alpha) << 24)) >>> 0;
  }

  clearDepth(value: number): void {
    this.clearDepthValue = value;
  }

  clearStencil(value: number): void {
    this.clearStencilValue = value | 0;
  }

  clear(mask: number): void {
    let nativeMask = 0;
    if (mask & this.COLOR_BUFFER_BIT) nativeMask |= 1;
    if (mask & this.DEPTH_BUFFER_BIT) nativeMask |= 2;
    if (mask & this.STENCIL_BUFFER_BIT) nativeMask |= 4;
    if (mask & ~(this.COLOR_BUFFER_BIT | this.DEPTH_BUFFER_BIT | this.STENCIL_BUFFER_BIT))
      throw new RangeError("unsupported clear mask");
    this.clearMask = nativeMask;
  }

  enable(capability: number): void {
    this.setCapability(capability, true);
  }

  disable(capability: number): void {
    this.setCapability(capability, false);
  }

  blendFunc(source: number, destination: number): void {
    this.command(3, 1, this.blendFactor(source), this.blendFactor(destination),
      this.blendFactor(source), this.blendFactor(destination), 0, 0, 0);
  }

  vertexAttribPointer(
    location: number,
    components: number,
    type: number,
    normalized: boolean,
    stride: number,
    offset: number,
  ): void {
    this.boundBuffer(this.ARRAY_BUFFER);
    const state: AttributeState = {
      components,
      type: this.vertexType(type),
      normalized,
      stride,
      offset,
      enabled: this.attributes.get(location)?.enabled ?? false,
    };
    this.attributes.set(location, state);
    if (state.enabled) this.emitAttribute(location, state);
  }

  enableVertexAttribArray(location: number): void {
    const state = this.attributes.get(location);
    if (!state) throw new Error("vertexAttribPointer must be called before enabling an attribute");
    state.enabled = true;
    this.emitAttribute(location, state);
  }

  disableVertexAttribArray(location: number): void {
    const state = this.attributes.get(location);
    if (state) state.enabled = false;
    this.command(15, location, 0, 0, 0, 0, 0, 0);
  }

  uniform1i(location: WebGLUniformLocation | null, value: number): void {
    if (!location) return;
    this.uniform(location, 0, [value | 0]);
  }

  uniform1f(location: WebGLUniformLocation | null, value: number): void {
    if (!location) return;
    this.uniform(location, 1, [floatBits(value)]);
  }

  uniform2fv(location: WebGLUniformLocation | null, value: ArrayLike<number>): void {
    this.uniformFloats(location, 2, 2, value);
  }

  uniform3fv(location: WebGLUniformLocation | null, value: ArrayLike<number>): void {
    this.uniformFloats(location, 3, 3, value);
  }

  uniform4fv(location: WebGLUniformLocation | null, value: ArrayLike<number>): void {
    this.uniformFloats(location, 4, 4, value);
  }

  uniformMatrix4fv(
    location: WebGLUniformLocation | null,
    transpose: boolean,
    value: ArrayLike<number>,
  ): void {
    if (transpose) throw new RangeError("WebGL matrices cannot be transposed");
    this.uniformFloats(location, 7, 16, value);
  }

  drawArrays(mode: number, first: number, count: number): void {
    this.command(17, this.primitive(mode), first, count);
  }

  drawElements(mode: number, count: number, type: number, offset: number): void {
    this.boundBuffer(this.ELEMENT_ARRAY_BUFFER);
    this.command(18, this.primitive(mode), count, this.vertexType(type), offset);
  }

  getParameter(parameter: number): number {
    if (parameter === this.MAX_TEXTURE_SIZE) return this.capabilities.maxTextureSize;
    if (parameter === this.MAX_TEXTURE_IMAGE_UNITS) return this.capabilities.maxTextureUnits;
    if (parameter === this.MAX_VERTEX_ATTRIBS) return this.capabilities.maxVertexAttributes;
    throw new RangeError("unsupported WebGL parameter");
  }

  getExtension(_name: string): null {
    return null;
  }

  private allocateHandle(): number {
    const result = this.nextHandle++;
    if (result === 0) throw new RangeError("WebGL resource handle space exhausted");
    return result;
  }

  private command(op: number, ...args: number[]): void {
    const command: NativeGlesCommand = { op };
    for (let index = 0; index < Math.min(args.length, 8); ++index) {
      (command as unknown as Record<string, number>)[`a${index}`] = args[index] >>> 0;
    }
    this.commands.push(command);
  }

  private boundBuffer(target: number): WebGLBuffer {
    if (target === this.ARRAY_BUFFER) return requireLive(this.arrayBuffer, "ARRAY_BUFFER");
    if (target === this.ELEMENT_ARRAY_BUFFER)
      return requireLive(this.indexBuffer, "ELEMENT_ARRAY_BUFFER");
    throw new RangeError("unsupported WebGL buffer target");
  }

  private boundTexture(): WebGLTexture {
    return requireLive(this.boundTextures.get(this.textureUnit) ?? null, "TEXTURE_2D");
  }

  private bufferUsage(value: number): number {
    if (value === this.STATIC_DRAW) return 0;
    if (value === this.DYNAMIC_DRAW) return 1;
    if (value === this.STREAM_DRAW) return 2;
    throw new RangeError("unsupported buffer usage");
  }

  private primitive(value: number): number {
    const values = [this.POINTS, this.LINES, this.LINE_STRIP, this.TRIANGLES,
      this.TRIANGLE_STRIP, this.TRIANGLE_FAN];
    const mapped = values.indexOf(value);
    if (mapped < 0) throw new RangeError("unsupported primitive");
    return mapped;
  }

  private vertexType(value: number): number {
    if (value === this.FLOAT) return 0;
    if (value === this.UNSIGNED_BYTE) return 1;
    if (value === this.BYTE) return 2;
    if (value === this.UNSIGNED_SHORT) return 3;
    if (value === this.SHORT) return 4;
    throw new RangeError("unsupported vertex type");
  }

  private blendFactor(value: number): number {
    if (value === this.ZERO) return 0;
    if (value === this.ONE) return 1;
    if (value === this.SRC_ALPHA) return 6;
    if (value === this.ONE_MINUS_SRC_ALPHA) return 7;
    throw new RangeError("unsupported blend factor");
  }

  private setCapability(capability: number, enabled: boolean): void {
    if (capability === this.BLEND) {
      this.command(3, enabled ? 1 : 0, 6, 7, 6, 7, 0, 0, 0);
    } else if (capability === this.DEPTH_TEST) {
      this.command(4, enabled ? 1 : 0, 1, 1);
    } else if (capability === this.STENCIL_TEST) {
      this.command(6, enabled ? 1 : 0, 0xffffffff, 0xffffffff);
    } else if (capability === this.CULL_FACE) {
      this.command(10, enabled ? 1 : 0, 1, 0);
    } else if (capability === this.SCISSOR_TEST) {
      const [x, y, width, height] = this.scissorBox;
      this.command(2, enabled ? 1 : 0, x, y, width, height);
    } else {
      throw new RangeError("unsupported WebGL capability");
    }
  }

  private emitAttribute(location: number, state: AttributeState): void {
    this.command(15, location, state.components, state.type,
      state.normalized ? 1 : 0, state.stride, state.offset, 1);
  }

  private uniform(location: WebGLUniformLocation, type: number, words: number[]): void {
    const offset = this.words.length;
    this.words.push(...words.map((word) => word >>> 0));
    this.command(16, location.value, type, 1, offset);
  }

  private uniformFloats(
    location: WebGLUniformLocation | null,
    type: number,
    elements: number,
    values: ArrayLike<number>,
  ): void {
    if (!location) return;
    if (values.length === 0 || values.length % elements !== 0)
      throw new RangeError("uniform data has an invalid length");
    const offset = this.words.length;
    for (let index = 0; index < values.length; ++index)
      this.words.push(floatBits(values[index]));
    this.command(16, location.value, type, values.length / elements, offset);
  }

  private wrapFlags(flags: number, value: number, repeatBit: number, mirrorBit: number): number {
    flags &= ~((1 << repeatBit) | (1 << mirrorBit));
    if (value === this.REPEAT) return flags | (1 << repeatBit);
    if (value === this.MIRRORED_REPEAT) return flags | (1 << mirrorBit);
    if (value === this.CLAMP_TO_EDGE) return flags;
    throw new RangeError("unsupported texture wrapping mode");
  }

  private validateTextureArguments(
    target: number,
    level: number,
    internalFormat: number,
    border: number,
    format: number,
    type: number,
  ): void {
    if (target !== this.TEXTURE_2D || level !== 0 || border !== 0 ||
        internalFormat !== this.RGBA || format !== this.RGBA ||
        type !== this.UNSIGNED_BYTE)
      throw new RangeError("only level-0 RGBA/UNSIGNED_BYTE textures are supported");
  }
}
