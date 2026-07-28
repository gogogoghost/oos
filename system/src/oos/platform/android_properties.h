#pragma once

#include <sys/system_properties.h>

extern "C" {

int property_get(const char *key, char *value, const char *default_value);
int property_set(const char *key, const char *value);
}

#ifndef PROPERTY_KEY_MAX
#define PROPERTY_KEY_MAX PROP_NAME_MAX
#endif

#ifndef PROPERTY_VALUE_MAX
#define PROPERTY_VALUE_MAX PROP_VALUE_MAX
#endif
