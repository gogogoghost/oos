declare module "oos:runtime" {
  export type LogLevel = "debug" | "info" | "warn" | "error";
  export function abiVersion(): number;
  export function wallClockMinutes(): number;
  export function monotonicTimeUs(): bigint;
  export function wallClockTimeMs(): bigint;
  export function wakeMainThread(): void;
  export function requestExit(): void;
  export function setStatusBarStyle(backgroundRgb: number, darkIcons: boolean): void;
  export function setSurfaceMode(mode: "normal" | "immersive"): void;
  export function log(level: LogLevel, message: string): void;
}

declare module "oos:applications" {
  export type ApplicationRuntime = "js" | "wasm";
  export interface ApplicationInfo {
    id: string;
    name: string;
    version: string;
    runtime: ApplicationRuntime;
    enabled: boolean;
  }
  export function list(): ApplicationInfo[];
  export function launch(appId: string): void;
  export function uninstall(appId: string): void;
}

declare module "oos:system-ui" {
  export interface SystemUiSnapshot {
    revision: number;
    statusBarVisible: boolean;
    locked: boolean;
    showClock: boolean;
    showNetwork: boolean;
    showBatteryPercentage: boolean;
    backgroundRgb: number;
    darkIcons: boolean;
    batteryAvailable: boolean;
    batteryPercent: number;
    charging: boolean;
    wifiAvailable: boolean;
    wifiConnected: boolean;
    cellularAvailable: boolean;
    cellularRegistered: boolean;
    roaming: boolean;
    signalBars: number;
    radioTechnology: string;
  }
  export function snapshot(): string;
  export function setLocked(locked: boolean): void;
}

declare module "oos:system-settings" {
  export interface StatusBarPreferences {
    showClock: boolean;
    showNetwork: boolean;
    showBatteryPercentage: boolean;
    revision: bigint;
  }
  export function getStatusBar(): StatusBarPreferences;
  export function setStatusBar(
    showClock: boolean,
    showNetwork: boolean,
    showBatteryPercentage: boolean,
  ): void;
}

declare module "oos:modules" {
  export type ModuleKind = "js" | "wasm";
  export interface ModuleInfo { name: string; runtime: ModuleKind }
  export function enumerate(): ModuleInfo[];
  export function invoke(
    module: string,
    operation: string,
    request: ArrayBuffer | ArrayBufferView,
  ): Uint8Array;
}

declare module "oos:canvas" {
  export interface NativeCanvasOptions {
    context: "2d" | "mesh2d" | "webgl";
    x: number;
    y: number;
    width: number;
    height: number;
    z: number;
    visible: boolean;
  }

  export interface NativeCanvas2dCommand {
    op: "clear" | "fillRect" | "strokeRect" | "fillText" | "pushClip" | "popClip";
    x?: number;
    y?: number;
    width?: number;
    height?: number;
    radius?: number;
    lineWidth?: number;
    fontSize?: number;
    rgba?: number;
    text?: string;
  }

  export function create(options: NativeCanvasOptions): number;
  export function configure(
    handle: number,
    geometry: Omit<NativeCanvasOptions, "context"> & { visible: boolean },
  ): void;
  export function destroy(handle: number): void;
  export function submit2d(handle: number, commands: NativeCanvas2dCommand[]): void;

  export interface NativeTextureDescriptor {
    format: number;
    x: number;
    y: number;
    width: number;
    height: number;
    rowStride: number;
    flags: number;
  }

  export interface NativeMeshVertex {
    x: number;
    y: number;
    u: number;
    v: number;
    r?: number;
    g?: number;
    b?: number;
    a?: number;
  }

  export interface NativeMeshCommand {
    firstIndex: number;
    indexCount: number;
    texture: number;
    clipMinX: number;
    clipMinY: number;
    clipMaxX: number;
    clipMaxY: number;
  }

  export function textureSet(
    canvas: number,
    texture: number,
    descriptor: NativeTextureDescriptor,
    pixels: ArrayBuffer | ArrayBufferView,
  ): void;
  export function textureFree(canvas: number, texture: number): void;
  export function submitMesh(
    canvas: number,
    vertices: NativeMeshVertex[],
    indices: Uint16Array,
    commands: NativeMeshCommand[],
    clearRgba: number,
  ): void;
}

declare module "oos:graphics" {
  import type {
    NativeMeshCommand,
    NativeMeshVertex,
    NativeTextureDescriptor,
  } from "oos:canvas";

