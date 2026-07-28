#!/usr/bin/env bash

oos_wpe_load_features() {
  local feature_file=$1
  local line name value

  [[ -f "$feature_file" ]] || {
    echo "Missing shared WPE feature profile: $feature_file" >&2
    return 1
  }
  OOS_WPE_CMAKE_OPTIONS=()
  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -z "$line" || "$line" == \#* ]] && continue
    if [[ ! "$line" =~ ^([A-Z][A-Z0-9_]*)=(ON|OFF)$ ]]; then
      echo "Invalid WPE feature entry: $line" >&2
      return 1
    fi
    name=${BASH_REMATCH[1]}
    value=${BASH_REMATCH[2]}
    OOS_WPE_CMAKE_OPTIONS+=("-D$name=$value")
  done <"$feature_file"
  [[ ${#OOS_WPE_CMAKE_OPTIONS[@]} -gt 0 ]] || {
    echo "Shared WPE feature profile is empty: $feature_file" >&2
    return 1
  }
  OOS_WPE_FEATURE_OPTIONS="${OOS_WPE_CMAKE_OPTIONS[*]}"
  export OOS_WPE_FEATURE_OPTIONS
}

oos_wpe_verify_features() {
  local config_header=$1
  local cache_file=${2:-"$(dirname "$config_header")/CMakeCache.txt"}
  local option name value expected

  [[ -f "$config_header" && -f "$cache_file" ]] || {
    echo "Missing WPE generated configuration: $config_header or $cache_file" >&2
    return 1
  }
  for option in "${OOS_WPE_CMAKE_OPTIONS[@]}"; do
    name=${option%%=*}
    name=${name#-D}
    value=${option#*=}
    [[ "$value" == ON ]] && expected=1 || expected=0
    grep -Eq "^$name:BOOL=$value$" "$cache_file" || {
      echo "WPE CMake verification failed: expected $name=$value" >&2
      return 1
    }
    if grep -q "^#define $name " "$config_header" &&
       ! grep -q "^#define $name $expected$" "$config_header"; then
      echo "WPE generated feature mismatch: expected $name=$expected" >&2
      return 1
    fi
  done
}
