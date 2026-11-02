# REST API (v1)

Base URL: `http://127.0.0.1:8080` for local development. Every `/api/v1` request needs `Authorization: Bearer <token>`. Interactive OpenAPI docs are at `/docs`.

| Credential | Where it comes from | Can do |
|---|---|---|
| **Ingest token** | named tokens (`POST /tokens`, `make ingest-token`) or the optional `SMARTPAGER_INGEST_TOKEN` | `POST /messages`, `POST /webhooks/*` only |
| **Admin token** | `SMARTPAGER_ADMIN_TOKEN` | everything except the device endpoints |
| **Device token** | issued per device (`smartpager-admin device-create`). Only its SHA-256 is stored | `GET /devices/{own id}/sync`, `POST /devices/{own id}/events` |

Admin endpoints, `/admin`, `/docs` and `/openapi.json` answer **404** to requests that came through the public proxy (see [DEPLOY_PUBLIC.md](DEPLOY_PUBLIC.md)).

Errors use FastAPI's shape: `{"detail": ...}`. The status codes are 401 (missing or bad token), 404, 409 (device exists), 413 (body > 64 KiB), 422 (validation), 429 (rate limited, with `Retry-After`) and 500 (no internal details are returned).

## Messages

### `POST /api/v1/messages` (ingest or admin)

```json
{
  "source": "monitoring",            // manual|webhook|email|imessage|system|monitoring (default manual)
  "external_id": "alert-4711",       // optional; (source, external_id) is unique → idempotent
  "sender": "Grafana",               // ≤ 120 chars
  "subject": "CPU high",             // ≤ 200 chars
  "body": "node-3 at 97%",           // ≤ 4000 chars, UTF-8; control chars except \n\t are stripped
  "priority": "high",                // low|normal|high|critical (default normal)
  "created_at": "2026-11-02T14:00:00Z",   // optional, must carry a timezone
  "expires_at": "2026-11-03T14:00:00Z",   // optional; expired messages turn into tombstones
  "metadata": {"k": "v"}             // optional, ≤ 2 KiB JSON
}
```

Returns **201** `{"message": {...}, "duplicate": false}`. If the message already exists it returns **200** with the original message and `"duplicate": true`. Without `external_id`, an `Idempotency-Key` header gives the same retry safety.

### `GET /api/v1/messages` (admin)
Query parameters: `source`, `unread=true`, `include_deleted=true`, `limit` (1–200, default 50) and `before_id`. Results are ordered newest first. When there may be more, use `next_before_id` for the next page.

### `GET /api/v1/messages/{id}` · `POST .../{id}/read` · `POST .../{id}/ack` · `DELETE .../{id}` (admin)
- `read` and `ack` are idempotent. The first timestamp is kept and repeating the call creates no new change. `ack` implies `read`.
- `DELETE` turns the message into a **tombstone**: its content is wiped and devices are told to remove it. Tombstones are hard-deleted after `SMARTPAGER_TOMBSTONE_DAYS`.

Message states are tracked separately:

| Field | Set when |
|---|---|
| `delivered_at` | a device confirmed (`ack_cursor`) a sync cursor that covers the message, after saving it to flash |
| `read_at` | the user opened it on the device, or an admin marked it read |
| `acknowledged_at` | the user explicitly acknowledged it (long-press on the device), or an admin did |

## Webhooks (ingest)

Authenticate with `Authorization: Bearer <ingest token>`. For senders that can't set headers, use `?token=<ingest token>`, which is accepted only here and never for the admin token.

- `POST /api/v1/webhooks/generic`: `{"title", "message", "sender", "priority", "id"}`. Extra fields are ignored and long fields are clipped.
- `POST /api/v1/webhooks/alertmanager`: Prometheus Alertmanager v4 payloads. `severity=critical|page` maps to priority critical, other firing alerts to high, and resolved alerts to low. Re-deliveries are deduplicated.

## Devices (admin)

- `GET /api/v1/devices`: id, name, last_seen_at, last_ip, firmware, acked_cursor, and the last status report (rssi, heap, uptime, …).
- `POST /api/v1/devices` `{"id": "pager-2", "name": "Desk"}` → `{"device": {...}, "token": "spd_…"}`. The token is shown only in this response.
- `POST /api/v1/devices/{id}/token` rotates the token. The old one stops working immediately.

## Integration tokens and audit (admin)

