#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)

"$SCRIPT_DIR/run_e2e.sh" \
  "$TOP_DIR/design/hybridacc-cc/example/test/test_conv1x1_s1_ic12oc16h48.yaml" \
  "$TOP_DIR/design/hybridacc-cc/example/test/test_conv1x1_s4_ic48oc16h48.yaml" \
  "$@"