  export interface SurfaceSize { width: number; height: number }
  export interface GraphicsLimits {
    maxTextureSize: number;
    maxTextureBytes: number;
    maxVertices: number;
    maxIndices: number;
    maxDrawCommands: number;
  }

  export function surfaceSize(): SurfaceSize;
  export function pixelsPerPoint(): number;
  export function surfaceFormat(): number;
  export function supportedTextureFormats(): number;
  export function graphicsLimits(): GraphicsLimits;
  export function textureSet(
    texture: number,
    descriptor: NativeTextureDescriptor,
    pixels: ArrayBuffer | ArrayBufferView,
  ): void;
  export function textureFree(texture: number): void;
  export function submit(
    vertices: NativeMeshVertex[],
    indices: Uint16Array,
    commands: NativeMeshCommand[],
    clearRgba: number,
  ): void;
}

declare module "oos:device" {
  export interface Descriptor {
    id: string;
    manufacturer: string;
    model: string;
    androidApi: number;
    primaryWidth: number;
    primaryHeight: number;
    secondaryWidth: number;
    secondaryHeight: number;
  }
  export function getDescriptor(): Descriptor;
  export function getCapability(feature: number): number;
  export function getAccess(feature: number): number;
}

declare module "oos:storage" {
  export function kvGet(key: string): Uint8Array | null;
  export function kvSet(key: string, value: ArrayBuffer | ArrayBufferView): void;
  export function kvDelete(key: string): boolean;
  export function kvClear(): void;
  export function databaseExecute(database: string, sql: string): number;
  export function databasePrepare(database: string, sql: string): number;
  export function statementBindNull(statement: number, index: number): void;
  export function statementBindInteger(statement: number, index: number, value: bigint): void;
  export function statementBindFloat(statement: number, index: number, value: number): void;
  export function statementBindText(statement: number, index: number, value: string): void;
  export function statementBindBlob(statement: number, index: number, value: ArrayBuffer | ArrayBufferView): void;
  export function statementStep(statement: number): number;
  export function statementColumnCount(statement: number): number;
  export function statementColumnKind(statement: number, column: number): number;
  export function statementColumnInteger(statement: number, column: number): bigint;
  export function statementColumnFloat(statement: number, column: number): number;
  export function statementColumnText(statement: number, column: number): string;
  export function statementColumnBlob(statement: number, column: number): Uint8Array;
  export function statementFinish(statement: number): void;
}

declare module "oos:device-storage" {
  export interface Entry { path: string; size: bigint; lastModifiedMs: bigint }
  export function enumerateFiles(volume: number): Entry[];
  export function readFile(volume: number, path: string): Uint8Array;
  export function writeFile(
    volume: number,
    path: string,
    mode: number,
    bytes: ArrayBuffer | ArrayBufferView,
  ): void;
  export function deleteFile(volume: number, path: string): boolean;
  export function freeSpace(volume: number): bigint;
  export function usedSpace(volume: number): bigint;
}

declare module "oos:font-assets" {
  export function load(role: number): Uint8Array;
}

declare module "oos:assets" {
  export interface OpenedAsset { handle: number; size: bigint }
  export function open(path: string): OpenedAsset;
  export function read(handle: number, offset: bigint, maximumBytes: number): Uint8Array;
  export function close(handle: number): void;
}

declare module "oos:system-services" {
  export function request(service: string, operation: string, payload: string): string;
}

