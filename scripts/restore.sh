#!/usr/bin/env bash
# Restore a backup made by scripts/backup.sh:  scripts/restore.sh backups/smartpager-….tar.gz
#   - takes a safety backup of the current state first (if the server is running)
#   - .env: restored if this machine has none; otherwise left alone and the backup's copy is
#     written next to it as .env.restored-* for comparison
#   - replaces the database (server is stopped and restarted) and reinstalls the OTA release
# Set YES=1 to skip the confirmation prompt.
set -euo pipefail
cd "$(dirname "$0")/.."

archive=${1:-}
[ -n "$archive" ] && [ -f "$archive" ] || { echo "usage: make restore FILE=backups/smartpager-….tar.gz" >&2; exit 1; }

umask 077
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
tar -xzf "$archive" -C "$tmp"
[ -f "$tmp/smartpager.db" ] && [ -f "$tmp/env" ] || { echo "$archive is not a SmartPager backup" >&2; exit 1; }

python3 - "$tmp/smartpager.db" <<'EOF'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
ok = db.execute("PRAGMA integrity_check").fetchone()[0]
if ok != "ok":
    sys.exit(f"backup database is damaged: {ok}")
m = db.execute("SELECT count(*) FROM messages").fetchone()[0]
d = db.execute("SELECT count(*) FROM devices").fetchone()[0]
print(f"backup contains {m} messages and {d} devices")
EOF

if [ "${YES:-}" != 1 ]; then
  read -r -p "Replace this server's database with $archive? [y/N] " answer
  [ "$answer" = y ] || [ "$answer" = Y ] || { echo "cancelled"; exit 1; }
fi

if [ -n "$(docker compose ps -q smartpager 2>/dev/null)" ]; then
  echo "safety backup of the current state:"
  scripts/backup.sh
fi

stamp=$(date +%Y%m%d-%H%M%S)
if [ ! -f .env ]; then
  cp "$tmp/env" .env && chmod 600 .env && echo "restored .env"
elif ! cmp -s "$tmp/env" .env; then
  cp "$tmp/env" ".env.restored-$stamp" && chmod 600 ".env.restored-$stamp"
  echo "kept the existing .env; the backup's copy is .env.restored-$stamp (compare and merge by hand)"
fi

make --no-print-directory import-db DB="$tmp/smartpager.db"
if [ -f "$tmp/firmware/manifest.json" ]; then
  chmod 644 "$tmp"/firmware/*   # signed release, not secret; the server (uid 10001) must read it
  make --no-print-directory ota-install DIR="$tmp/firmware"
fi
echo "restore complete"
