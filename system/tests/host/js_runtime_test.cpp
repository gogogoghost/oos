#include "oos/apps/app_manifest.h"
#include "oos/device/device.h"
#include "oos/input/key_input.h"
#include "oos/runtime/application_scene.h"
#include "oos/runtime/canvas_2d.h"
#include "oos/runtime/graphics_host.h"
#include "oos/runtime/js_app.h"
#include "oos/runtime/native_ui.h"
#include "oos/ui/system_ui_settings.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>

namespace {

class FakeGraphics final : public oos::runtime::GraphicsHost {
public:
  uint32_t width() const override { return 240; }
  uint32_t height() const override { return 298; }
  uint32_t surfaceFormat() const override { return OOS_TEXTURE_RGB565; }
  uint32_t supportedTextureFormats() const override {
    return OOS_TEXTURE_FORMAT_MASK;
  }
  bool setTexture(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                  uint32_t, uint32_t, const uint8_t *, size_t) override {
    ++texture_updates;
    return true;
  }
  bool freeTexture(uint32_t) override { return true; }
  bool submit(const OosGfxVertex *, size_t, const uint16_t *, size_t,
              const OosGfxDrawCommand *, size_t command_count,
              uint32_t) override {
    ++submissions;
    last_command_count = command_count;
    return true;
  }
  bool glesCapabilities(OosGlesCapabilities &result) override {
    result = {2,
              0,
              OOS_GFX_MAX_TEXTURE_SIZE,
              8,
              8,
              8,
              128,
              64,
              16,
              8,
              OOS_GLES_MAX_BUFFER_BYTES,
              OOS_GLES_MAX_COMMANDS,
              OOS_GLES_MAX_COMMAND_DATA_WORDS};
    return true;
  }
  bool setGlesBuffer(uint32_t, uint32_t, uint32_t, const uint8_t *,
                     size_t) override {
    return true;
  }
  bool writeGlesBuffer(uint32_t, uint32_t, const uint8_t *, size_t) override {
    return true;
  }
  bool freeGlesBuffer(uint32_t) override { return true; }
  bool setGlesShader(uint32_t, uint32_t, const char *, size_t) override {
    return true;
  }
  bool freeGlesShader(uint32_t) override { return true; }
  bool setGlesProgram(uint32_t, uint32_t, uint32_t) override { return true; }
  bool freeGlesProgram(uint32_t) override { return true; }
  int32_t glesAttributeLocation(uint32_t, const char *, size_t) override {
    return 0;
  }
  int32_t glesUniformLocation(uint32_t, const char *, size_t) override {
    return 0;
  }
  bool submitGles(const OosGlesCommand *, size_t, const uint32_t *,
                  size_t) override {
    return true;
  }
  bool submitGlesToTexture(uint32_t, uint32_t, uint32_t, const OosGlesCommand *,
                           size_t, const uint32_t *, size_t) override {
    ++gles_submissions;
    return true;
  }

  size_t texture_updates = 0;
  size_t submissions = 0;
  size_t last_command_count = 0;
  size_t gles_submissions = 0;
};

struct TestPackage {
  std::filesystem::path root;
  std::filesystem::path app;
  std::filesystem::path modules;
  std::filesystem::path data;
  std::filesystem::path assets;
  std::filesystem::path internal;
  std::filesystem::path removable;

  TestPackage() {
    const auto seed =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() /
           ("oos-js-runtime-" + std::to_string(seed));
    app = root / "app";
    modules = root / "modules";
    data = root / "data";
    assets = root / "assets";
    internal = root / "internal";
    removable = root / "removable";
    std::filesystem::create_directories(app);
    std::filesystem::create_directories(modules);
    std::filesystem::create_directories(data);
    std::filesystem::create_directories(assets);
    std::filesystem::create_directories(internal);
    std::filesystem::create_directories(removable);
  }

  ~TestPackage() { std::filesystem::remove_all(root); }

  void write(const std::filesystem::path &path, const char *source) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << source;
    output.close();
    assert(output.good());
  }
};

