# Development setup

For working on SmartPager itself. To **run** it, follow [GUIDE.md](GUIDE.md).

## Server

Native, with Python ≥ 3.12:

```bash
make install            # server/.venv with dependencies
make test               # backend tests
make env                # .env with random tokens (never overwrites an existing one)
make run                # uvicorn on SMARTPAGER_BIND:SMARTPAGER_PORT (default 127.0.0.1:8080)
```

In Docker, without the HTTPS front end:

```bash
make up                 # build, start, wait for the health check
make device ID=pager-1
make logs
```

Useful during development:

| Command | What it does |
|---|---|
| `make backup` / `make import-db DB=path.db` | snapshot the container database / replace it (e.g. move from a native run) |
| `make audit` | recent admin and security events |
| `docker compose exec smartpager smartpager-admin --help` | all CLI commands (devices, ingest tokens, audit, purge) |
| <http://127.0.0.1:8080/docs> | interactive API docs (local port only) |
| `scripts/demo.sh` | the whole device protocol with curl: sync, inject, long poll, read/ack, retried events |

**Container hardening:** non-root user (uid 10001), read-only root filesystem, all Linux capabilities dropped, and `no-new-privileges`.

**Docker Desktop on macOS** NATs client addresses, so `last_ip` and the audit log show `192.168.65.1`.

## Firmware

```bash
make fw-test            # host unit tests (message store, sync parsing, OTA manifest, buttons, …)
make fw-build           # compile for the T-Display-S3
make fw-sim             # render the UI with mock messages into docs/screenshots (no hardware)
make fw-flash           # flash over USB (PORT_DEV=/dev/cu.usbmodem…)
make fw-monitor         # serial console
make fw-diag            # flash the hardware test image (colour bars, buttons, touch)
```

`make ota-keygen` must have been run once: the firmware embeds your OTA public key. Without it the build still works, but *Settings → Update* shows "Not configured".

## Testing with a pager on the home network (plain HTTP)

Quick to set up, but **not encrypted**: messages and the device token cross the network readable by anyone who can capture traffic. Use it on a trusted network for testing only.

1. Set `SMARTPAGER_BIND=0.0.0.0` in `.env`, run `make up`, and allow the port in the firewall.
2. Provision the pager with `http://<computer-LAN-IP>:8080` ([GUIDE.md §6.3](GUIDE.md#63-enter-the-settings)).

When you move to HTTPS ([GUIDE.md §5](GUIDE.md#5-start-it-and-obtain-the-certificate)):
- set `SMARTPAGER_BIND=127.0.0.1` again,
- rotate the tokens (`make rotate-secrets`, `make rotate ID=…`), and
- re-provision the pager with the `https://` URL.

## Pager setup reference

- **Setup mode** starts automatically when the pager has no settings. To enter it later:
  - *Settings → Setup mode* (hold KEY), or
  - hold **KEY** (GPIO14) while pressing **RST**.

  Don't hold BOOT for this: BOOT at reset starts the ROM bootloader.
- **Portal fields:**
  - Wi-Fi, server URL, device ID, device token
  - optional custom root CA (only for a private CA; Let's Encrypt is built in)
  - POSIX time zone (default Europe/Prague `CET-1CEST,M3.5.0,M10.5.0/3`)
  - polling interval, brightness, instant delivery (long polling), 180° rotation
- **Secret fields** (Wi-Fi password, token) are never shown back. Leave them blank to keep the stored values.
- ***Factory reset settings*** erases only SmartPager's settings and cache. The firmware stays.
- **USB provisioning** with `tools/provision.py` accepts `--url --device --ssid --tz --restart`. Secrets come from the `SMARTPAGER_DEVICE_TOKEN` and `WIFI_PASSWORD` environment variables.
