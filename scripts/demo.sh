#!/usr/bin/env bash
# End-to-end protocol demo against a running server, using the same requests the firmware makes.
#
#   export SMARTPAGER_URL=http://127.0.0.1:8080
#   export SMARTPAGER_INGEST_TOKEN=...  SMARTPAGER_ADMIN_TOKEN=...
#   export DEVICE_ID=pager-1 DEVICE_TOKEN=spd_...
#   scripts/demo.sh
set -euo pipefail
B=${SMARTPAGER_URL:-http://127.0.0.1:8080}
: "${SMARTPAGER_INGEST_TOKEN:?}" "${SMARTPAGER_ADMIN_TOKEN:?}" "${DEVICE_TOKEN:?}"
DEV=${DEVICE_ID:-pager-1}
J='Content-Type: application/json'
py() { python3 -c "import sys,json; d=json.load(sys.stdin); $1"; }

echo "== health"; curl -sf "$B/health"; echo

echo "== 1. device: initial sync"
S=$(curl -sf -H "Authorization: Bearer $DEVICE_TOKEN" "$B/api/v1/devices/$DEV/sync?limit=50")
CUR=$(echo "$S" | py 'print(d["cursor"])'); EP=$(echo "$S" | py 'print(d["epoch"])')
echo "cursor=$CUR epoch=$EP unread=$(echo "$S" | py 'print(d["unread"])')"

echo "== 2. inject a message (ingest token)"
ID=$(curl -sf -X POST "$B/api/v1/messages" -H "Authorization: Bearer $SMARTPAGER_INGEST_TOKEN" -H "$J" \
  -d "{\"source\":\"system\",\"sender\":\"demo.sh\",\"subject\":\"Hello from the demo\",\"body\":\"Sent at $(date +%H:%M:%S). Příliš žluťoučký kůň.\",\"priority\":\"high\",\"external_id\":\"demo-$(date +%s)\"}" \
  | py 'print(d["message"]["id"])')
echo "message id=$ID"

echo "== 3. device: incremental sync from cursor $CUR (long poll, returns at once)"
S=$(curl -sf -H "Authorization: Bearer $DEVICE_TOKEN" "$B/api/v1/devices/$DEV/sync?cursor=$CUR&epoch=$EP&wait=20")
echo "$S" | py 'print("items:", [(i["id"], i["subj"]) for i in d["items"]], "cursor:", d["cursor"])'
CUR=$(echo "$S" | py 'print(d["cursor"])')

echo "== 4. device: user opened + acknowledged it (idempotent events), cursor confirmed"
EV="{\"ack_cursor\":$CUR,\"events\":[{\"id\":\"demo-r-$ID\",\"type\":\"read\",\"message_id\":$ID},{\"id\":\"demo-a-$ID\",\"type\":\"ack\",\"message_id\":$ID}]}"
curl -sf -X POST "$B/api/v1/devices/$DEV/events" -H "Authorization: Bearer $DEVICE_TOKEN" -H "$J" -d "$EV"; echo
echo "   retry of the same batch:"
curl -sf -X POST "$B/api/v1/devices/$DEV/events" -H "Authorization: Bearer $DEVICE_TOKEN" -H "$J" -d "$EV"; echo

echo "== 5. admin view of message $ID"
curl -sf -H "Authorization: Bearer $SMARTPAGER_ADMIN_TOKEN" "$B/api/v1/messages/$ID" \
  | py 'print({k: d[k] for k in ("id","subject","delivered_at","read_at","acknowledged_at")})'
