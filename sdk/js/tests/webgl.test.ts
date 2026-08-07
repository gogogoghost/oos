import { beforeEach, describe, expect, it, vi } from "vitest";

const native = vi.hoisted(() => ({
  create: vi.fn(() => 41),
  configure: vi.fn(),
  destroy: vi.fn(),
  submit2d: vi.fn(),
  canvasTextureSet: vi.fn(),
  canvasTextureFree: vi.fn(),
  submitMesh: vi.fn(),
  capabilities: vi.fn(() => ({
    majorVersion: 2,
    minorVersion: 0,
    maxTextureSize: 2048,
    maxTextureUnits: 8,
    maxVertexAttributes: 8,
    maxVaryingVectors: 8,
    maxVertexUniformVectors: 128,
    maxFragmentUniformVectors: 64,
    depthBits: 16,
    stencilBits: 8,
    maxBufferBytes: 1024 * 1024,
    maxCommands: 4096,
    maxCommandDataWords: 16384,
  })),
  textureSet: vi.fn(),
  textureFree: vi.fn(),
  bufferSet: vi.fn(),
  bufferWrite: vi.fn(),
  bufferFree: vi.fn(),
  shaderSet: vi.fn(),
  shaderFree: vi.fn(),
  programSet: vi.fn(),
  programFree: vi.fn(),
  attributeLocation: vi.fn(() => 3),
  uniformLocation: vi.fn(() => 5),
  submit: vi.fn(),
}));

vi.mock("oos:canvas", () => ({
  create: native.create,
  configure: native.configure,
  destroy: native.destroy,
  submit2d: native.submit2d,
  textureSet: native.canvasTextureSet,
  textureFree: native.canvasTextureFree,
  submitMesh: native.submitMesh,
}));

vi.mock("oos:gles", () => ({
  capabilities: native.capabilities,
  textureSet: native.textureSet,
  textureFree: native.textureFree,
  bufferSet: native.bufferSet,
  bufferWrite: native.bufferWrite,
  bufferFree: native.bufferFree,
  shaderSet: native.shaderSet,
  shaderFree: native.shaderFree,
  programSet: native.programSet,
  programFree: native.programFree,
  attributeLocation: native.attributeLocation,
  uniformLocation: native.uniformLocation,
  submit: native.submit,
}));

import { createCanvas } from "../src/canvas";

describe("WebGL platform backend", () => {
  beforeEach(() => vi.clearAllMocks());

  it("batches a complete draw into one native submit", () => {
    const canvas = createCanvas("webgl", { width: 64, height: 48 });
    const gl = canvas.getContext("webgl");

    const vertex = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vertex, "attribute vec2 position; void main(){gl_Position=vec4(position,0.,1.);}");
    gl.compileShader(vertex);
    const fragment = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fragment, "void main(){gl_FragColor=vec4(1.);}");
    gl.compileShader(fragment);
    const program = gl.createProgram();
    gl.attachShader(program, vertex);
    gl.attachShader(program, fragment);
    gl.linkProgram(program);
    gl.useProgram(program);

    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0, 1, 0, 0, 1]), gl.STATIC_DRAW);
    const position = gl.getAttribLocation(program, "position");
    gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 8, 0);
    gl.enableVertexAttribArray(position);
    const color = gl.getUniformLocation(program, "color");
    gl.uniform4fv(color, [1, 0, 0, 1]);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.flush();

    expect(native.shaderSet).toHaveBeenCalledTimes(2);
    expect(native.programSet).toHaveBeenCalledTimes(1);
    expect(native.bufferSet).toHaveBeenCalledTimes(1);
    expect(native.submit).toHaveBeenCalledTimes(1);
    const [handle, commands, words] = native.submit.mock.calls[0];
    expect(handle).toBe(41);
    expect(commands[0].op).toBe(0);
    expect(commands.at(-1)?.op).toBe(19);
    expect(commands.some((command: { op: number }) => command.op === 17)).toBe(true);
    expect(words).toBeInstanceOf(Uint32Array);
    expect(words.length).toBe(4);
  });

  it("scopes texture resources to its canvas and frees them", () => {
    const canvas = createCanvas("webgl", { width: 16, height: 16 });
    const gl = canvas.getContext("webgl");
    const texture = gl.createTexture();
    gl.activeTexture(gl.TEXTURE0 + 1);
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texImage2D(
      gl.TEXTURE_2D,
      0,
      gl.RGBA,
      1,
      1,
      0,
      gl.RGBA,
      gl.UNSIGNED_BYTE,
      new Uint8Array([255, 0, 0, 255]),
    );
    gl.deleteTexture(texture);

    expect(native.textureSet).toHaveBeenCalledWith(
      41,
      texture.id,
      expect.objectContaining({ width: 1, height: 1, rowStride: 4 }),
      expect.any(Uint8Array),
    );
    expect(native.textureFree).toHaveBeenCalledWith(41, texture.id);
    expect(() => gl.bindTexture(gl.TEXTURE_2D, texture)).toThrow(/deleted/);
  });
});