oos::runtime::JsAppOptions optionsFor(const TestPackage &package) {
  oos::runtime::JsAppOptions options;
  options.app_id = "org.example.quickjs";
  options.application_directory = package.app.string();
  options.module_directory = package.modules.string();
  options.data_directory = package.data.string();
  options.asset_directory = package.assets.string();
  options.internal_media_directory = package.internal.string();
  options.removable_media_directory = package.removable.string();
  options.service_permission_mask =
      oos::apps::permissionBit(
          oos::apps::DeviceServicePermission::DeviceStorageRead) |
      oos::apps::permissionBit(
          oos::apps::DeviceServicePermission::DeviceStorageWrite) |
      oos::apps::permissionBit(
          oos::apps::DeviceServicePermission::DeviceStorageCreate);
  options.enforce_service_permissions = true;
  options.execution_time_limit_ms = 20;
  options.modules.push_back(
      {"shared", oos::apps::AppRuntimeKind::JavaScript, "modules/shared.mjs"});
  return options;
}

void testLifecycleAndImports() {
  TestPackage package;
  package.write(package.app / "helper.mjs", "export const local = 5;\n");
  package.write(
      package.modules / "shared.mjs",
      "import { abiVersion } from 'oos:runtime';\n"
      "import { kvSet } from 'oos:storage';\n"
      "export const shared = 7;\n"
      "export function invoke(operation, request) {\n"
      "  if (operation !== 'increment') throw new Error('bad operation');\n"
      "  if (abiVersion() !== 8) throw new Error('bad ABI');\n"
      "  const response = new Uint8Array([request[0] + 1]);\n"
      "  kvSet('module-result', response);\n"
      "  return response;\n"
      "}\n");
  package.write(package.assets / "fixture.bin", "asset-data");
  package.write(
      package.app / "main.mjs",
      "import { abiVersion, wallClockMinutes, requestExit } from "
      "'oos:runtime';\n"
      "import { getDescriptor } from 'oos:device';\n"
      "import { kvSet, kvGet, databaseExecute } from 'oos:storage';\n"
      "import { open as openAsset, read as readAsset, close as closeAsset } "
      "from 'oos:assets';\n"
      "import { writeFile, readFile, deleteFile } from 'oos:device-storage';\n"
      "import { load as loadFont } from 'oos:font-assets';\n"
      "import { request as systemRequest } from 'oos:system-services';\n"
      "import { supportedFormats, lastError as audioError } from 'oos:audio';\n"
      "import { lastError as cameraError } from 'oos:camera';\n"
      "import { lastError as powerError } from 'oos:power';\n"
      "import { lastError as vibratorError } from 'oos:vibrator';\n"
      "import { lastError as wifiError } from 'oos:wifi';\n"
      "import { lastError as ipError } from 'oos:ip';\n"
      "import { lastError as bluetoothError } from 'oos:bluetooth';\n"
      "import { lastError as modemError } from 'oos:modem';\n"
      "import { lastError as codecError } from 'oos:codec';\n"
      "import { create, submit2d, textureSet, submitMesh, destroy } from "
      "'oos:canvas';\n"
      "import { surfaceSize, graphicsLimits, textureSet as rootTextureSet, "
      "submit as rootSubmit } from 'oos:graphics';\n"
      "import { capabilities, submit as submitGles } from 'oos:gles';\n"
      "import { submit as submitUi, clear as clearUi } from "
      "'oos:solid-internal';\n"
      "import { enumerate, invoke } from 'oos:modules';\n"
      "import { local } from './helper.mjs';\n"
      "import { shared } from 'shared';\n"
      "let ready = false;\n"
      "let canvas = 0;\n"
      "let mesh = 0;\n"
      "let webgl = 0;\n"
      "export function initialize() {\n"
      "  const reply = invoke('shared', 'increment', new Uint8Array([8]));\n"
      "  const moduleResult = kvGet('module-result');\n"
      "  kvSet('answer', new Uint8Array([4, 2]));\n"
      "  const stored = kvGet('answer');\n"
      "  const changes = databaseExecute('main', 'CREATE TABLE IF NOT EXISTS "
      "smoke(value INTEGER)');\n"
      "  const asset = openAsset('fixture.bin');\n"
      "  const assetBytes = readAsset(asset.handle, 0n, 64);\n"
      "  closeAsset(asset.handle);\n"
      "  writeFile(0, 'quickjs.bin', 0, new Uint8Array([6, 7, 8]));\n"
      "  const mediaBytes = readFile(0, 'quickjs.bin');\n"
      "  const removed = deleteFile(0, 'quickjs.bin');\n"
      "  const serviceErrors = [audioError(), cameraError(), powerError(), "
      "vibratorError(), wifiError(), ipError(), bluetoothError(), "
      "modemError(), codecError()];\n"
      "  ready = abiVersion() === 8 && wallClockMinutes() < 1440 &&\n"
      "    surfaceSize().width === 240 && surfaceSize().height === 298 &&\n"
      "    graphicsLimits().maxVertices === 65535 &&\n"
      "    getDescriptor().id === '' && stored?.[0] === 4 && stored?.[1] === 2 "
      "&&\n"
      "    changes === 0 && assetBytes.length === 10 && assetBytes[0] === 97 "
      "&& assetBytes[9] === 97 &&\n"
      "    mediaBytes[2] === 8 && removed && supportedFormats().length > 0 &&\n"
      "    serviceErrors.every(value => typeof value === 'string') &&\n"
      "    typeof loadFont === 'function' && typeof systemRequest === "
      "'function' &&\n"
      "    local + shared === 12 && reply[0] === 9 && moduleResult?.[0] === 9 "
      "&&\n"
      "    enumerate().some(module => module.name === 'shared' && "
      "module.runtime === 'js') &&\n"
      "    WebAssembly.validate('shared') === false &&\n"
      "    typeof document === 'undefined' && typeof window === 'undefined';\n"
      "  rootTextureSet(9, { format: 3, x: 0, y: 0, width: 1, height: 1, "
      "rowStride: 4, flags: 4 }, new Uint8Array([255, 255, 255, 255]));\n"
      "  rootSubmit([{ x: 0, y: 0, u: 0, v: 0 }, { x: 1, y: 0, u: 1, v: 0 }, { "
      "x: 0, y: 1, u: 0, v: 1 }], new Uint16Array([0, 1, 2]), [{ firstIndex: "
      "0, indexCount: 3, texture: 9, clipMinX: 0, clipMinY: 0, clipMaxX: 1, "
      "clipMaxY: 1 }], 0xff000000);\n"
      "  canvas = create({ context: '2d', x: 0, y: 0, width: 40, height: 40, "
      "z: 1 });\n"
      "  submit2d(canvas, [{ op: 'clear', x: 0, y: 0, width: 40, height: 40, "
      "rgba: 0xff202020 }]);\n"
      "  mesh = create({ context: 'mesh2d', x: 40, y: 0, width: 20, height: "
      "20, z: 2 });\n"
      "  textureSet(mesh, 1, { format: 3, x: 0, y: 0, width: 1, height: 1, "
      "rowStride: 4, flags: 4 }, new Uint8Array([255, 255, 255, 255]));\n"
      "  submitMesh(mesh, [{ x: 0, y: 0, u: 0, v: 0 }, { x: 20, y: 0, u: 1, v: "
      "0 }, { x: 0, y: 20, u: 0, v: 1 }], new Uint16Array([0, 1, 2]), [{ "
      "firstIndex: 0, indexCount: 3, texture: 1, clipMinX: 0, clipMinY: 0, "
      "clipMaxX: 20, clipMaxY: 20 }], 0);\n"
      "  webgl = create({ context: 'webgl', x: 60, y: 0, width: 20, height: "
      "20, z: 3 });\n"
      "  ready = ready && capabilities(webgl).maxCommands >= 2;\n"
      "  submitGles(webgl, [{ op: 0, a0: 1, a1: 0xff000000, a2: 1065353216 }, "
      "{ op: 19 }], new Uint32Array());\n"
      "  submitUi([\n"
      "    { id: 1, kind: 'container', class: 'flex flex-col w-full h-full "
      "p-2' },\n"
      "    { id: 2, parent: 1, kind: 'text', class: 'text-sm text-white', "
      "text: 'Ready' },\n"
      "    { id: 3, parent: 1, kind: 'canvas', class: 'grow w-full', canvas }\n"
      "  ]);\n"
      "  return ready;\n"
      "}\n"
      "export function onKey(event) {\n"
      "  return ready && event.code === 42 && event.action === 'pressed' &&\n"
      "    typeof event.monotonicTimeUs === 'bigint';\n"
      "}\n"
      "export function frame(now) {\n"
      "  if (ready && typeof now === 'bigint') requestExit();\n"
      "  return 17;\n"
      "}\n"
      "export function shutdown() { clearUi(); destroy(webgl); destroy(mesh); "
      "destroy(canvas); }\n");

  FakeGraphics graphics;
  oos::runtime::ApplicationScene scene(graphics);
  oos::runtime::JsApp app(scene, optionsFor(package));
  assert(app.load((package.app / "main.mjs").c_str()));
  const bool initialized = app.initialize();
  if (!initialized)
    std::fprintf(stderr, "JavaScript initialize failed: %s\n", app.lastError());
  assert(initialized);
  oos::input::KeyEvent key;
  key.code = 42;
  key.action = oos::input::KeyAction::Pressed;
  assert(app.dispatchKey(key, 1234));
  uint32_t delay = 0;
  assert(app.render(5678, delay));
  assert(scene.present());
  assert(delay == 17);
  assert(graphics.submissions == 1);
  assert(graphics.gles_submissions == 1);
  assert(app.takeExitRequest());
  assert(!app.takeExitRequest());
  app.shutdown();
  assert(!app.loaded());
}

