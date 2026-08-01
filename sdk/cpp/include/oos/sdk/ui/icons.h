#pragma once

#include <lvgl.h>

namespace oos::sdk::ui::icons {

// OOS system icons use only LVGL's bundled Font Awesome 5 Free subset. Keep
// every mapping here so the visual source and licensing remain auditable.
inline constexpr char kPhone[] = LV_SYMBOL_CALL;
inline constexpr char kMessages[] = LV_SYMBOL_ENVELOPE;
inline constexpr char kContacts[] = LV_SYMBOL_LIST;
inline constexpr char kCamera[] = LV_SYMBOL_IMAGE;
inline constexpr char kFiles[] = LV_SYMBOL_DIRECTORY;
inline constexpr char kSettings[] = LV_SYMBOL_SETTINGS;

inline constexpr char kWifi[] = LV_SYMBOL_WIFI;
inline constexpr char kCharge[] = LV_SYMBOL_CHARGE;
inline constexpr char kBatteryFull[] = LV_SYMBOL_BATTERY_FULL;
inline constexpr char kBatteryThreeQuarters[] = LV_SYMBOL_BATTERY_3;
inline constexpr char kBatteryHalf[] = LV_SYMBOL_BATTERY_2;
inline constexpr char kBatteryQuarter[] = LV_SYMBOL_BATTERY_1;
inline constexpr char kBatteryEmpty[] = LV_SYMBOL_BATTERY_EMPTY;

} // namespace oos::sdk::ui::icons
