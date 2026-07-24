#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
httpd="$root/components/pb_httpd/pb_httpd.c"
# The dashboard/control UI now lives in the embedded SPA, not a C string.
portal="$root/components/pb_portal/www/app.html"

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

# Dashboard/control ownership invariants (now in the embedded SPA app.html):
grep -q "EventSource('/api/v2/events')" "$portal"    # SSE observer
grep -q "if(polling)" "$portal"                      # serialized poll fallback (no overlapping fetches)
grep -q "lease_id:lease" "$portal"                   # heartbeat sends the exact device-issued lease
grep -q "lease=r.lease_id" "$portal"                 # lease taken only from the command response
grep -q "l.owner!==actor" "$portal"                  # a stale/foreign lease is dropped
grep -q "s.target.maximum_c" "$portal"               # UI honors the runtime max-target ceiling
grep -q 'id="a-range"' "$portal"                     # Automatic: chamber-target control present
grep -q 'id="d-range"' "$portal"                     # Dry: target control present
grep -q 'id="a-action"' "$portal"                    # Automatic: primary action present
grep -q 'id="d-action"' "$portal"                    # Dry: primary action present
grep -q 'id="a-msg"' "$portal"                       # Automatic: command feedback line present
grep -q 'id="d-msg"' "$portal"                       # Dry: command feedback line present
grep -q "command('auto', {target_c:fields.autoT.val" "$portal"         # auto sends the user's target+threshold
grep -q "command('drying_start', {target_c:fields.dryT.val" "$portal"  # dry sends the user's target+hours
grep -q "Rejected: '+" "$portal"                     # command rejection surfaced to the user
grep -q 'strcmp(s_replay\[i\].actor_id, actor_id)' "$httpd"

# Remembered mode parameters must stay in the authoritative snapshot and state
# document: the dashboard pre-fills from them and buttons re-arm from them.
grep -q "s.params" "$portal" || {
    echo "dashboard no longer reads the params snapshot" >&2
    exit 1
}
for field in manual_target_c auto_target_c auto_bed_threshold_c dry_target_c dry_hours; do
    grep -q "\"$field\"" "$httpd" || {
        echo "state document is missing params.$field" >&2
        exit 1
    }
    grep -q "pr.$field" "$portal" || {
        echo "dashboard no longer pre-fills from params.$field" >&2
        exit 1
    }
done
grep -q 's->params.manual_target_c' "$httpd" || {
    echo "state document no longer uses the lock-consistent params snapshot" >&2
    exit 1
}
grep -q 'expected->valuedouble < UINT32_MAX' "$httpd"

if grep -q 'cJSON_AddStringToObject(lease, "id"' "$httpd"; then
    echo "unauthenticated state exposes raw lease id" >&2
    exit 1
fi

echo "api v2 static contract checks: PASS"