void testImportConfinement() {
  TestPackage package;
  package.write(package.root / "outside.mjs", "export const value = 1;\n");
  package.write(package.app / "main.mjs",
                "import { value } from '../outside.mjs';\n"
                "export function frame() { return value; }\n");
  FakeGraphics graphics;
  oos::runtime::JsApp app(graphics, optionsFor(package));
  assert(!app.load((package.app / "main.mjs").c_str()));
  assert(std::string(app.lastError()).find("escapes the package") !=
         std::string::npos);
}

void testSystemSettingsAndWifiModules() {
  TestPackage package;
  std::filesystem::create_directories(package.root / "system");
  package.write(
      package.app / "main.mjs",
      "import { enabled, setEnabled, select } from 'oos:wifi';\n"
      "import { getStatusBar, setStatusBar } from 'oos:system-settings';\n"
      "export function initialize() {\n"
      "  if (!enabled()) throw new Error('wifi should start enabled');\n"
      "  setEnabled(false);\n"
      "  if (enabled()) throw new Error('wifi disable failed');\n"
      "  setEnabled(true);\n"
      "  select(1);\n"
      "  setStatusBar(false, true, false);\n"
      "  const value = getStatusBar();\n"
      "  return !value.showClock && value.showNetwork &&\n"
      "    !value.showBatteryPercentage && typeof value.revision === "
      "'bigint';\n"
      "}\n"
      "export function frame() { return 1000; }\n");

  oos::ui::SystemUiSettings settings(package.root.string());
  assert(settings.initialize());
  std::unique_ptr<oos::device::Device> device = oos::device::createDevice();
  assert(device);
  auto options = optionsFor(package);
  options.system_ui_settings = &settings;
  options.service_permission_mask =
      oos::apps::permissionBit(oos::apps::DeviceServicePermission::Wifi) |
      oos::apps::permissionBit(
          oos::apps::DeviceServicePermission::SystemSettings);
  FakeGraphics graphics;
  oos::runtime::JsApp app(graphics, *device, std::move(options));
  assert(app.load((package.app / "main.mjs").c_str()));
  const bool initialized = app.initialize();
  if (!initialized)
    std::fprintf(stderr, "JavaScript service initialize failed: %s\n",
                 app.lastError());
  assert(initialized);
  assert(!settings.statusBar().show_clock);
  assert(settings.statusBar().show_network);
  assert(!settings.statusBar().show_battery_percentage);
  app.shutdown();
}

