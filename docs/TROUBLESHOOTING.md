# Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Server exits with `SMARTPAGER_ADMIN_TOKEN must be set…` | Put two different random values (≥ 24 chars) into `.env` and load it (`set -a; source .env; set +a`). |
| `401 invalid credentials` | Check the token type: the ingest token can't list messages, the admin token can't sync as a device. Device tokens only work for their own `{device_id}`. |
| `429 rate limit exceeded` | Too many failed logins from that IP, or a script hammering the API. Wait for `Retry-After` or raise `SMARTPAGER_RATE_LIMIT_PER_MIN`. |
| Pager footer shows *Server offline · retrying* | The pager can't reach the URL. The server must be bound to the LAN, not `127.0.0.1`. Check the firewall. From another LAN device run `curl http://<ip>:8080/health`. Retries back off up to 30 s; *Settings → Sync now* retries immediately. |
| *Server rejected token* | Wrong device ID or token in setup, or the token was rotated. Re-enter it in setup mode. The pager waits 5 min between attempts. |
| Settings → Last sync shows *network error* with an https URL | The certificate didn't verify. Let's Encrypt works out of the box. For another CA, paste its **root** in setup mode, and remove a stale custom CA if you moved to Let's Encrypt. Check that the URL host matches the certificate, and that NTP is reachable for date checks. |
| `make up-public`: no certificate | Ports 80/443 must reach this machine from the internet and DNS must point at it. See `make logs-caddy`. |
| Admin console 404 at `https://…/admin` | By design: the admin is only on the local port. Use `http://127.0.0.1:8080/admin` on the server, or an SSH tunnel / VPN ([DEPLOY_PUBLIC.md](DEPLOY_PUBLIC.md#6-reaching-the-admin-console-remotely)). |
| Settings → Update shows *Bad signature* | The published manifest wasn't signed with the key this pager was built with. Publish with `make ota-publish` from the machine that holds `secrets/ota_signing_key.pem`. |
| Settings → Update shows *Not configured* | The firmware was built without `firmware/include/ota_pubkey.h`. Run `make ota-keygen`, then `make fw-flash` once over USB. |
| Clock shows `--:--` | No NTP and no successful sync yet. It fills in after the first sync, because server time is used as a fallback. |
| Inbox empties after a server rebuild | Expected. A new database has a new epoch, so the device performs a full resync (`reset`). |
| Colours look wrong / image is mirrored | Flash `pio run -e diag -t upload`. The bars must read red, green, blue, white from the left with USB on the left. If USB is on the right, enable *Rotate screen 180°* in setup. |
| Touch does nothing | Check the boot log or *Settings → Touch*. "Not detected" means a non-touch board or a CST328 (`0x1A`) controller, which isn't supported yet. Buttons still work. |
| USB port disappears / boot loop | Hold BOOT, tap RST, release BOOT, then upload again ([FLASHING.md](FLASHING.md)). |
| Long polling behind a proxy cuts off | Set the proxy's read timeout above 30 s, or turn instant delivery off (Settings → Polling cycles through push / 5 / 15 / 30 / 60 s). |
| Emoji missing on the device | By design: the device fonts cover Latin, Latin-1 and Latin Extended-A (all Czech letters), so emoji are removed instead of drawn as boxes. Other scripts (e.g. Cyrillic) will show as boxes. |
