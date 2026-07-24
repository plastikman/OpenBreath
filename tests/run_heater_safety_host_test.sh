#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-heater-safety-test"

# The real pb_heater.h must win over the tests/stubs one (it carries the fault
# enum + the pure pb_heater_eval_trip inline), so its include dir comes FIRST;
# esp_err.h is picked up from the stubs.
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_heater/include" \
  -I"$root/tests/stubs" \
  "$root/tests/pb_heater_safety_host_test.c" \
  -lm -o "$out"

"$out"