void testExecutionDeadline() {
  TestPackage package;
  package.write(package.app / "main.mjs",
                "export function initialize() { while (true) {} }\n"
                "export function frame() { return 0; }\n");
  FakeGraphics graphics;
  auto options = optionsFor(package);
  options.execution_time_limit_ms = 5;
  oos::runtime::JsApp app(graphics, std::move(options));
  assert(app.load((package.app / "main.mjs").c_str()));
  const auto start = std::chrono::steady_clock::now();
  assert(!app.initialize());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  assert(elapsed < std::chrono::seconds(1));
}

void testCanvasSceneComposition() {
  FakeGraphics graphics;
  oos::runtime::ApplicationScene scene(graphics);
  const uint32_t canvas = scene.createCanvas(
      {10, 20, 80, 60, 4, true}, oos::runtime::CanvasContextKind::Canvas2d);
  assert(canvas != 0);
  oos::runtime::Canvas2dCommand clear;
  clear.opcode = static_cast<uint8_t>(oos::runtime::Canvas2dOpcode::Clear);
  clear.width = 80;
  clear.height = 60;
  clear.rgba = 0xff302010u;
  assert(scene.submitCanvas2d(canvas, &clear, 1, nullptr, 0));

  const uint8_t white[] = {255, 255, 255, 255};
  assert(scene.setTexture(1, OOS_TEXTURE_RGBA8888, 0, 0, 1, 1, 4,
                          OOS_TEXTURE_REPLACE, white, sizeof(white)));
  const OosGfxVertex vertices[] = {
      {{0, 0}, {0, 0}, {255, 255, 255, 255}},
      {{1, 0}, {1, 0}, {255, 255, 255, 255}},
      {{0, 1}, {0, 1}, {255, 255, 255, 255}},
  };
  const uint16_t indices[] = {0, 1, 2};
  const OosGfxDrawCommand draw = {0, 3, 1, {0, 0}, {1, 1}};
  assert(scene.submit(vertices, 3, indices, 3, &draw, 1, 0xff000000u));
  assert(scene.present());
  assert(graphics.texture_updates == 2);
  assert(graphics.submissions == 1);
  assert(!scene.setCanvasTexture(canvas, 2, OOS_TEXTURE_RGBA8888, 0, 0, 1, 1, 4,
                                 OOS_TEXTURE_REPLACE, white, sizeof(white)));
  assert(scene.destroyCanvas(canvas));
}

