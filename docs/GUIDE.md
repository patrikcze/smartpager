# Secure setup guide

This guide takes you from an empty machine to pagers that receive messages **encrypted over HTTPS** from anywhere. Follow it top to bottom the first time.

- [1. How the pieces fit](#1-how-the-pieces-fit)
- [2. What you need](#2-what-you-need)
- [3. Prepare the server](#3-prepare-the-server)
- [4. Start it with HTTPS](#4-start-it-with-https)
- [5. Connect a pager](#5-connect-a-pager)
- [6. Send messages securely](#6-send-messages-securely)
- [7. Use the admin console safely](#7-use-the-admin-console-safely)
- [8. Firmware updates](#8-firmware-updates)
- [9. Day-to-day operations](#9-day-to-day-operations)
- [10. Security checklist](#10-security-checklist)
- [Appendix: testing on a home network without HTTPS](#appendix-testing-on-a-home-network-without-https)

---

## 1. How the pieces fit

```
  pager(s) ─────────┐  HTTPS (TLS 1.2+, certificate verified on the pager)
  scripts, webhooks ┼──────────────▶  Caddy :443  ──▶  SmartPager server  ──▶  SQLite
  e-mail connector ─┘  internet / LAN   Let's Encrypt     (Docker network,      (Docker volume)
                                         auto-renewing     not reachable from
                                                            outside)
  you (admin) ──▶ http://127.0.0.1:8080/admin   (only on the server machine, or via SSH/VPN)
```

| Credential | Who holds it | What it can do |
|---|---|---|
| **Device token** (`spd_…`) | one per pager | sync its own inbox, report read/ack, download signed firmware |
| **Ingest token** (`spi_…`) | one per integration (script, Grafana, …) | create messages only |
| **Admin token** | you, in `.env` | everything, but only on the local port |
| **OTA signing key** | you, in `secrets/` | sign firmware updates. Never leaves your computer |

What is encrypted in this setup is summarised in [SECURITY.md](SECURITY.md#what-is-encrypted).

## 2. What you need

- A computer that stays on: a small Linux box, NAS or Mac, with Docker and this repository.
- A **domain name** pointing to your public IP, e.g. `pager.example.com`. Dynamic DNS works.
- Ports **80 and 443** forwarded from your router to that computer. Let's Encrypt uses port 80 to issue the certificate.
- For flashing pagers: USB and [PlatformIO](https://platformio.org/install/cli).

## 3. Prepare the server

```bash
make env
```

This creates `.env` with random tokens. The file is readable only by you and is never committed. Edit it:

```bash
SMARTPAGER_DOMAIN=pager.example.com
ACME_EMAIL=you@example.com
SMARTPAGER_BIND=127.0.0.1        # keep the unencrypted port on this machine only
```

Then create your firmware-signing key once and **back it up** (password manager or an encrypted drive):

```bash
make ota-keygen
```

## 4. Start it with HTTPS

```bash
make up-public
```

```bash
make logs-caddy
```

Wait for `certificate obtained successfully`, then check from outside your network, e.g. on a phone using mobile data:

```bash
curl https://pager.example.com/health
```

It should return `{"status":"ok",…}`. These requests should return `Not Found` from the internet:

- `https://pager.example.com/admin`
- `https://pager.example.com/docs`
- `https://pager.example.com/api/v1/messages`

The server comes back by itself after a reboot as long as Docker starts at login (Docker Desktop → Settings → General → *Start Docker Desktop when you sign in*).

> **Pagers at home use the public name too.** If your router can't route your own public IP back inside (no "NAT loopback"), add a local DNS entry for `pager.example.com` pointing to the server's LAN address. The certificate stays valid either way.

## 5. Connect a pager

### 5.1 Register it on the server

```bash
make device ID=pager-1
```

This prints `token: spd_…` **once**. Only a hash is stored, so copy it now. If you lose it, run `make rotate ID=pager-1` to get a new one.

### 5.2 Flash the firmware (first time only, over USB)

```bash
make fw-flash
```

The firmware includes the Let's Encrypt root certificates and your OTA public key, so later updates come over Wi-Fi ([§8](#8-firmware-updates)). Board-specific notes are in [FLASHING.md](FLASHING.md).

### 5.3 Enter the settings

A pager without settings starts **setup mode**. Its screen shows a Wi-Fi network `SmartPager-XXXX`, a one-time password, and `http://192.168.4.1`. To re-enter setup mode later: *Settings → Setup mode* (hold KEY), or hold **KEY** while pressing **RST**.

**Option A: phone or laptop (no tools needed)**

1. Join `SmartPager-XXXX` with the password shown on the pager.
2. Open `http://192.168.4.1` and fill in:
   - **Wi-Fi network and password**
   - **Server URL:** `https://pager.example.com`. It must start with `https://`.
   - **Device ID:** `pager-1`
   - **Device token:** the `spd_…` value
   - leave *Custom root CA* empty: Let's Encrypt is built in
3. Tap **Save and restart**.

**Option B: from the computer over USB (no typing of long tokens)**

While the setup screen is shown:

```bash
SMARTPAGER_DEVICE_TOKEN=spd_… WIFI_PASSWORD='…' \
  uv run --no-project --with pyserial python tools/provision.py --port /dev/cu.usbmodem1101 \
  --url https://pager.example.com --device pager-1 --ssid "MyWiFi" --restart
```

Secrets are passed as environment variables, so they don't end up in your shell history. `pio device list` shows the port name.

### 5.4 Check the connection

Within a few seconds the pager footer reads *Synced just now*, and in the console (§7) the device shows *seen just now* with firmware `0.2.1`. Send a test message (§6). It should appear on the pager in about 1–2 s.

**More pagers:** repeat §5 with `ID=pager-2` and so on. All pagers share one inbox: reading or acknowledging on one marks it on all.

## 6. Send messages securely

Give **each** script or service its own ingest token:

```bash
make ingest-token NAME=backup-script
```

Then send over HTTPS:

```bash
curl -X POST https://pager.example.com/api/v1/messages \
  -H "Authorization: Bearer spi_…" -H "Content-Type: application/json" \
  -d '{"source":"system","sender":"NAS","subject":"Backup completed","priority":"normal","external_id":"backup-2026-11-02"}'
```

- Use a unique `external_id` per event, so a retried request never pages you twice.
- Services that can't set headers can use `https://pager.example.com/api/v1/webhooks/generic?token=spi_…`. Only ingest tokens work in URLs.
- If a token leaks, revoke just that one: in the console, or with `docker compose exec smartpager smartpager-admin ingest-revoke backup-script`.
- Request formats, Alertmanager and e-mail are covered in [API.md](API.md) and [INTEGRATIONS.md](INTEGRATIONS.md).

## 7. Use the admin console safely

The console and every admin API are **only** on the local port. They return 404 through the public address, by design.

- **On the server:** <http://127.0.0.1:8080/admin>. Paste the admin token (`grep ADMIN .env`). It's kept only in that browser tab; *Lock* forgets it.
- **From elsewhere:** open an SSH tunnel, then use the same local URL:

  ```bash
  ssh -L 8080:127.0.0.1:8080 you@server
  ```

  A VPN such as Tailscale also works. Never set `SMARTPAGER_BIND=0.0.0.0` on an internet-facing machine.

## 8. Firmware updates

Releases are signed with your key. The pager verifies the signature, refuses older versions, and checks the image hash before installing. It installs only when you ask it to.

```bash
# 1. bump SMARTPAGER_VERSION in firmware/src/app_state.h (e.g. 0.2.1 → 0.2.2)
make ota-publish
```

On the pager: *Settings → Update* shows `0.2.2 ready` (it checks every 6 h and after boot). Hold **KEY** to install. It restarts and keeps its inbox.

There is no automatic rollback. If a release doesn't boot, reflash it with `make fw-flash`.

## 9. Day-to-day operations

| Task | Command |
|---|---|
| Health / status | `make health`, `make ps`, `make logs` |
| Backup the database (keep copies off the machine) | `make backup` → `backups/` |
| Who did what | `make audit`, or *Audit log* in the console |
| New admin token (e.g. after sharing a screen) | `make rotate-secrets` |
| Lost or stolen pager | `make rotate ID=pager-1` immediately (old token stops at once) |
| Remove a pager | `docker compose exec smartpager smartpager-admin device-delete pager-1` |
| Stop everything | `make down` (data is kept) |

## 10. Security checklist

- [ ] Pagers and integrations use `https://pager.example.com`, never `http://`.
- [ ] `SMARTPAGER_BIND=127.0.0.1` in `.env`, so the unencrypted port isn't reachable from the network.
- [ ] One ingest token per integration. The shared `SMARTPAGER_INGEST_TOKEN` in `.env` is empty once nothing uses it.
- [ ] Every token that ever travelled over plain HTTP has been rotated (`make rotate-secrets`, `make rotate ID=…`).
- [ ] `.env`, `secrets/ota_signing_key.pem` and `backups/` are backed up somewhere safe and never shared or committed.
- [ ] Docker starts at login, and `make health` works after a reboot.
- [ ] You know what isn't protected: messages are readable on the server and on the pager's flash ([SECURITY.md](SECURITY.md#what-is-encrypted)).

---

## Appendix: testing on a home network without HTTPS

For a quick first test you can skip the domain and run plain HTTP on your LAN:

1. Set `SMARTPAGER_BIND=0.0.0.0` in `.env` and run `make up`.
2. Use `http://<server-LAN-IP>:8080` as the pager's server URL.

**This is not encrypted.** Anyone on that network who can capture traffic can read messages and the device token. Use it only on a network you trust, and only for testing. When you switch to §4:
- set `SMARTPAGER_BIND=127.0.0.1` again,
- rotate the tokens, and
- point the pagers at the `https://` URL.