declare module "oos:audio" {
  export interface AudioStream { sampleRate: number; channelCount: number; deviceId: number; framesTransferred: bigint }
  export interface FormatSupport { mimeType: string; extensions: string; decoder: number; streaming: boolean; seekable: boolean }
  export interface OpenedPcm { handle: number; audioStream: AudioStream }
  export interface PcmStatus { audioStream: AudioStream; queuedFrames: bigint; consumedFrames: bigint; underruns: number; paused: boolean }
  export interface PlaybackStatus { state: number; positionMs: bigint; durationMs: bigint; underruns: number; failure: number }
  export interface RecordingResult { audioStream: AudioStream; peak: number; rms: number; path: string }
  export function supportedFormats(): FormatSupport[];
  export function getPcmCapabilities(): { minimumSampleRate: number; maximumSampleRate: number; supportedChannelMask: number; minimumCapacityFrames: number; maximumCapacityFrames: number };
  export function getSourceLimits(): { maximumSourceBytes: bigint; maximumSessionBytes: bigint; maximumSources: number; maximumPlayers: number };
  export function pcmOpen(sampleRate: number, channelCount: number, capacityFrames: number, usage: number): OpenedPcm;
  export function pcmWrite(handle: number, samples: Int16Array): bigint;
  export function pcmSetVolume(handle: number, volume: number): void;
  export function pcmPause(handle: number): void;
  export function pcmResume(handle: number): void;
  export function pcmFlush(handle: number): void;
  export function pcmStatus(handle: number): PcmStatus;
  export function pcmClose(handle: number): void;
  export function playerOpenAsset(path: string, usage: number): number;
  export function sourceCreate(bytes: ArrayBuffer | ArrayBufferView, mimeType: string, locatorHint: string): number;
  export function sourceClose(handle: number): void;
  export function playerOpenSource(source: number, usage: number): number;
  export function playerPlay(handle: number): void;
  export function playerPause(handle: number): void;
  export function playerSeek(handle: number, positionMs: bigint): void;
  export function playerSetVolume(handle: number, volume: number): void;
  export function playerSetLooping(handle: number, looping: boolean): void;
  export function playerStatus(handle: number): PlaybackStatus;
  export function playerClose(handle: number): void;
  export function playTone(frequencyHz: number, durationMs: number, volume: number, usage: number): AudioStream;
  export function recordWav(path: string, durationMs: number): RecordingResult;
  export function lastError(): string;
}

declare module "oos:camera" {
  export interface CameraInfo { id: string; facing: number; sensorOrientation: number; hardwareLevel: number; flashAvailable: boolean; maxJpegWidth: number; maxJpegHeight: number }
  export interface PhotoResult { path: string; width: number; height: number; bytes: bigint }
  export function enumerate(): CameraInfo[];
  export function setTorch(cameraId: string, enabled: boolean): void;
  export function captureJpeg(cameraId: string, path: string, width: number, height: number, flash: boolean, timeoutMs: number): PhotoResult;
  export function lastError(): string;
}

declare module "oos:power" {
  export interface BatterySnapshot { state: number; capacityPercent: number; voltageMicrovolts: number; currentMicroamps: number; temperatureTenthsCelsius: number; usbOnline: boolean }
  export function queryBattery(): BatterySnapshot;
  export function waitForBatteryEvent(timeoutMs: number): BatterySnapshot | null;
  export function setInteractive(interactive: boolean): void;
  export function acquireWakeLock(name: string): void;
  export function releaseWakeLock(name: string): void;
  export function enableAutoSuspend(): void;
  export function disableAutoSuspend(): void;
  export function scheduleRtcWake(delaySeconds: number): void;
  export function clearRtcWake(): void;
  export function suspend(gracefulTimeoutMs: number): void;
  export function queryFlipState(): number;
  export function lastError(): string;
}

declare module "oos:vibrator" {
  export function vibrate(durationMs: number): void;
  export function stop(): void;
  export function supportsAmplitudeControl(): boolean;
  export function setAmplitude(amplitude: number): void;
  export function lastError(): string;
}

declare module "oos:wifi" {
  export interface Status { state: string; ssid: string; bssid: string; ipAddress: string; networkId: number }
  export interface AccessPoint { bssid: string; frequencyMhz: number; signalDbm: number; capabilities: string; ssid: string }
  export interface Network { id: number; ssid: string; bssid: string; capabilities: string }
  export function enabled(): boolean;
  export function setEnabled(enabled: boolean): void;
  export function getStatus(): Status;
  export function scan(waitMs: number): AccessPoint[];
  export function listNetworks(): Network[];
  export function connect(ssid: string, security: number, credential: string): number;
  export function disconnect(): void;
  export function reconnect(): void;
  export function select(networkId: number): void;
  export function forget(networkId: number): void;
  export function saveConfiguration(): void;
  export function lastError(): string;
}

declare module "oos:ip" {
  export interface Configuration { interfaceName: string; address: string; prefixLength: number; gateway: string; dns1: string; dns2: string }
  export function getStatus(): Configuration;
  export function useDhcp(timeoutMs: number): void;
  export function useStatic(configuration: Configuration): void;
  export function lastError(): string;
}

