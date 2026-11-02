# Architecture

All external traffic enters through **Caddy** (HTTPS, Let's Encrypt, path allow-list; `deploy/Caddyfile`), which forwards to the server on the internal Docker network. The admin console is reached only on the server's local port. The diagram shows the logical flows behind Caddy.

```
 ┌───────────────┐  POST /messages        ┌──────────────────────────────┐   GET  /devices/{id}/sync (long poll)  ┌─────────────────────┐
 │ curl / scripts │ ─────────────────────▶│  FastAPI  (uvicorn, 1 proc)   │◀──────────────────────────────────────│ ESP32-S3 SmartPager │
 │ webhooks       │  POST /webhooks/*     │  ├ auth: admin/ingest/device  │   POST /devices/{id}/events            │  net task (core 0)  │
 │ IMAP connector │                       │  ├ ChangeNotifier (long poll) │──────────────────────────────────────▶│  UI task  (core 1)  │
 └───────────────┘                        │  └ SQLite (WAL)               │                                        └─────────────────────┘
 ┌───────────────┐  admin API            │     messages · devices ·      │
 │ /admin console │ ─────────────────────▶│     device_events · meta     │
 └───────────────┘                        └──────────────────────────────┘
```

## Backend (`server/app`)

| Module | Responsibility |
|---|---|
| `config.py` | Environment settings. Refuses to start without two distinct secrets of at least 24 characters |
| `database/db.py` | Connections, `BEGIN IMMEDIATE` write transactions, the global change sequence, numbered migrations (`PRAGMA user_version`) |
| `services/messages.py` | All persistence logic: create (idempotent), read/ack (monotonic), tombstones, retention purge, sync paging, device events, device tokens (SHA-256) |
| `services/notifier.py` | `asyncio.Event` swap used to wake long-poll requests. The token is taken before the query, so no wake-up is lost |
| `api/routes.py`, `api/deps.py` | HTTP layer, bearer auth, bounded token-bucket rate limiter |
| `integrations/` | Webhook adapters (in-process) and the IMAP connector (separate process) |
| `main.py` | App factory, request-size limit middleware, security headers, hourly retention task |

There is no ORM and no message queue. SQLite in WAL mode is the single source of truth. A device that has been offline for a week simply resumes from its cursor.

### Why long polling

With 15 s polling the average delivery delay is about 7.5 s. A device that keeps a `wait=20` sync request open gets new messages as soon as they are committed. Measured on the dev server: the message was injected 1.0 s after the long poll started, and the request returned at **1.02 s**, i.e. about 20 ms after the commit.

The cost is one idle HTTP connection per device. Reliability is unchanged:
- every long poll is an ordinary sync request with the same cursor semantics, and
- the firmware falls back to plain polling after any error, or when long polling is turned off in Settings.

MQTT would add a broker and buy nothing for a single-user deployment.

## Firmware (`firmware/src`)

| Directory | Contents | Host-tested |
|---|---|---|
| `messaging/` | `MessageStore` (bounded: 40 messages, 32 pending events, sorted newest first, crash-safe binary snapshot with CRC32), sync JSON validation, events payload, `Backoff`, `ButtonFsm` | ✔ |
| `ui/` | LVGL screens (Home, Inbox, Detail, Settings, new-message banner), formatting helpers, generated Inter fonts with Czech glyphs | format ✔, screens rendered on the host |
| `hardware/` | ST7789 i80 + LVGL driver, backlight pulses, buttons, CST816 touch | device only |
| `storage/` | NVS config (Preferences), LittleFS cache with temp-file + rename | device only |
| `network/` | network task (Wi-Fi, NTP, sync, events, cache persistence), setup portal | device only |
| `sim/` | host renderer: real UI code + mock data → PNG | – |
| `diag/` | hardware verification image | – |

### Tasks and data flow

- **UI task (Arduino `loop`, core 1).**
  - Runs `lv_timer_handler()` and polls the buttons every 5 ms.
  - Copies the shared `DeviceStatus` under the mutex.
  - Calls `uiTick()`, which redraws only when the store's `version()`, the minute or the status changes.
  - It never touches the network.
- **Network task (core 0, 12 KB stack).** Runs a state machine: Wi-Fi up → sync (long poll) → upload events → wait.
  - Network errors back off exponentially with jitter, from 2 s up to 30 s (2 requests/min while the server is down). An authentication error waits 5 min.
  - Signed firmware updates are checked after the first sync and every 6 h, and installed only on user request (see [GUIDE.md](GUIDE.md#8-firmware-updates)).
  - New messages are announced to the UI through a FreeRTOS queue, which triggers the banner and wakes the backlight.
  - User actions (read, ack, "sync now") call `net::kick()` to wake the task.
- **Shared state:** `g_store` and `g_status` behind one mutex. Flash writes happen **outside** the lock: the store is serialized under the lock, then written without it.

### Reliability properties

- **Bounded memory.** Messages live in a fixed PSRAM array (40 × 1.3 KB). HTTP responses are capped at 48 KB. JSON is parsed with a field filter and a nesting limit. Every string is copied with UTF-8-safe truncation.
- **Untrusted input.** A page is validated in full before any item is applied. A bad type, wrong schema, truncated JSON or oversized string rejects the page, and the cursor does not move.
- **Exactly-once effects.**
  - Messages are upserted by id, so a duplicate delivery updates rather than duplicates.
  - Read/ack only move forward, so a stale server copy can't undo a local action that is still pending.
  - Events carry random ids, so retries are deduplicated on the server.
- **Crash consistency.** The messages, cursor, epoch and pending events live in **one** file. The file is written to `inbox.tmp` and then renamed. If power is lost between the delete and the rename, the complete temp file is recovered on boot. A corrupt file (CRC, size or layout mismatch) is discarded and triggers a full resync. The cursor is acknowledged to the server only after the file is saved.
- **Flash wear.** At most one cache write per 5 s, and only after something changed. The write count is reported to the server in the device status.
- **Watchdog.** The task watchdog is set to 45 s, longer than the longest blocking call (20 s long poll + 10 s timeout), and both tasks are subscribed. Setup mode unsubscribes because it waits for a human.
- **Time.** NTP (`pool.ntp.org`, `time.cloudflare.com`) with a POSIX TZ string. Until NTP answers, the server's `server_time` sets the clock, so ages and the clock still work behind firewalls.

### UI and controls

320×170 landscape. Dark background `#0A0B10`, 14 px rounded cards, a lavender accent, one colour per source, and Inter at 12/14/16/22 px plus 52 px clock digits. Screenshots are in [`docs/screenshots`](screenshots), rendered by `pio run -e sim` from the same code that runs on the device.

| | BOOT short | KEY short | KEY hold | BOOT hold |
|---|---|---|---|---|
| Home | Settings | Inbox | Inbox (first unread) | – |
| Inbox | previous | next | open (marks read) | Home |
| Detail | scroll up | scroll down | **acknowledge** | Inbox |
| Settings | previous row | next row | change / run row | Home |

- **Touch (CST816 only):** tap a card or row to open it, drag to scroll, swipe right to go back, swipe left on Home to open the Inbox.
- **Dimming:** after 60 s without input the screen dims to 8 %. The next press only wakes it, and a new message also wakes it.
