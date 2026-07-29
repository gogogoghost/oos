#pragma once

#include "oos/compositor/surface_transport.h"

struct wpe_view_backend;

namespace oos::web {

uint32_t wpeKeySymbol(uint16_t code);
bool dispatchWpeKey(wpe_view_backend *backend,
                    const OosSurfaceTransportKey &key);

} // namespace oos::web
