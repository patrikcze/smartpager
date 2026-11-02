# Running SmartPager on the internet (personal use)

This setup is for **one owner** using SmartPager as an everyday pager from anywhere. It is not meant for hosting other people. The design:

```
 internet ──443──▶ Caddy (Let's Encrypt) ──▶ smartpager:8080      (docker network)
                      │  routes ONLY: device sync/events/firmware, POST /messages,
                      │               /webhooks/*, /health        everything else → 404
 this machine ──▶ http://127.0.0.1:8080/admin                    (admin: local only)
```

## Exposure

| Exposed publicly (through Caddy) | Never exposed |
|---|---|
| `GET /api/v1/devices/{id}/sync`, `POST …/events`, `GET …/firmware`, `GET …/firmware/image`: device token | `/admin`, `/docs`, `/openapi.json` |
| `POST /api/v1/messages`, `POST /api/v1/webhooks/*`: ingest tokens | every admin API (list, read/ack, delete, devices, tokens, audit) |
| `GET /health` | |

There are two independent layers:

1. **Caddy routes only the paths on the left.** Anything else gets a 404 before it reaches the app.
2. **The app refuses admin requests that came through the proxy** (they carry `X-Forwarded-For`), and also refuses the admin token for message creation through the proxy.

A mistake in either layer alone doesn't expose the admin. Opting out (`SMARTPAGER_ADMIN_VIA_PROXY=1`) is possible but not recommended.

> **Why not "admin only from my LAN IP"?** With Docker Desktop on macOS, every incoming connection, including ones from the internet, appears to come from `192.168.65.1`, a private address. An IP allow-list would silently let the whole internet in. Keeping admin off the public site entirely avoids that trap on every platform.

## 1. Prerequisites

- A domain name pointing at your public IP, e.g. `pager.example.com` (an A/AAAA record or dynamic DNS).
- Ports **80 and 443** forwarded from your router to this machine. Port 80 is needed for Let's Encrypt's HTTP challenge and redirects to HTTPS.
- Docker, and this repository with `.env` created by `make env`.

## 2. Configure

In `.env`:

```bash
SMARTPAGER_DOMAIN=pager.example.com
ACME_EMAIL=you@example.com
SMARTPAGER_BIND=127.0.0.1     # keep the plain-HTTP port local: the admin stays on this machine
```

## 3. Start

```bash
make up-public
```

```bash
make logs-caddy      # watch for "certificate obtained successfully"
```

```bash
curl https://pager.example.com/health
```

The certificate renews automatically. Caddy's state lives in the `caddy-data` volume.

## 4. Rotate everything that ever crossed plain HTTP

If you ran the server on the LAN over `http://` before, its tokens travelled unencrypted:

```bash
make rotate-secrets          # new admin + shared ingest token in .env (old file kept as .env.bak-*)
make rotate ID=pager-1       # new device token (shown once)
```

Then put the new device token and the `https://pager.example.com` URL on the pager. Either use setup mode, or this command while the pager is in setup mode:

```bash
SMARTPAGER_DEVICE_TOKEN=spd_… tools/provision.py --port /dev/cu.usbmodem1101 --url https://pager.example.com --device pager-1 --restart
```

No certificate needs to be pasted. The firmware ships with the Let's Encrypt roots: ISRG Root X1 and X2, plus the new Generation Y roots YE and YR.

## 5. One token per integration

Rather than handing the shared ingest token to every script and service:

```bash
make ingest-token NAME=grafana
```

```bash
make ingest-token NAME=backup-script
```

Each token can be revoked alone, from the console (*Integration tokens*) or with `docker compose exec smartpager smartpager-admin ingest-revoke grafana`. Every message records which credential created it (`origin`), and admin actions go into the audit log (`make audit`). Once nothing uses the shared `SMARTPAGER_INGEST_TOKEN`, you can empty it in `.env`.

For services that can't set an `Authorization` header, webhooks also accept the token in the URL:

```
https://pager.example.com/api/v1/webhooks/generic?token=spi_…
```

URLs end up in logs and history, so only **ingest** tokens are accepted that way, never the admin token. Use a dedicated token per service.

## 6. Reaching the admin console remotely

Pick one:

- **On the server itself:** <http://127.0.0.1:8080/admin>.
- **SSH tunnel:** run this, then open <http://127.0.0.1:8080/admin> locally.

  ```bash
  ssh -L 8080:127.0.0.1:8080 you@server
  ```
- **Tailscale or another VPN.** Set `SMARTPAGER_BIND` to the VPN interface address, never `0.0.0.0` on a machine with port-forwarding.

## 7. Firmware updates over the air

Updates are signed with **your** key, which never leaves your computer. The pager verifies them with the public key compiled into its firmware.

```bash
make ota-keygen       # once: secrets/ota_signing_key.pem (back it up!) + firmware/include/ota_pubkey.h
make fw-flash         # once over USB, so the pager trusts your key
# later, for each release: bump SMARTPAGER_VERSION in firmware/src/app_state.h, then
make ota-publish      # build, sign, copy into the server's data volume
```

The pager checks every 6 hours and after boot. **Settings → Update** shows *0.2.1 ready*, and holding KEY there installs it. Before switching the boot partition, the pager checks:
- the manifest signature (ECDSA P-256 over version, size and SHA-256),
- that the version is newer, and
- the SHA-256 of the downloaded image.

A compromised server can withhold updates but can't install anything. There is no automatic rollback: if a release doesn't boot, reflash it over USB.

## 8. Operations checklist

- [ ] `make backup` regularly; copy `backups/` off the machine.
- [ ] Back up `secrets/ota_signing_key.pem` and `.env`, both outside git.
- [ ] Docker Desktop → *Start when you sign in*, so `restart: unless-stopped` survives a reboot.
- [ ] Lost pager: `make rotate ID=pager-1` immediately. Cached messages on it are not encrypted (see [SECURITY.md](SECURITY.md)).
- [ ] Glance at `make audit` now and then.

## Limits of this setup

- **Single process.** Rate limits and long-poll wake-ups are in memory, which is fine for one user and a few pagers.
- **Client IPs on macOS.** With Docker Desktop, rate limiting and the audit log see every client as the same address. Failed-login limiting still works, but it is per server rather than per attacker. On Linux, real client IPs are preserved.
- **No user accounts.** All pagers share one inbox.
