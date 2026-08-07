#include "app.h"
#include "oos/apps/app_repository.h"
#include "oos/apps/settings/settings.h"
#include "oos/sdk/guest/platform.h"
#include "oos/ui/system_ui_settings.h"

#include <memory>

namespace {

std::unique_ptr<oos::sdk::guest::GraphicsHost> graphics;
std::unique_ptr<oos::apps::AppRepository> repository;
std::unique_ptr<oos::sdk::guest::Device> device;
std::unique_ptr<oos::ui::SystemUiSettings> system_settings;
std::unique_ptr<oos::sdk::guest::StatusBarController> status_bar;
std::unique_ptr<oos::apps::settings::Settings> settings;

oos::input::KeyEvent convert(
    const exports_oos_platform_lifecycle_key_event_t &event) {
  return {static_cast<int64_t>(oos_platform_runtime_monotonic_time_us()),
          static_cast<uint16_t>(event.code),
          static_cast<oos::input::KeyAction>(event.action), {}, {}};
}

bool launchRequestedApplication() {
  std::string id = settings->takeLaunchRequest();
  if (id.empty())
    return true;
  app_string_t value{
      reinterpret_cast<uint8_t *>(const_cast<char *>(id.data())), id.size()};
  oos_platform_applications_error_code_t error{};
  return oos_platform_applications_launch(&value, &error);
}

} // namespace

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  graphics = std::make_unique<oos::sdk::guest::GraphicsHost>();
  repository = std::make_unique<oos::apps::AppRepository>();
  device = std::make_unique<oos::sdk::guest::Device>();
  system_settings = std::make_unique<oos::ui::SystemUiSettings>();
  status_bar = std::make_unique<oos::sdk::guest::StatusBarController>();
  settings = std::make_unique<oos::apps::settings::Settings>(
      *graphics, *repository, *device, *system_settings, *status_bar, "");
  if (!repository->initialize() || !system_settings->initialize() ||
      !settings->initialize()) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  return true;
}

void exports_oos_platform_lifecycle_event(
    exports_oos_platform_lifecycle_key_event_t *event) {
  if (settings && event)
    settings->dispatchKey(convert(*event),
                          static_cast<int64_t>(
                              oos_platform_runtime_monotonic_time_us()));
}

bool exports_oos_platform_lifecycle_frame(
    uint64_t monotonic_time_us, uint32_t *next_delay_ms,
    exports_oos_platform_lifecycle_error_code_t *error) {
  if (!settings ||
      !settings->frame(static_cast<int64_t>(monotonic_time_us),
                       *next_delay_ms) ||
      !launchRequestedApplication()) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  return true;
}

void exports_oos_platform_lifecycle_shutdown() {
  if (settings)
    settings->shutdown();
  settings.reset();
  status_bar.reset();
  system_settings.reset();
  device.reset();
  repository.reset();
  graphics.reset();
}
