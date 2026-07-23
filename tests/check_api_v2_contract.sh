#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
httpd="$root/components/pb_httpd/pb_httpd.c"
portal="$root/components/pb_portal/pb_portal.c"

for route in info state command heartbeat events health; do
    grep -q "\"/api/v2/$route\"" "$httpd" || {
        echo "missing API v2 route: $route" >&2
        exit 1
    }
done

if grep -Eq '\.uri = "/(status|target|heartbeat|reset)"' "$httpd"; then
    echo "alpha API route was reintroduced" >&2
    exit 1
fi

grep -q "EventSource('/api/v2/events')" "$portal"
grep -q "if(polling)return" "$portal"
grep -q "lease_id:lease" "$portal"
grep -q "lease=r.lease_id" "$portal"
grep -q "l.owner!==actor" "$portal"
grep -q "s.target.maximum_c" "$portal"
grep -q 'strcmp(s_replay\[i\].actor_id, actor_id)' "$httpd"
grep -q 'expected->valuedouble < UINT32_MAX' "$httpd"

if grep -q 'cJSON_AddStringToObject(lease, "id"' "$httpd"; then
    echo "unauthenticated state exposes raw lease id" >&2
    exit 1
fi

echo "api v2 static contract checks: PASS"
