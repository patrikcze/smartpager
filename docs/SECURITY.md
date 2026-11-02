# Security

## What is encrypted

| Path | Recommended setup ([GUIDE.md](GUIDE.md)) | Home-LAN test mode (`http://`) |
|---|---|---|
| Pager ⇄ server (messages, device token, read/ack) | **Encrypted:** HTTPS via Caddy + Let's Encrypt. The pager verifies the certificate chain and hostname | **Not encrypted.** Readable by anyone who can capture traffic on that network |
| Scripts / webhooks / e-mail connector → server | **Encrypted** (HTTPS) | Not encrypted |
| Caddy → SmartPager server | Plain HTTP inside the Docker network on the same machine, never on the wire | n/a |
| Admin console | Only on `127.0.0.1` (same machine) or through your SSH tunnel / VPN, which encrypts it | Same, unless you bound it to the LAN |
| Setup portal (phone → pager) | Plain HTTP on the pager's own WPA2 network, with a random one-time password shown only on the pager screen | Same |
| Firmware updates | Delivered over HTTPS, and **signed**: authenticity doesn't depend on the transport | Signed, but not encrypted in transit |
| Messages stored on the server (SQLite) | **Not encrypted at rest.** Protected by the machine's own disk encryption (e.g. FileVault) and file permissions | Same |
| Messages cached on the pager (last 40) | **Not encrypted at rest** (see below) | Same |

In short:
- With HTTPS, nobody between your pager or script and your server can read or change messages.
- The server and the pager themselves hold messages in readable form, so this is **not end-to-end encryption**.
- Anyone with access to the server machine, its backups, or the physical pager can read stored messages.
- Keep sensitive content short, and use retention (`SMARTPAGER_RETENTION_DAYS`) to limit how much is kept.

## What is reachable from the internet

With `make up-public`, Caddy forwards only:
- the pager endpoints (`/api/v1/devices/{id}/sync`, `…/events`, `…/firmware`, `…/firmware/image`)
- message creation (`POST /api/v1/messages`) and `POST /api/v1/webhooks/*`
- `GET /health`

Everything else, including `/admin`, `/docs` and the whole admin API, gets a 404 at the proxy. The app independently refuses admin requests that arrive through the proxy.

**Why there's no IP allow-list for the admin.** Docker Desktop on macOS presents every incoming connection, including internet traffic, as `192.168.65.1`, a private address. An allow-list based on IP ranges would silently admit everyone. Keeping the admin off the public site works the same on every platform.

## Credentials

| Secret | Stored | Notes |
|---|---|---|
| Admin token | server environment (`.env`, never committed) | full management API and the `/admin` console |
| Ingest tokens | named per integration (`make ingest-token NAME=…`): server stores only the SHA-256. Optional shared one in `.env` | create messages and webhooks only. Revocable one by one; each message records its `origin` |
| Device token | server: SHA-256 hash only. Device: NVS | one per device, rotatable (`POST /devices/{id}/token`). Only accepted for that device's own sync and events endpoints |
| Wi-Fi password, device token, CA | device NVS (`smartpager` namespace) | see below |

- The server refuses to start without an admin token of at least 24 characters. The optional shared ingest token must also be at least 24 characters and differ from it.
- **The admin is local-only.** Requests that arrive through the public proxy (they carry `X-Forwarded-For`) are refused on every admin endpoint, `/admin`, `/docs` and `/openapi.json`, and the admin token can't create messages that way. Caddy additionally routes only device, ingest and webhook paths ([GUIDE.md](GUIDE.md)).
- **Audit log.** Admin and CLI actions (device and token creation/rotation/revocation, read/ack/delete, firmware downloads) are recorded with actor and client IP, never message contents. Retained for max(retention, 90) days. View it with `make audit` or in the console.
- No credentials have defaults, in the server or the firmware. The setup access point gets a new random 10-character WPA2 password every time setup mode starts, and it is shown only on the device screen.
- Token comparisons are constant-time. Failed authentications are rate-limited per client IP. Normal traffic is rate-limited per credential (`SMARTPAGER_RATE_LIMIT_PER_MIN`).

