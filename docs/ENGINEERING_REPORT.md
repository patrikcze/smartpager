# Engineering report

Development and verification log from the initial build (2026-11-02). It records what was tested, how, and what remains unverified. For using SmartPager, see [GUIDE.md](GUIDE.md).

## Build and test record

### What was implemented

- **Hardware identification** (read-only). See [docs/HARDWARE.md](HARDWARE.md).
- **Backend:**
  - SQLite with WAL and versioned migrations.
  - Separate admin, ingest and per-device credentials (device tokens are stored hashed).
  - Idempotent ingestion keyed on `source+external_id` or an `Idempotency-Key` header.
  - Delivered, read and acknowledged are recorded as separate states.
  - Tombstones, retention purge, and a cursor-based incremental sync with epochs, resets, bounded pages and **long polling**.
  - Device events (read/ack/cursor) applied exactly once.
  - Rate limiting, a 64 KiB request limit, security headers, and no logging of message contents.
  - Admin web console.
  - Generic and Alertmanager webhooks.
  - Read-only IMAP connector.
  - Dockerfile and Compose (non-root, read-only filesystem, bound to localhost).
- **Firmware:**
  - ST7789 i80 display driver following LILYGO's init path.
  - LVGL 8.3 UI with Home, Inbox, Detail and Settings screens, a new-message banner, idle dimming, and Czech-capable fonts.
  - Button state machine (debounce, short/long press). CST816 touch detected at runtime.
  - Setup portal on a SoftAP with a random WPA2 password, storing config in NVS.
  - NTP time with a TZ setting, plus server time as a fallback.
  - Network task with long poll, backoff with jitter, and event upload.
  - Bounded PSRAM inbox persisted crash-safely on LittleFS.
  - Task watchdog.
  - A `diag` image for verifying the display, touch and buttons.

### What was actually tested

| Test | Result |
|---|---|
| Backend: `pytest` (auth and scoping, unauthorized access, duplicates incl. concurrent ingestion, read/ack, cursor pagination, messages arriving mid-sync, tombstones, resets after epoch change, purge or cursor ahead, UTF-8 truncation, long poll, malformed and oversized input, idempotent events, restart persistence, webhooks, IMAP MIME mapping) | **55 passed** |
| Firmware host tests: `pio test -e native` (store ordering, upserts, eviction, local-vs-server state, pending-event bounds, UTF-8 safety, emoji stripping, corruption detection, JSON validation incl. truncated/oversized/wrong-type input, events payload, backoff, button FSM, formatting) | **21 passed** |
| **Milestone 1**: real uvicorn server, message injected via the API and received through the device sync protocol (long poll returned ~20 ms after the commit), read/ack events and retry deduplication, admin view showing delivered/read/acked | **done** (`scripts/demo.sh`) |
| **Milestone 2**: `pio run -e tdisplays3` for the verified board | **compiles, 0 warnings in project sources** |
| UI with mock messages: real LVGL screens rendered on the host (`docs/screenshots/`) | **done**. Two layout bugs found this way and fixed |
| Admin console in a browser against the live server (unlock, send, list, device status) | **done** |
| **On the device (flashed with your authorization):** diag image: chip, 16 MB flash, 8189 KB PSRAM, **CST816 touch at 0x15** | **verified** |
| Device provisioning: USB pre-load (`tools/provision.py`) + Wi-Fi via the setup portal, joined Wi-Fi, authenticated, synced all existing messages | **verified** |
| **Milestone 3, end-to-end:** API → device delivery incl. flash save and cursor confirmation: **1.1 s, 2.0 s, 6.8 s** (RSSI −83 dBm) | **verified** |
| Cache survives firmware update and reset (`cache=loaded messages=8 cursor=13` at boot) | **verified** |
| Read/ack made with the device buttons reached the server (5 messages read, 4 acknowledged; delivered, read and acked recorded separately) | **verified** |
| Docker: `make build` / `make up`, container healthy, data imported, pager synced against it (delivery ~2 s), runs non-root with a read-only root filesystem | **verified** |
| Public-deployment hardening: Caddy routing (pager/ingest/webhook paths only; `/admin`, `/docs`, admin API → 404; long poll held through the proxy), admin token refused via proxy, 64 KB body cap, HSTS. Tested locally on `https://localhost` (Caddy internal CA); **not yet tested with a real domain and Let's Encrypt** | **verified locally** |
| Signed OTA on the device: tampered manifest rejected ("Bad signature", nothing downloaded); correctly signed 0.2.1 offered, installed by the user, pager restarted on 0.2.1 with its inbox intact | **verified** |
| Server outage 40 s: UI task and device kept running (no reboot, heap stable ~174–180 KB). Delivery after restart **52 s → 14.7 s** after capping backoff at 30 s | **verified** |

