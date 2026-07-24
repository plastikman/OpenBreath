#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-buttons-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/pb_buttons/include" \
  "$root/components/pb_buttons/pb_buttons_sm.c" \
  "$root/tests/pb_buttons_host_test.c" \
  -o "$out"

"$out"
