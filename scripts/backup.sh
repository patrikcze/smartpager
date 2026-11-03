#!/usr/bin/env bash
# Back up everything the SmartPager server needs to be rebuilt into one archive:
#   backups/smartpager-YYYYmmdd-HHMMSS.tar.gz
#     smartpager.db   consistent SQLite snapshot (online backup API), integrity-checked
#     env             the server's .env (admin token, settings) — the archive is therefore SECRET
#     firmware/       the installed signed OTA release, if any
# Keeps the newest $BACKUP_KEEP archives (default 14). Run from anywhere; used by `make backup`
# and by the nightly systemd timer (`make backup-timer`).
#
# If BACKUP_NOTIFY_TOKEN (an ingest token, `make ingest-token NAME=backup`) is set in .env,
# a failed backup pages you through the local server.
set -euo pipefail
cd "$(dirname "$0")/.."

[ -f .env ] || { echo "missing .env — run from the SmartPager checkout on the server" >&2; exit 1; }
set -a; . ./.env; set +a

keep=${BACKUP_KEEP:-14}
stamp=$(date +%Y%m%d-%H%M%S)
name=smartpager-$stamp
umask 077
mkdir -p backups
chmod 700 backups
work=$(mktemp -d "backups/.tmp-$stamp.XXXX")

notify_failure() {
  local status=$?
  rm -rf "$work"
  [ "$status" -eq 0 ] && return
  echo "backup FAILED (exit $status)" >&2
  if [ -n "${BACKUP_NOTIFY_TOKEN:-}" ]; then
    # Posted from inside the container, so host port mappings (or an SSH tunnel on that port)
    # don't matter. The token goes through the environment, never the command line.
    docker compose exec -T -e "TOKEN=$BACKUP_NOTIFY_TOKEN" smartpager python - "$(hostname -s)" "$status" "$stamp" <<'EOF' \
      && echo "failure alert sent" >&2 || echo "could not send the failure alert" >&2
import json, os, sys, urllib.request
host, status, stamp = sys.argv[1:4]
msg = {"source": "system", "sender": host, "subject": "SmartPager backup failed",
       "body": f"Exit status {status} at {stamp}. Check: journalctl -u smartpager-backup",
       "priority": "high", "external_id": f"backup-failed-{stamp}"}
req = urllib.request.Request("http://127.0.0.1:8080/api/v1/messages", json.dumps(msg).encode(),
                             {"Authorization": f"Bearer {os.environ['TOKEN']}",
                              "Content-Type": "application/json"})
urllib.request.urlopen(req, timeout=10)
EOF
  fi
  exit "$status"
}
trap notify_failure EXIT

cid=$(docker compose ps -q smartpager)
[ -n "$cid" ] || { echo "the smartpager container is not running" >&2; exit 1; }

# Snapshot inside the container (the database lives in a Docker volume), check it, copy it out.
docker compose exec -T smartpager python - "$stamp" <<'EOF'
import sqlite3, sys
dst = f"/data/backup-{sys.argv[1]}.db"
src = sqlite3.connect("/data/smartpager.db")
out = sqlite3.connect(dst)
src.backup(out)
out.execute("PRAGMA journal_mode=DELETE")
ok = out.execute("PRAGMA integrity_check").fetchone()[0]
n = out.execute("SELECT count(*) FROM messages").fetchone()[0]
out.close()
if ok != "ok":
    sys.exit(f"integrity check failed: {ok}")
print(f"database snapshot ok ({n} messages)")
EOF
docker cp -q "$cid:/data/backup-$stamp.db" "$work/smartpager.db"
docker compose exec -T smartpager rm -f "/data/backup-$stamp.db"

cp .env "$work/env"
if docker compose exec -T smartpager test -f /data/firmware/manifest.json; then
  docker cp -q "$cid:/data/firmware" "$work/firmware"
fi

tar -czf "backups/$name.tar.gz.part" -C "$work" .
mv "backups/$name.tar.gz.part" "backups/$name.tar.gz"
echo "backups/$name.tar.gz ($(du -h "backups/$name.tar.gz" | cut -f1))"

# Rotation: newest $keep archives stay.
ls -1t backups/smartpager-*.tar.gz | tail -n +"$((keep + 1))" | while read -r old; do
  rm -f "$old" && echo "removed old $old"
done