declare module "oos:bluetooth" {
  export interface DiscoveredDevice { address: string; name: string; rssi: number; deviceClass: number; deviceType: number; advertisingData: Uint8Array }
  export function enable(timeoutMs: number): void;
  export function disable(timeoutMs: number): void;
  export function classicScan(durationMs: number): DiscoveredDevice[];
  export function leScan(durationMs: number): DiscoveredDevice[];
  export function pair(address: string, transport: number): void;
  export function unpair(address: string): void;
  export function cancelPairing(address: string): void;
  export function profileConnect(address: string, profile: number): void;
  export function profileDisconnect(address: string, profile: number): void;
  export function profileConnectionCycle(address: string, profile: number, holdMs: number): void;
  export function leConnectionCycle(address: string, holdMs: number, timeoutMs: number): void;
  export function lastError(): string;
}

declare module "oos:modem" {
  export interface RequestStatus { operation: string; error: number; timedOut: boolean }
  export interface Snapshot {
    serviceConnected: boolean; radioState: number; basebandVersion: string;
    identity: Record<string, string>; sim: Record<string, number>;
    signal: Record<string, number>; voiceRegistration: Record<string, number>;
    dataRegistration: Record<string, number>; networkOperator: Record<string, string>;
    preferredNetworkType: number; voiceRadioTechnology: number;
    currentCallCount: number; dataCallCount: number; hardwareConfigCount: number;
    radioAccessFamily: number; logicalModemUuid: string; requests: RequestStatus[];
  }
  export function querySnapshot(timeoutMs: number): Snapshot;
  export function setRadioPower(enabled: boolean, timeoutMs: number): RequestStatus;
  export function lastError(): string;
}

declare module "oos:codec" {
  export interface RoundTripResult { encoderName: string; decoderName: string; encoderHardwareAccelerated: boolean; decoderHardwareAccelerated: boolean; width: number; height: number; inputFrames: number; outputBuffers: number; decodedFrames: number; encodedBytes: bigint }
  export function testH264RoundTrip(width: number, height: number, frameCount: number, timeoutMs: number): RoundTripResult;
  export function lastError(): string;
}

declare module "oos:gles" {
  import type { NativeTextureDescriptor } from "oos:canvas";

  export interface NativeGlesCapabilities {
    majorVersion: number;
    minorVersion: number;
    maxTextureSize: number;
    maxTextureUnits: number;
    maxVertexAttributes: number;
    maxVaryingVectors: number;
    maxVertexUniformVectors: number;
    maxFragmentUniformVectors: number;
    depthBits: number;
    stencilBits: number;
    maxBufferBytes: number;
    maxCommands: number;
    maxCommandDataWords: number;
  }

  export interface NativeGlesCommand {
    op: number;
    a0?: number;
    a1?: number;
    a2?: number;
    a3?: number;
    a4?: number;
    a5?: number;
    a6?: number;
    a7?: number;
  }

  export function capabilities(canvas: number): NativeGlesCapabilities;
  export function textureSet(
    canvas: number,
    texture: number,
    descriptor: NativeTextureDescriptor,
    pixels: ArrayBuffer | ArrayBufferView,
  ): void;
  export function textureFree(canvas: number, texture: number): void;
  export function bufferSet(
    canvas: number,
    buffer: number,
    size: number,
    usage: number,
    data: ArrayBuffer | ArrayBufferView,
  ): void;
  export function bufferWrite(
    canvas: number,
    buffer: number,
    offset: number,
    data: ArrayBuffer | ArrayBufferView,
  ): void;
  export function bufferFree(canvas: number, buffer: number): void;
  export function shaderSet(canvas: number, shader: number, stage: number, source: string): void;
  export function shaderFree(canvas: number, shader: number): void;
  export function programSet(
    canvas: number,
    program: number,
    vertexShader: number,
    fragmentShader: number,
  ): void;
  export function programFree(canvas: number, program: number): void;
  export function attributeLocation(canvas: number, program: number, name: string): number;
  export function uniformLocation(canvas: number, program: number, name: string): number;
  export function submit(
    canvas: number,
    commands: NativeGlesCommand[],
    data: Uint32Array,
  ): void;
}

// Private transport used only by the Solid platform renderer. It is not part
// of the application SDK exports.
declare module "oos:solid-internal" {
  export interface NativeUiNode {
    id: number;
    parent?: number;
    kind: "container" | "text" | "canvas";
    class?: string;
    text?: string;
    canvas?: number;
  }

  export function submit(nodes: NativeUiNode[]): void;
  export function clear(): void;
}
