#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-policy-test"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/tests/stubs" \
  -I"$root/components/pb_policy/include" \
  -I"$root/components/pb_buttons/include" \
  "$root/components/pb_policy/pb_policy.c" \
  "$root/tests/pb_policy_host_test.c" \
  -lm -o "$out"

"$out"