### Firmware memory (measured at build time)

| Image | Static RAM | Flash (of the 6.4 MB app slot) |
|---|---|---|
| `tdisplays3` | 53,764 B (16.4 %) | 1,209,601 B (18.5 %) |
| `diag` | 20,124 B (6.1 %) | 433,229 B (6.6 %) |

Runtime budget by design:
- inbox: 40 × 1,276 B ≈ 51 KB in PSRAM
- LVGL draw buffers: 2 × 21.8 KB internal DMA RAM
- HTTP responses: ≤ 48 KB

Measured at runtime on the device: **free internal heap 174–180 KB with 8 cached messages, 143 KB with 23** (LVGL's small per-row allocations use internal RAM; bounded by the 40-message cap), **free PSRAM 8.27 MB**. Both appear in *Settings → Memory* and in the device status sent to the server.

### Not yet verified (needs hardware or credentials)

1. **On the device, still unverified by me** (needs eyes or fingers on the board):
   - colours, orientation and backlight steps as seen on the panel
   - which physical button is BOOT and which is KEY
   - touch coordinate mapping
   - watchdog behaviour under a real hang
   - Wi-Fi loss/reconnect (only server outages were tested)
   - HTTPS against a real Let's Encrypt certificate on the device (the built-in roots are compiled in but untested against a live public server)
   - long-running memory behaviour: free heap went 180 → 174 → 170 KB over the first ~5 minutes of use. That is too short to tell a leak from LVGL settling. Watch `heap` in `/admin` over a day.
2. **IMAP** against a live mailbox, and HTTPS against a real certificate: no account or certificate was available.

### Known limitations

- **Touch:** only the CST816 controller is supported. Early Touch units with a CST328 run on buttons only.
- **Text:** fonts cover Latin, Latin-1 and Latin Extended-A. Emoji are stripped, and other scripts render as boxes.
- **OTA rollback:** none. A release that doesn't boot needs a USB reflash.
- **Data at rest:** messages and credentials on the device are not encrypted at rest. Flash encryption is deliberately not enabled because it is irreversible ([SECURITY.md](SECURITY.md)).
- **Single process:** one backend process. The rate limiter and long-poll notifier live in memory, which is fine for a single-user deployment.
- **Event latency:** a read/ack made while a long poll is open is uploaded when that poll returns (≤ 20 s).
- **iMessage:** not implemented (see [docs/INTEGRATIONS.md](INTEGRATIONS.md)).

### Next steps to deploy

1. Optionally back up the clock firmware, then flash `diag` and check colours, orientation, buttons and touch ([FLASHING.md](FLASHING.md)).
2. Run the server on the LAN (or behind HTTPS) and register `pager-1`.
3. Flash `tdisplays3`, join `SmartPager-XXXX`, and enter Wi-Fi, server URL, device ID and token.
4. Run `scripts/demo.sh` against the LAN URL and watch the message arrive. Acknowledge it on the device and confirm `acknowledged_at` in `/admin`.

### Dependency versions

PlatformIO `espressif32@6.5.0` (arduino-esp32 2.0.14, ESP-IDF 4.4), LVGL 8.3.11, ArduinoJson 7.2.1, Unity 2.6.1 (tests). Python 3.13 (≥ 3.12 supported), FastAPI 0.162, Pydantic 2.14, Uvicorn 0.54. Fonts: Inter 4.1 via lv_font_conv 1.5.2. Tools: esptool 5.6.0, PlatformIO Core 6.2.0.
