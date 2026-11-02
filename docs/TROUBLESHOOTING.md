# Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Server exits with `SMARTPAGER_ADMIN_TOKEN must be set…` | Put two different random values (≥ 24 chars) into `.env` and load it (`set -a; source .env; set +a`). |
| `401 invalid credentials` | Check the token type: the ingest token can't list messages, the admin token can't sync as a device. Device tokens only work for their own `{device_id}`. |
| `429 rate limit exceeded` | Too many failed logins from that IP, or a script hammering the API. Wait for `Retry-After` or raise `SMARTPAGER_RATE_LIMIT_PER_MIN`. |
| Pager footer shows *Server offline · retrying* | The pager can't reach the URL. The server must be bound to the LAN, not `127.0.0.1`. Check the firewall. From another LAN device run `curl http://<ip>:8080/health`. Retries back off up to 5 min; *Settings → Sync now* retries immediately. |
| *Server rejected token* | Wrong device ID or token in setup, or the token was rotated. Re-enter it in setup mode. The pager waits 5 min between attempts. |
| Settings → Last sync shows *bad response* with an https URL | No CA certificate saved, or the wrong one. Paste the **root** CA of the server's chain (e.g. ISRG Root X1) in setup mode. Also check the pager's clock: NTP must be reachable for certificate date checks. |
| Clock shows `--:--` | No NTP and no successful sync yet. It fills in after the first sync, because server time is used as a fallback. |
| Inbox empties after a server rebuild | Expected. A new database has a new epoch, so the device performs a full resync (`reset`). |
| Colours look wrong / image is mirrored | Flash `pio run -e diag -t upload`. The bars must read red, green, blue, white from the left with USB on the left. If USB is on the right, enable *Rotate screen 180°* in setup. |
| Touch does nothing | Check the boot log or *Settings → Touch*. "Not detected" means a non-touch board or a CST328 (`0x1A`) controller, which isn't supported yet. Buttons still work. |
| USB port disappears / boot loop | Hold BOOT, tap RST, release BOOT, then upload again ([FLASHING.md](FLASHING.md)). |
| Long polling behind a proxy cuts off | Set the proxy's read timeout above 30 s, or turn instant delivery off (Settings → Polling cycles through push / 5 / 15 / 30 / 60 s). |
| Emoji missing on the device | By design: the device fonts cover Latin, Latin-1 and Latin Extended-A (all Czech letters), so emoji are removed instead of drawn as boxes. Other scripts (e.g. Cyrillic) will show as boxes. |
