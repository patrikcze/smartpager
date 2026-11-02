# Security

## Credentials

| Secret | Stored | Notes |
|---|---|---|
| Admin token | server environment (`.env`, never committed) | full management API and the `/admin` console |
| Ingest token | server environment | create messages and webhooks only. Give this one to scripts and connectors |
| Device token | server: SHA-256 hash only. Device: NVS | one per device, rotatable (`POST /devices/{id}/token`). Only accepted for that device's own sync and events endpoints |
| Wi-Fi password, device token, CA | device NVS (`smartpager` namespace) | see below |

- The server refuses to start unless both server tokens are set, are at least 24 characters long, and differ from each other.
- No credentials have defaults, in the server or the firmware. The setup access point gets a new random 10-character WPA2 password every time setup mode starts, and it is shown only on the device screen.
- Token comparisons are constant-time. Failed authentications are rate-limited per client IP. Normal traffic is rate-limited per credential (`SMARTPAGER_RATE_LIMIT_PER_MIN`).

## Transport

- **Server binding.** The server binds to `127.0.0.1` by default, including the published port in Docker Compose. Exposing it on the LAN is an explicit choice ([SETUP.md](SETUP.md#2-make-the-server-reachable-from-the-pager)).
- **Plain HTTP** is meant for isolated, trusted development networks only. The device token and message contents travel unencrypted.
- **HTTPS.** The firmware checks the server certificate against the root CA PEM entered in setup (`WiFiClientSecure::setCACert`). An `https://` URL without a CA is refused. There is no "insecure" mode. Hostname checking is done by mbedTLS against the URL host.

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