- `GET /api/v1/tokens`: name, created, last used, revoked. Hashes and secrets are never returned.
- `POST /api/v1/tokens` `{"name": "grafana"}` → `{"token_info": {...}, "token": "spi_…"}`. The token is shown only in this response.
- `DELETE /api/v1/tokens/{id}` revokes it immediately. The name can be reused afterwards.
- `GET /api/v1/audit?limit=100`: `{id, at, actor, action, target, ip}`, newest first.

Messages carry `origin`: `admin`, `ingest` (the shared token) or `token:<name>`.

## Firmware (device token)

- `GET /api/v1/devices/{id}/firmware` → `{"version", "size", "sha256", "signature"}`, or 404 when nothing is published.
- `GET /api/v1/devices/{id}/firmware/image` → the signed application image.

`signature` is a base64 DER ECDSA P-256 signature over `smartpager-ota|<version>|<size>|<sha256>`. The device verifies it; the server only hosts the files (`make ota-publish`).

## Device sync protocol (schema 1)

### `GET /api/v1/devices/{id}/sync`

| Param | Default | Meaning |
|---|---|---|
| `cursor` | 0 | last cursor the device has **persisted** |
| `epoch` | – | epoch returned by the previous sync |
| `limit` | 10 (1–50) | max items per page |
| `max_body` | 1024 (64–4096) | body is truncated to this many **UTF-8 bytes** without splitting a character; `trunc: true` marks it |
| `wait` | 0 (0–60, capped by `SMARTPAGER_MAX_LONG_POLL_S`) | long-poll: if nothing is newer than `cursor`, hold the request until a change is committed or the time runs out |
| `fw` | – | firmware version, recorded for diagnostics |

```json
{
  "schema": 1, "epoch": "afa07f41", "server_time": 1793628779,
  "reset": false, "cursor": 7, "has_more": false, "unread": 2,
  "items": [
    {"id": 1, "seq": 6, "deleted": false, "src": "monitoring", "from": "Grafana", "subj": "Záloha",
     "body": "…", "trunc": false, "pri": 2, "ts": 1793628779, "read": 0, "ack": 0},
    {"id": 2, "seq": 7, "deleted": true, "src": "", "from": "", "subj": "", "body": "", "trunc": false,
     "pri": 1, "ts": 0, "read": 0, "ack": 0}
  ]
}
```

Times are Unix seconds UTC, and `0` means unset. `pri` runs from 0 (low) to 3 (critical).

**How the cursor works.** Every change to a message (create, read, ack, delete) gets the next value of a global sequence (`seq`) inside the same SQLite write transaction. Write transactions are serialized, so a change with a lower `seq` is always committed before one with a higher `seq`. A device that saw everything up to cursor N can therefore never miss an older change that commits later. This holds even when messages arrive in the middle of a multi-page sync. Each message appears once, at its latest `seq`.

**When the server sends `reset: true`.** The device must drop its cache and continue from the returned cursor. This happens when:
- the epoch differs (database recreated or restored),
- the cursor is older than the purge horizon (tombstones it would need were hard-deleted), or
- the cursor is ahead of the server (backup restored).

A reset response, and any sync from cursor 0, contains only live messages and no tombstones.

**Device algorithm:**
1. GET sync. Validate the whole page.
2. Apply it to the cache.
3. Save the cache together with the new cursor and epoch.
4. Only then POST `ack_cursor`.
5. Repeat while `has_more`.

### `POST /api/v1/devices/{id}/events`

```json
{
  "ack_cursor": 7,
  "events": [
    {"id": "9f2c41aa0b7d13e4", "type": "read", "message_id": 1, "at": 1793628800},
    {"id": "41aa0b7d13e49f2c", "type": "ack",  "message_id": 1}
  ],
  "status": {"fw": "0.1.0", "rssi": -58, "heap": 182000, "uptime": 3600}
}
```

→ `{"accepted": [...], "duplicates": [...], "unknown_messages": [...]}`

- Event ids are random 64-bit values generated by the device. `(device, event id)` is unique, so retrying after a lost response is safe. Every id listed in any of the three arrays is final, and the device drops it from its queue.
- An `at` in the future is clamped to server time. If `at` is missing (the device clock wasn't set yet), server time is used.
- An `ack_cursor` only moves forward and is ignored if it is beyond the server's current sequence.
- Up to 50 events per request, and `status` must be ≤ 1 KiB.

## Other

- `GET /health` (no auth) → `{"status": "ok", "version": "0.1.0", "seq": 42}`
- `GET /admin`: browser console (it asks for the admin token, which is kept in `sessionStorage` only)
