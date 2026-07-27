#!/usr/bin/env bash

set -euo pipefail

cat >&2 <<'EOF'
This legacy test was retired when WPE display ownership was removed.
WPE frames must pass through the OOS host compositor; a WPE-specific process
may not switch HWC and the Nokia 2780 cover directly.

Use scripts/test-display-lifecycle.sh for the current serialized panel test.
Future in-process switching belongs in the device-independent compositor API.
EOF
exit 2
