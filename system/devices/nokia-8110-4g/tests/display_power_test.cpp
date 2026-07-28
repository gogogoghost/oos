#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include <hardware/lights.h>
#include <hardware/power.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

void onInvalidate(const hwc_procs_t *) {}
void onVsync(const hwc_procs_t *, int, int64_t) {}
void onHotplug(const hwc_procs_t *, int, int) {}

const hwc_procs_t kHwcProcs = {
    .invalidate = onInvalidate,
    .vsync = onVsync,
    .hotplug = onHotplug,
};

bool writeText(const char *path, const char *text) {
  FILE *file = std::fopen(path, "w");
  if (!file) {
    std::fprintf(stderr, "open %s failed: %s\n", path, std::strerror(errno));
    return false;
  }
  const bool success = std::fputs(text, file) >= 0;
  if (!success)
    std::fprintf(stderr, "write %s failed: %s\n", path, std::strerror(errno));
  std::fclose(file);
  return success;
}

light_device_t *openBacklight() {
  const hw_module_t *module = nullptr;
  const int load_result = hw_get_module(LIGHTS_HARDWARE_MODULE_ID, &module);
  if (load_result != 0 || !module || !module->methods ||
      !module->methods->open) {
    std::fprintf(stderr, "load lights HAL failed: %d\n", load_result);
    return nullptr;
  }

  light_device_t *light = nullptr;
  const int open_result = module->methods->open(
      module, LIGHT_ID_BACKLIGHT, reinterpret_cast<hw_device_t **>(&light));
  if (open_result != 0 || !light || !light->set_light) {
    std::fprintf(stderr, "open lights backlight failed: %d\n", open_result);
    return nullptr;
  }
  return light;
}

bool setBacklight(light_device_t *light, unsigned brightness) {
  const unsigned value = brightness & 0xff;
  const light_state_t state = {
      .color = 0xff000000u | (value << 16) | (value << 8) | value,
      .flashMode = LIGHT_FLASH_NONE,
      .flashOnMS = 0,
      .flashOffMS = 0,
      .brightnessMode = BRIGHTNESS_MODE_USER,
  };
  const int result = light->set_light(light, &state);
  std::fprintf(stderr, "lights HAL backlight=%u: %d\n", brightness, result);
  return result == 0;
}

} // namespace

int main(int argc, char **argv) {
  const unsigned seconds =
      argc == 2 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10))
                : 120;

  writeText("/sys/power/wake_lock", "oos-display-power-test\n");

  const hw_module_t *power_hardware = nullptr;
  power_module_t *power = nullptr;
  const int power_result =
      hw_get_module(POWER_HARDWARE_MODULE_ID, &power_hardware);
  if (power_result == 0 && power_hardware) {
    power = reinterpret_cast<power_module_t *>(
        const_cast<hw_module_t *>(power_hardware));
    if (power->init)
      power->init(power);
    if (power->setInteractive)
      power->setInteractive(power, 1);
    std::fprintf(stderr, "power HAL interactive=true\n");
  } else {
    std::fprintf(stderr, "load power HAL failed: %d\n", power_result);
  }

  light_device_t *backlight = openBacklight();
  if (!backlight)
    return 1;
  setBacklight(backlight, 0);

  const hw_module_t *hwc_module = nullptr;
  hwc_composer_device_1_t *hwc = nullptr;
  const int module_result = hw_get_module(HWC_HARDWARE_MODULE_ID, &hwc_module);
  const int open_result =
      module_result == 0 && hwc_module ? hwc_open_1(hwc_module, &hwc) : -1;
  if (module_result != 0 || open_result != 0 || !hwc) {
    std::fprintf(stderr, "open HWC1 failed: module=%d open=%d\n", module_result,
                 open_result);
    backlight->common.close(&backlight->common);
    return 1;
  }
  if (hwc->registerProcs)
    hwc->registerProcs(hwc, &kHwcProcs);

  if (!hwc->setPowerMode) {
    std::fprintf(stderr, "HWC setPowerMode is unavailable\n");
    return 1;
  }

  int result = hwc->setPowerMode(hwc, HWC_DISPLAY_PRIMARY, HWC_POWER_MODE_OFF);
  std::fprintf(stderr, "HWC power OFF: %d\n", result);
  usleep(500000);
  result = hwc->setPowerMode(hwc, HWC_DISPLAY_PRIMARY, HWC_POWER_MODE_NORMAL);
  std::fprintf(stderr, "HWC power NORMAL: %d\n", result);

  if (hwc->eventControl) {
    result = hwc->eventControl(hwc, HWC_DISPLAY_PRIMARY, HWC_EVENT_VSYNC, 1);
    std::fprintf(stderr, "HWC VSYNC enable: %d\n", result);
  }

  usleep(200000);
  setBacklight(backlight, 255);
  std::fprintf(stderr, "LCD power probe active for %u seconds\n", seconds);
  sleep(seconds);

  setBacklight(backlight, 0);
  if (hwc->eventControl)
    hwc->eventControl(hwc, HWC_DISPLAY_PRIMARY, HWC_EVENT_VSYNC, 0);
  hwc->setPowerMode(hwc, HWC_DISPLAY_PRIMARY, HWC_POWER_MODE_OFF);
  hwc_close_1(hwc);
  backlight->common.close(&backlight->common);
  if (power && power->setInteractive)
    power->setInteractive(power, 0);
  writeText("/sys/power/wake_unlock", "oos-display-power-test\n");
  return 0;
}
