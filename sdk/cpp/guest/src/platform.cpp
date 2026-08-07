#include "oos/sdk/guest/platform.h"

#include "app.h"

#include <cstring>

namespace oos::sdk::guest {
namespace {

app_string_t stringView(const char *value, size_t size) {
  return {reinterpret_cast<uint8_t *>(const_cast<char *>(value)), size};
}

std::string copyString(const app_string_t &value) {
  return std::string(value.ptr ? reinterpret_cast<const char *>(value.ptr) : "",
                     value.len);
}

} // namespace

GraphicsHost::GraphicsHost() {
  oos_platform_graphics_size_t size{};
  oos_platform_graphics_surface_size(&size);
  width_ = size.width;
  height_ = size.height;
}

uint32_t GraphicsHost::width() const { return width_; }
uint32_t GraphicsHost::height() const { return height_; }
float GraphicsHost::pixelsPerPoint() const {
  return oos_platform_graphics_pixels_per_point();
}
uint32_t GraphicsHost::surfaceFormat() const {
  return oos_platform_graphics_surface_format();
}
uint32_t GraphicsHost::supportedTextureFormats() const {
  return oos_platform_graphics_supported_texture_formats();
}

bool GraphicsHost::setTexture(uint32_t texture, uint32_t format, uint32_t x,
                              uint32_t y, uint32_t width, uint32_t height,
                              uint32_t row_stride, uint32_t flags,
                              const uint8_t *pixels, size_t pixel_bytes) {
  oos_platform_graphics_point_t point{x, y};
  oos_platform_graphics_size_t size{width, height};
  app_list_u8_t bytes{const_cast<uint8_t *>(pixels), pixel_bytes};
  oos_platform_graphics_error_code_t error{};
  return oos_platform_graphics_texture_set(
      texture, static_cast<oos_platform_graphics_texture_format_t>(format),
      &point, &size, row_stride,
      static_cast<oos_platform_graphics_texture_flags_t>(flags), &bytes,
      &error);
}

bool GraphicsHost::freeTexture(uint32_t texture) {
  oos_platform_graphics_error_code_t error{};
  return oos_platform_graphics_texture_free(texture, &error);
}

bool GraphicsHost::submit(const OosGfxVertex *vertices, size_t vertex_count,
                          const uint16_t *indices, size_t index_count,
                          const OosGfxDrawCommand *commands,
                          size_t command_count, uint32_t clear_rgba) {
  static_assert(sizeof(OosGfxVertex) ==
                sizeof(oos_platform_graphics_vertex_t));
  static_assert(sizeof(OosGfxDrawCommand) ==
                sizeof(oos_platform_graphics_draw_command_t));
  oos_platform_graphics_list_vertex_t vertex_list{
      reinterpret_cast<oos_platform_graphics_vertex_t *>(
          const_cast<OosGfxVertex *>(vertices)),
      vertex_count};
  app_list_u16_t index_list{const_cast<uint16_t *>(indices), index_count};
  oos_platform_graphics_list_draw_command_t command_list{
      reinterpret_cast<oos_platform_graphics_draw_command_t *>(
          const_cast<OosGfxDrawCommand *>(commands)),
      command_count};
  oos_platform_graphics_error_code_t error{};
  return oos_platform_graphics_submit(&vertex_list, &index_list, &command_list,
                                      clear_rgba, &error);
}

bool GraphicsHost::glesCapabilities(OosGlesCapabilities &result) {
  static_assert(sizeof(result) == sizeof(oos_platform_gles_capabilities_t));
  oos_platform_gles_capabilities_t capabilities{};
  oos_platform_gles_get_capabilities(0, &capabilities);
  std::memcpy(&result, &capabilities, sizeof(result));
  return capabilities.major_version != 0;
}

bool GraphicsHost::setGlesBuffer(uint32_t buffer, uint32_t size,
                                 uint32_t usage, const uint8_t *data,
                                 size_t data_size) {
  app_list_u8_t bytes{const_cast<uint8_t *>(data), data_size};
  oos_platform_gles_error_code_t error{};
  return oos_platform_gles_buffer_set(
      0, buffer, size, static_cast<oos_platform_gles_buffer_usage_t>(usage),
      &bytes, &error);
}

bool GraphicsHost::writeGlesBuffer(uint32_t buffer, uint32_t offset,
                                   const uint8_t *data, size_t data_size) {
  app_list_u8_t bytes{const_cast<uint8_t *>(data), data_size};
  oos_platform_gles_error_code_t error{};
  return oos_platform_gles_buffer_write(0, buffer, offset, &bytes, &error);
}

bool GraphicsHost::freeGlesBuffer(uint32_t buffer) {
  oos_platform_gles_error_code_t error{};
  return oos_platform_gles_buffer_free(0, buffer, &error);
}

bool GraphicsHost::setGlesShader(uint32_t shader, uint32_t stage,
                                 const char *source, size_t source_size) {
  app_string_t text = stringView(source, source_size);
  oos_platform_gles_error_code_t error{};
  return oos_platform_gles_shader_set(
      0, shader, static_cast<oos_platform_gles_shader_stage_t>(stage), &text,
      &error);
}

bool GraphicsHost::freeGlesShader(uint32_t shader) {
  oos_platform_gles_error_code_t error{};
  return oos_platform_gles_shader_free(0, shader, &error);
}

bool GraphicsHost::setGlesProgram(uint32_t program, uint32_t vertex_shader,
                                  uint32_t fragment_shader) {
  oos_platform_gles_error_code_t error{};
  return oos_platform_gles_program_set(0, program, vertex_shader,
                                       fragment_shader, &error);
}

bool GraphicsHost::freeGlesProgram(uint32_t program) {
  oos_platform_gles_error_code_t error{};
  return oos_platform_gles_program_free(0, program, &error);
}

int32_t GraphicsHost::glesAttributeLocation(uint32_t program, const char *name,
                                            size_t name_size) {
  app_string_t text = stringView(name, name_size);
  return oos_platform_gles_attribute_location(0, program, &text);
}

int32_t GraphicsHost::glesUniformLocation(uint32_t program, const char *name,
                                          size_t name_size) {
  app_string_t text = stringView(name, name_size);
  return oos_platform_gles_uniform_location(0, program, &text);
}

bool GraphicsHost::submitGles(const OosGlesCommand *commands,
                              size_t command_count, const uint32_t *data,
                              size_t data_words) {
  static_assert(sizeof(OosGlesCommand) == sizeof(oos_platform_gles_command_t));
  oos_platform_gles_list_command_t command_list{
      reinterpret_cast<oos_platform_gles_command_t *>(
          const_cast<OosGlesCommand *>(commands)),
      command_count};
  app_list_u32_t words{const_cast<uint32_t *>(data), data_words};
  oos_platform_gles_error_code_t error{};
  return oos_platform_gles_submit(0, &command_list, &words, &error);
}

void StatusBarController::setStatusBarAppearance(
    ::oos::ui::StatusBarAppearance appearance) {
  appearance.background_rgb &= 0x00ffffffu;
  oos_platform_runtime_error_code_t error{};
  const auto icons = appearance.dark_icons
                         ? OOS_PLATFORM_RUNTIME_STATUS_BAR_ICON_THEME_DARK
                         : OOS_PLATFORM_RUNTIME_STATUS_BAR_ICON_THEME_LIGHT;
  if (oos_platform_runtime_set_status_bar_style(appearance.background_rgb,
                                                 icons, &error))
    appearance_ = appearance;
}

::oos::ui::StatusBarAppearance StatusBarController::statusBarAppearance() const {
  return appearance_;
}

bool StatusBarController::setSurfaceMode(::oos::ui::SurfaceMode mode) {
  oos_platform_runtime_error_code_t error{};
  const auto native = mode == ::oos::ui::SurfaceMode::Immersive
                          ? OOS_PLATFORM_RUNTIME_SURFACE_MODE_IMMERSIVE
                          : OOS_PLATFORM_RUNTIME_SURFACE_MODE_NORMAL;
  if (!oos_platform_runtime_set_surface_mode(native, &error))
    return false;
  mode_ = mode;
  return true;
}

::oos::ui::SurfaceMode StatusBarController::surfaceMode() const {
  return mode_;
}

Device::Device() {
  oos_platform_device_descriptor_t value{};
  oos_platform_device_get_descriptor(&value);
  id_ = copyString(value.id);
  manufacturer_ = copyString(value.manufacturer);
  model_ = copyString(value.model);
  descriptor_ = {id_.c_str(),
                 manufacturer_.c_str(),
                 model_.c_str(),
                 value.android_api,
                 value.primary_width,
                 value.primary_height,
                 value.secondary_width,
                 value.secondary_height,
                 cpu_core_.c_str(),
                 cpu_arch_.c_str()};
  oos_platform_device_descriptor_free(&value);
}

const device::DeviceDescriptor &Device::descriptor() const {
  return descriptor_;
}
const device::ServiceConfiguration &Device::services() const {
  return services_;
}
device::CapabilityState Device::capability(device::Feature feature) const {
  const auto value = oos_platform_device_get_capability(
      static_cast<oos_platform_device_feature_t>(feature));
  return static_cast<device::CapabilityState>(value);
}
bool Device::initialize(const device::DeviceInitOptions &) { return true; }
void Device::shutdown() {}
device::Display &Device::display() {
  __builtin_trap();
}
input::KeyInputSource &Device::keyInput() {
  __builtin_trap();
}
const std::string &Device::lastError() const { return error_; }

int64_t wallClockSeconds() {
  return oos_platform_runtime_wall_clock_time_ms() / 1000;
}
uint32_t wallClockMinute() {
  return oos_platform_runtime_wall_clock_minutes();
}

} // namespace oos::sdk::guest
