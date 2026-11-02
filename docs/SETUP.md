# Setup

## 1. Run the backend

### Native (Python ≥ 3.12)

```bash
cd server
uv venv --python 3.13 .venv && uv pip install -e '.[dev]'     # or: python3 -m venv .venv && .venv/bin/pip install -e '.[dev]'
cp ../.env.example ../.env    # then put two random secrets into it:
.venv/bin/python -m app.cli token    # run twice: SMARTPAGER_ADMIN_TOKEN and SMARTPAGER_INGEST_TOKEN
set -a; source ../.env; set +a
.venv/bin/uvicorn --factory app.main:create_app --host 127.0.0.1 --port 8080
```

Tests:

```bash
cd server && .venv/bin/pytest -q
```

### Docker (production)

```bash
make env        # .env with fresh random tokens; never overwrites an existing one
make up         # build image, start container, wait for the health check
make device ID=pager-1
```

| Target | What it does |
|---|---|
| `make logs` / `make ps` / `make health` | follow logs, container status, `/health` |
| `make backup` | consistent SQLite snapshot into `./backups/` (git-ignored, mode 600) |
| `make import-db DB=path.db` | replace the container database, e.g. to move from a native install |
| `make rotate ID=pager-1` | issue a new device token |
| `make down` | stop. The `smartpager-data` volume is kept |

- **Security:** the container runs as a non-root user (uid 10001) with a read-only root filesystem, no Linux capabilities, and `no-new-privileges`.
- **Restarts:** `restart: unless-stopped` brings it back after reboots, as long as Docker itself starts at login (Docker Desktop → Settings → General).
- **Docker Desktop on macOS:** client addresses are NATed, so `last_ip` in `/admin` shows `192.168.65.1` rather than the pager's real IP.

## 2. Make the server reachable from the pager

The default binding is **localhost only**, so the pager on Wi-Fi can't reach it. Pick one of these:

- **Trusted home LAN, plain HTTP** (development only). Set `SMARTPAGER_BIND=0.0.0.0` in `.env` (used by both `make run` and `make up`). Allow the port in the macOS firewall, then use `http://<mac-ip>:8080` as the server URL. Messages and the device token then cross your Wi-Fi unencrypted.
- **HTTPS (recommended, also from the internet).** Run `make up-public`, which uses the bundled Caddy with automatic Let's Encrypt. Then enter `https://pager.example.com` on the pager; the Let's Encrypt roots are built in. Full guide: [DEPLOY_PUBLIC.md](DEPLOY_PUBLIC.md).

## 3. Provision the pager

1. Register the device and copy the printed token:

   ```bash
   cd server && .venv/bin/python -m app.cli device-create pager-1 --name "Desk"
   ```

   You can also use the Devices panel at `http://127.0.0.1:8080/admin`.
2. Flash the firmware ([FLASHING.md](FLASHING.md)). Without configuration the pager starts **setup mode** and shows its access point name (`SmartPager-XXXX`), a random password, and `http://192.168.4.1`.
3. Join that Wi-Fi from a phone or laptop, open the page, and fill in:
   - Wi-Fi network and password
   - server URL, device ID and device token
   - optionally: a custom root CA (only for a private CA; Let's Encrypt works without it), time zone (POSIX TZ, default Europe/Prague `CET-1CEST,M3.5.0,M10.5.0/3`), polling interval, brightness, instant delivery (long polling), and 180° rotation
4. Click **Save and restart**. The pager joins your Wi-Fi and syncs.

**Shortcut over USB.** While the setup screen is shown you can pre-load the long values from your computer, so the phone form needs only Wi-Fi:

```bash
SMARTPAGER_DEVICE_TOKEN=spd_... tools/provision.py --port /dev/cu.usbmodem1101 --url http://192.168.1.10:8080 --device pager-1
```

`--ssid` and `WIFI_PASSWORD` can be passed the same way, together with `--restart`, to provision completely without the portal. Needs pyserial.

**Re-entering setup mode later:**
- Settings → *Setup mode* → hold KEY, or
- hold **KEY** (GPIO14) while pressing **RST**.

Don't hold BOOT for this: BOOT at reset starts the ROM bootloader.

The portal leaves secret fields empty. Leave them blank to keep the stored values. *Factory reset settings* erases only SmartPager's settings and cache. The firmware stays.

## 4. Send messages

```bash
curl -X POST http://127.0.0.1:8080/api/v1/messages \
  -H "Authorization: Bearer $SMARTPAGER_INGEST_TOKEN" -H 'Content-Type: application/json' \
  -d '{"sender":"Ops","subject":"Backup completed","body":"Nightly NAS backup OK","priority":"normal"}'
```

```bash
curl -H "Authorization: Bearer $SMARTPAGER_ADMIN_TOKEN" 'http://127.0.0.1:8080/api/v1/messages?limit=10'
```

`scripts/demo.sh` walks through the whole device protocol with curl: initial sync, inject, long-poll sync, read/ack events, retried events, and the admin view. Integrations are covered in [INTEGRATIONS.md](INTEGRATIONS.md).

## 5. Preview the UI without hardware

```bash
cd firmware
pio test -e native              # host unit tests
pio run -e sim && .pio/build/sim/program ../docs/screenshots
```