void testCanvas2dPixels() {
  oos::runtime::Canvas2dRenderer renderer(4, 4, nullptr);
  oos::runtime::Canvas2dCommand commands[2];
  commands[0].opcode =
      static_cast<uint8_t>(oos::runtime::Canvas2dOpcode::Clear);
  commands[0].width = 4;
  commands[0].height = 4;
  commands[0].rgba = 0xff000000u;
  commands[1].opcode =
      static_cast<uint8_t>(oos::runtime::Canvas2dOpcode::FillRect);
  commands[1].x = 1;
  commands[1].y = 1;
  commands[1].width = 2;
  commands[1].height = 2;
  commands[1].rgba = 0xff0000ffu;
  assert(renderer.render(commands, 2, nullptr, 0));
  const uint8_t *pixels = renderer.pixels();
  const size_t center = (1 * 4 + 1) * 4;
  assert(pixels[center] == 255);
  assert(pixels[center + 1] == 0);
  assert(pixels[center + 2] == 0);
  assert(pixels[center + 3] == 255);

  oos::runtime::Canvas2dCommand clipped[4];
  clipped[0] = commands[0];
  clipped[1].opcode =
      static_cast<uint8_t>(oos::runtime::Canvas2dOpcode::PushClip);
  clipped[1].x = 1;
  clipped[1].y = 1;
  clipped[1].width = 1;
  clipped[1].height = 1;
  clipped[2] = commands[1];
  clipped[2].x = 0;
  clipped[2].y = 0;
  clipped[2].width = 4;
  clipped[2].height = 4;
  clipped[3].opcode =
      static_cast<uint8_t>(oos::runtime::Canvas2dOpcode::PopClip);
  assert(renderer.render(clipped, 4, nullptr, 0));
  pixels = renderer.pixels();
  assert(pixels[(1 * 4 + 1) * 4] == 255);
  assert(pixels[0] == 0);
}

