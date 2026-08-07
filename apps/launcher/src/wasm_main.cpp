#include "app.h"
#include "oos/apps/app_repository.h"
#include "oos/apps/launcher/launcher.h"
#include "oos/sdk/guest/platform.h"

#include <memory>

namespace {

std::unique_ptr<oos::sdk::guest::GraphicsHost> graphics;
std::unique_ptr<oos::apps::AppRepository> repository;
std::unique_ptr<oos::sdk::guest::StatusBarController> status_bar;
std::unique_ptr<oos::apps::launcher::Launcher> launcher;

oos::input::KeyEvent convert(
    const exports_oos_platform_lifecycle_key_event_t &event) {
  return {static_cast<int64_t>(oos_platform_runtime_monotonic_time_us()),
          static_cast<uint16_t>(event.code),
          static_cast<oos::input::KeyAction>(event.action), {}, {}};
}

bool launchRequestedApplication() {
  std::string id = launcher->takeLaunchRequest();
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
  status_bar = std::make_unique<oos::sdk::guest::StatusBarController>();
  launcher = std::make_unique<oos::apps::launcher::Launcher>(
      *graphics, *repository, *status_bar);
  if (!repository->initialize() || !launcher->initialize()) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  return true;
}

void exports_oos_platform_lifecycle_event(
    exports_oos_platform_lifecycle_key_event_t *event) {
  if (launcher && event)
    launcher->dispatchKey(convert(*event),
                          static_cast<int64_t>(
                              oos_platform_runtime_monotonic_time_us()));
}

bool exports_oos_platform_lifecycle_frame(
    uint64_t monotonic_time_us, uint32_t *next_delay_ms,
    exports_oos_platform_lifecycle_error_code_t *error) {
  if (!launcher ||
      !launcher->frame(static_cast<int64_t>(monotonic_time_us),
                       *next_delay_ms) ||
      !launchRequestedApplication()) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  return true;
}

void exports_oos_platform_lifecycle_shutdown() {
  if (launcher)
    launcher->shutdown();
  launcher.reset();
  status_bar.reset();
  repository.reset();
  graphics.reset();
}
