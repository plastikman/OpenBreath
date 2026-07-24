#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-ntc-status-test"

# The real pb_ntc.h must win over the tests/stubs one (it carries the raw/rail
# thresholds + the pure pb_ntc_classify inline), so its include dir comes FIRST;
# esp_err.h is picked up from the stubs.
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_ntc/include" \
  -I"$root/tests/stubs" \
  "$root/tests/pb_ntc_status_host_test.c" \
  -lm -o "$out"

"$out"
