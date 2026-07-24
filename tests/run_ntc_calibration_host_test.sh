#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-ntc-cal-test"

# The real pb_ntc.h must win over the tests/stubs one, so its include dir comes
# FIRST; esp_err.h is picked up from the stubs.
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_ntc/include" \
  -I"$root/tests/stubs" \
  "$root/tests/pb_ntc_calibration_host_test.c" \
  -lm -o "$out"

"$out"
