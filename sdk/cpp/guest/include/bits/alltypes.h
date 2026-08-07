#pragma once

// libc++'s WASI configuration expects musl's private mbstate_t header. OOS
// guests use picolibc, which exposes the same public type through this header.
#ifdef __NEED_mbstate_t
#include <bits/types/mbstate_t.h>
#endif
