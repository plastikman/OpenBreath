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
grep -q "id=atin" "$portal"
grep -q "id=dtin" "$portal"
grep -q "id=autobtn" "$portal"
grep -q "id=drybtn" "$portal"
grep -q "id=amsg" "$portal"
grep -q "id=dmsg" "$portal"
grep -q "document.getElementById('atin').value" "$portal"
grep -q "document.getElementById('dtin').value" "$portal"
grep -q "Rejected: '+errText" "$portal"
grep -q 'strcmp(s_replay\[i\].actor_id, actor_id)' "$httpd"

# Remembered mode parameters must stay in the state document: the dashboard
# pre-fills from them and the front-panel buttons re-arm from them.
for field in manual_target_c auto_target_c auto_bed_threshold_c dry_target_c dry_hours; do
    grep -q "\"$field\"" "$httpd" || {
        echo "state document is missing params.$field" >&2
        exit 1
    }
done
grep -q 'pb_policy_get_params' "$httpd" || {
    echo "state document no longer reads the remembered params" >&2
    exit 1
}
grep -q 'expected->valuedouble < UINT32_MAX' "$httpd"

if grep -q 'cJSON_AddStringToObject(lease, "id"' "$httpd"; then
    echo "unauthenticated state exposes raw lease id" >&2
    exit 1
fi

echo "api v2 static contract checks: PASS"
