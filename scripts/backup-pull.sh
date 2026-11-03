#!/usr/bin/env bash
# Copy the server's backup archives to this computer (off-site copy), over SSH:
#   scripts/backup-pull.sh rocky@pager.example.com            (or: make backup-pull HOST=…)
# Only archives not yet here are downloaded, into backups/<host>/ (git-ignored, mode 700).
# Options (environment): SSH_KEY=path/to/key  REMOTE_DIR=smartpager  LOCAL_KEEP=60
# The archives contain the server's .env (secrets): keep this disk encrypted (FileVault).
set -euo pipefail
cd "$(dirname "$0")/.."

host=${1:-}
[ -n "$host" ] || { echo "usage: make backup-pull HOST=user@server [SSH_KEY=~/.ssh/key]" >&2; exit 1; }
remote_dir=${REMOTE_DIR:-smartpager}
ssh_opts=(-o BatchMode=yes)
[ -n "${SSH_KEY:-}" ] && ssh_opts+=(-i "$SSH_KEY")

umask 077
dest="backups/${host#*@}"
mkdir -p "$dest"
chmod 700 backups "$dest"

remote=$(ssh "${ssh_opts[@]}" "$host" "cd '$remote_dir/backups' 2>/dev/null && ls -1 smartpager-*.tar.gz 2>/dev/null" || true)
[ -n "$remote" ] || { echo "no backups on $host:$remote_dir/backups — run 'make backup' there" >&2; exit 1; }

new=0
while read -r f; do
  [ -f "$dest/$f" ] && continue
  scp -q "${ssh_opts[@]}" "$host:$remote_dir/backups/$f" "$dest/$f.part" && mv "$dest/$f.part" "$dest/$f"
  tar -tzf "$dest/$f" ./smartpager.db >/dev/null   # complete and readable
  echo "pulled $dest/$f"
  new=$((new + 1))
done <<< "$remote"
echo "$new new archive(s); $(ls -1 "$dest"/smartpager-*.tar.gz | wc -l | tr -d ' ') in $dest"

ls -1t "$dest"/smartpager-*.tar.gz | tail -n +"$(( ${LOCAL_KEEP:-60} + 1 ))" | while read -r old; do
  rm -f "$old" && echo "removed old $old"
done
