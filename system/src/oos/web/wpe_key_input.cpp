#include "oos/web/wpe_key_input.h"

#include <wpe/wpe.h>

namespace oos::web {

uint32_t wpeKeySymbol(uint16_t code) {
  if (code >= 2 && code <= 10)
    return WPE_KEY_1 + (code - 2);
  switch (code) {
  case 11:
    return WPE_KEY_0;
  case 103:
    return WPE_KEY_Up;
  case 105:
    return WPE_KEY_Left;
  case 106:
    return WPE_KEY_Right;
  case 108:
    return WPE_KEY_Down;
  case 114:
    return WPE_KEY_AudioLowerVolume;
  case 115:
    return WPE_KEY_AudioRaiseVolume;
  case 116:
    return WPE_KEY_PowerOff;
  case 139:
    return WPE_KEY_Menu;
  case 158:
    return WPE_KEY_BackSpace;
  case 231:
    return WPE_KEY_Phone;
  case 352:
    return WPE_KEY_Return;
  case 357:
    return WPE_KEY_Option;
  case 522:
    return WPE_KEY_asterisk;
  case 523:
    return WPE_KEY_numbersign;
  default:
    return 0;
  }
}

bool dispatchWpeKey(wpe_view_backend *backend,
                    const OosSurfaceTransportKey &key) {
  const uint32_t symbol = wpeKeySymbol(key.code);
  if (!backend || !symbol || key.action > 2)
    return false;
  wpe_input_keyboard_event event = {
      static_cast<uint32_t>(key.timestamp_us / 1000), symbol, key.code,
      key.action != 0, 0};
  wpe_view_backend_dispatch_keyboard_event(backend, &event);
  return true;
}

} // namespace oos::web