void testNativeUiLayoutAndTailwind() {
  FakeGraphics graphics;
  oos::runtime::ApplicationScene scene(graphics);
  const uint32_t raw_canvas = scene.createCanvas(
      {0, 0, 10, 10, 1, true}, oos::runtime::CanvasContextKind::Mesh2d);
  assert(raw_canvas != 0);
  const uint8_t white[] = {255, 255, 255, 255};
  assert(scene.setCanvasTexture(raw_canvas, 1, OOS_TEXTURE_RGBA8888, 0, 0, 1, 1,
                                4, OOS_TEXTURE_REPLACE, white, sizeof(white)));
  const OosGfxVertex vertices[] = {
      {{0, 0}, {0, 0}, {255, 255, 255, 255}},
      {{10, 0}, {1, 0}, {255, 255, 255, 255}},
      {{0, 10}, {0, 1}, {255, 255, 255, 255}},
  };
  const uint16_t indices[] = {0, 1, 2};
  const OosGfxDrawCommand draw = {0, 3, 1, {0, 0}, {10, 10}};
  assert(
      scene.submitCanvasMesh(raw_canvas, vertices, 3, indices, 3, &draw, 1, 0));

  std::string strings;
  const auto append = [&](const char *value) {
    const uint32_t offset = static_cast<uint32_t>(strings.size());
    strings += value;
    return std::pair<uint32_t, uint32_t>{
        offset, static_cast<uint32_t>(std::strlen(value))};
  };
  const auto root_class =
      append("flex flex-col w-full h-full p-4 gap-2 bg-white");
  const auto text_class = append("text-base text-black");
  const auto text_value = append("Hello");
  const auto canvas_class = append("grow w-full");
  oos::runtime::UiNodeRecord nodes[3];
  nodes[0].id = 1;
  nodes[0].class_offset = root_class.first;
  nodes[0].class_length = root_class.second;
  nodes[1].id = 2;
  nodes[1].parent = 1;
  nodes[1].kind = static_cast<uint8_t>(oos::runtime::UiNodeKind::Text);
  nodes[1].class_offset = text_class.first;
  nodes[1].class_length = text_class.second;
  nodes[1].text_offset = text_value.first;
  nodes[1].text_length = text_value.second;
  nodes[2].id = 3;
  nodes[2].parent = 1;
  nodes[2].kind = static_cast<uint8_t>(oos::runtime::UiNodeKind::Canvas);
  nodes[2].class_offset = canvas_class.first;
  nodes[2].class_length = canvas_class.second;
  nodes[2].canvas = raw_canvas;

  oos::runtime::NativeUiEngine ui(scene);
  assert(ui.submit(nodes, 3, reinterpret_cast<const uint8_t *>(strings.data()),
                   strings.size()));
  assert(ui.layout().size() == 3);
  assert(ui.layout()[0].width == 240);
  assert(ui.layout()[0].height == 298);
  assert(ui.layout()[1].x == 16);
  assert(ui.layout()[2].width == 208);
  assert(ui.layout()[2].height > 200);
  assert(scene.present());
  assert(graphics.last_command_count == 2);

  assert(ui.submit(nodes, 2, reinterpret_cast<const uint8_t *>(strings.data()),
                   strings.size()));
  assert(scene.present());
  assert(graphics.last_command_count == 1);

  oos::runtime::UiStyle style;
  std::string error;
  assert(
      !oos::runtime::parseTailwindClasses("position:fixed", 14, style, error));
  assert(error.find("unsupported Tailwind utility") != std::string::npos);

  oos::runtime::UiNodeRecord shrinking[3];
  shrinking[0].id = 1;
  const auto shrink_root = append("flex flex-row w-[100px] h-[20px]");
  const auto fixed_child = append("w-[80px] h-full shrink-0");
  const auto flexible_child = append("w-[80px] h-full shrink");
  shrinking[0].class_offset = shrink_root.first;
  shrinking[0].class_length = shrink_root.second;
  shrinking[1].id = 2;
  shrinking[1].parent = 1;
  shrinking[1].class_offset = fixed_child.first;
  shrinking[1].class_length = fixed_child.second;
  shrinking[2].id = 3;
  shrinking[2].parent = 1;
  shrinking[2].class_offset = flexible_child.first;
  shrinking[2].class_length = flexible_child.second;
  assert(ui.submit(shrinking, 3,
                   reinterpret_cast<const uint8_t *>(strings.data()),
                   strings.size()));
  assert(ui.layout()[1].width == 80);
  assert(ui.layout()[2].width == 20);
}

} // namespace

int main() {
  testLifecycleAndImports();
  testImportConfinement();
  testSystemSettingsAndWifiModules();
  testExecutionDeadline();
  testCanvasSceneComposition();
  testCanvas2dPixels();
  testNativeUiLayoutAndTailwind();
  return 0;
}