## Transport

- **Server binding.** The server binds to `127.0.0.1` by default, including the published port in Docker Compose. Exposing it on the LAN is an explicit choice ([GUIDE.md](GUIDE.md), [SETUP.md](SETUP.md#testing-with-a-pager-on-the-home-network-plain-http)).
- **Plain HTTP** is meant for isolated, trusted development networks only. The device token and message contents travel unencrypted.
- **HTTPS.** The firmware always verifies the server certificate chain and hostname (mbedTLS). It trusts the built-in Let's Encrypt roots (ISRG Root X1, X2 and the Generation Y roots YE and YR), or a custom root CA provisioned in setup instead. There is no "insecure" mode.
- **Public deployment.** Caddy terminates TLS with Let's Encrypt, sets HSTS, caps bodies at 64 KB, and passes the real client IP only to the app (uvicorn trusts `X-Forwarded-For` solely from the Caddy container's fixed address).
- **Webhook URL tokens.** `?token=` is accepted only on `/webhooks/*` and only for ingest tokens, because URLs can end up in logs.

## Input handling

- Request bodies are capped at 64 KiB, including chunked uploads, by an ASGI middleware.
- Field lengths are capped: sender 120, subject 200, body 4000, metadata 2 KiB, device status 1 KiB, and 50 events per request.
- Unknown fields are rejected on the core API (`extra="forbid"`). C0 control characters are stripped. Timestamps without a timezone are rejected.
- The device treats the server as untrusted too. Each JSON page is filtered and fully validated before it is applied, response size is capped, every string is truncated safely, and emoji and malformed UTF-8 are removed before rendering.
- The admin console escapes all message content before inserting it into the page and is served with a strict CSP (`frame-ancestors 'none'`). The admin token stays in `sessionStorage` for that tab only.

## Logging and retention

- The server does not log message contents. Access logs are off (`--no-access-log`). Errors are logged with method and path only, and the IMAP connector logs counts only.
- The firmware logs no message text, tokens or passwords. It logs cursor numbers and counts.
- Retention: messages older than `SMARTPAGER_RETENTION_DAYS` (default 30) and expired messages become content-free tombstones. Tombstones and device-event records are hard-deleted after `SMARTPAGER_TOMBSTONE_DAYS` (default 7).

## Firmware updates (OTA)

- Updates are signed offline with the owner's ECDSA P-256 key (`secrets/ota_signing_key.pem`, git-ignored, mode 600). The server never sees the private key.
- The pager verifies the signature over `smartpager-ota|version|size|sha256` with the public key compiled into its firmware. It accepts only newer versions (no replayed downgrades), checks the image's SHA-256 before switching the boot partition, and installs only when the user holds KEY on *Settings → Update*.
- A compromised server can withhold updates but can't push firmware.
- If the signing key is lost, pagers built with it can only be updated over USB.

## Data at rest on the pager (known limitation)

- The last 40 messages (bodies up to 1 KB) are stored **unencrypted** in the LittleFS data partition.
- The Wi-Fi password and device token are stored **unencrypted** in NVS.
- Anyone with physical access and a USB cable can read them with `esptool read-flash`.

Fully protecting this needs ESP32-S3 **flash encryption** plus **NVS encryption**. Both burn eFuses irreversibly and change the flashing workflow for good, so this project does not enable them. If the pager will show sensitive content in an untrusted place:
- enable flash encryption in release mode yourself, following Espressif's guide, or
- send only short previews from the server: `max_body` is device-requested, and the IMAP connector sends only a 400-character preview.

If a device is lost, rotate its token immediately (`smartpager-admin device-rotate <id>`).

## Reporting issues

Open an issue in this repository. Do not include tokens or message contents.
