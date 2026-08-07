#!/usr/bin/env bash
set -euo pipefail

[[ $# -eq 1 && -f "$1" ]] || {
  echo "usage: $0 MODULE.wasm" >&2
  exit 2
}

OBJDUMP=${WASM_OBJDUMP:-wasm-objdump}
if command -v "$OBJDUMP" >/dev/null; then
  imports() {
    "$OBJDUMP" -x "$1" | sed -n 's/^.* <- //p'
  }
elif command -v wasm-tools >/dev/null; then
  imports() {
    wasm-tools print "$1" |
      sed -nE 's/^[[:space:]]*\(import "([^"]+)" "([^"]+)".*/\1.\2/p'
  }
else
  echo "wasm-objdump or wasm-tools is required to validate guest imports" >&2
  exit 1
fi

allowed_env='^(pthread_(create|join|detach|cancel|self|exit|mutex_init|mutex_lock|mutex_unlock|mutex_destroy|cond_init|cond_wait|cond_timedwait|cond_signal|cond_broadcast|cond_destroy|key_create|setspecific|getspecific|key_delete)|sem_(open|wait|trywait|post|getvalue|unlink|close))$'
unexpected=0
while IFS= read -r import; do
  [[ -n "$import" ]] || continue
  if [[ "$import" == oos:platform/* ]]; then
    continue
  fi
  if [[ "$import" == env.* ]] &&
     [[ "${import#env.}" =~ $allowed_env ]]; then
    continue
  fi
  echo "unexpected Wasm import: $import" >&2
  unexpected=1
done < <(imports "$1")
exit "$unexpected"
