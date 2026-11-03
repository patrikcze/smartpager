# Secure setup guide

From an empty server to pagers that receive messages **encrypted over HTTPS** from anywhere. The examples use `pager.example.com`; replace it with your own domain everywhere.

- [1. How the pieces fit](#1-how-the-pieces-fit)
- [2. What you need](#2-what-you-need)
- [3. Prepare the server machine](#3-prepare-the-server-machine)
- [4. Configure SmartPager](#4-configure-smartpager)
- [5. Start it and obtain the certificate](#5-start-it-and-obtain-the-certificate)
- [6. Connect a pager](#6-connect-a-pager)
- [7. Send messages securely](#7-send-messages-securely)
- [8. Use the admin console safely](#8-use-the-admin-console-safely)
- [9. Firmware updates](#9-firmware-updates)
- [10. Day-to-day operations](#10-day-to-day-operations)
- [11. Security checklist](#11-security-checklist)
- [Appendix A: ports 80/443 are already in use](#appendix-a-ports-80443-are-already-in-use)
- [Appendix B: moving existing data to the new server](#appendix-b-moving-existing-data-to-the-new-server)
- [Appendix C: testing on a home network without HTTPS](#appendix-c-testing-on-a-home-network-without-https)

---

## 1. How the pieces fit

```
  pager(s) ─────────┐  HTTPS (TLS 1.2+, certificate verified on the pager)
  scripts, webhooks ┼──────────────▶  Caddy :443  ──▶  SmartPager server  ──▶  SQLite
  e-mail connector ─┘    internet       Let's Encrypt     (Docker network,      (Docker volume)
                                        auto-renewing      not reachable from
                                                           outside)
  you (admin) ──▶ SSH tunnel ──▶ http://127.0.0.1:8080/admin   (never exposed publicly)
```

| Credential | Lives | What it can do |
|---|---|---|
| **Device token** (`spd_…`) | on each pager | sync its own inbox, report read/ack, download signed firmware |
| **Ingest token** (`spi_…`) | in each script or service | create messages only |
| **Admin token** | server's `.env` | everything, but only on the server's local port |
| **OTA signing key** | your own computer (`secrets/`), **never on the server** | sign firmware updates |

What is and isn't encrypted is summarised in [SECURITY.md](SECURITY.md#what-is-encrypted).

## 2. What you need

- **A Linux server with a public IP.** For example a small cloud VM (Oracle Cloud, Hetzner, …) or a home server with port forwarding. 1 CPU, 1 GB RAM and 5 GB disk are plenty. Both x86-64 and ARM (e.g. Oracle Ampere) work.
- **A domain name** with a DNS record for the pager name pointing at that IP, as an `A` record or a `CNAME` to a name that has one.
- **Your own computer** (macOS or Linux) with this repository, [PlatformIO](https://platformio.org/install/cli) and a USB cable, to flash pagers and sign firmware.

Check DNS from your computer:

```bash
dig +short pager.example.com
```

It must print your server's public IP. If the name also has an `AAAA` (IPv6) record, the server must be reachable over IPv6 too, or remove that record: Let's Encrypt tries IPv6 first.

## 3. Prepare the server machine

Log in over SSH and install Docker (with the Compose plugin), `git`, `make` and Python 3.

```bash
# Ubuntu / Debian
sudo apt update && sudo apt install -y git make python3 ca-certificates curl
curl -fsSL https://get.docker.com | sudo sh      # Docker Engine + compose plugin (official script)
sudo usermod -aG docker $USER                    # then log out and back in
```

On **Oracle Linux 8/9** (the default user is `opc`):

```bash
sudo dnf -y update
sudo dnf install -y git make python3 dnf-utils
sudo dnf remove -y podman buildah runc 2>/dev/null || true      # conflicts with Docker's containerd, if present
sudo dnf config-manager --add-repo https://download.docker.com/linux/rhel/docker-ce.repo
sudo dnf install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
sudo systemctl enable --now docker
sudo usermod -aG docker $USER                                     # then log out and back in
docker compose version                                            # check
```

For automatic security updates on Oracle Linux, run `sudo dnf install -y dnf-automatic`, set `apply_updates = yes` in `/etc/dnf/automatic.conf`, then `sudo systemctl enable --now dnf-automatic.timer`.

**Open ports 80 and 443.** Let's Encrypt needs port 80 to verify the domain. Pagers and scripts use 443. Cloud providers usually block both **twice**, and both layers must allow them:

1. **Cloud firewall.**
   - Oracle Cloud: *Networking → Virtual cloud networks → your VCN → Security Lists* (or the instance's *Network Security Group*). Add ingress rules for TCP **80** and **443** from `0.0.0.0/0`.
   - Other providers: the equivalent "security group" / firewall rules.
2. **Firewall on the VM itself:**
   ```bash
   # Ubuntu with ufw
   sudo ufw allow 80/tcp && sudo ufw allow 443/tcp
   ```
   ```bash
   # Oracle Linux / Rocky / RHEL (firewalld)
   sudo firewall-cmd --permanent --add-service=http --add-service=https && sudo firewall-cmd --reload
   ```
   If this says `firewall-cmd: command not found`, the image has no firewalld (the package is called `firewalld`, not `firewall-cmd`). Check `sudo iptables -S INPUT`: if it shows only `-P INPUT ACCEPT`, the VM itself blocks nothing and the cloud firewall is your only layer, so you can skip this step.
   Oracle's Ubuntu images ship their own `iptables` rules that reject everything except SSH. If HTTPS doesn't connect later, insert accept rules before the final REJECT, and persist them:
   ```bash
   sudo iptables -I INPUT 5 -p tcp --dport 80 -j ACCEPT && sudo iptables -I INPUT 5 -p tcp --dport 443 -j ACCEPT && sudo netfilter-persistent save
   ```

**Check that nothing else uses ports 80/443.** If your domain already serves a website from this machine:

```bash
sudo ss -ltnp '( sport = :80 or sport = :443 )'
```

If this shows nginx, Apache or another Caddy, follow [Appendix A](#appendix-a-ports-80443-are-already-in-use) instead of §5.

**Get the code onto the server:**

```bash
git clone https://github.com/<you>/<repo>.git smartpager && cd smartpager
```

## 4. Configure SmartPager

On the **server**:

```bash
make env
```

This creates `.env` with random admin and ingest tokens. It's readable only by you and never committed. Edit three lines in it:

```bash
SMARTPAGER_DOMAIN=pager.example.com
ACME_EMAIL=you@example.com          # Let's Encrypt expiry notices
SMARTPAGER_BIND=127.0.0.1           # plain-HTTP port stays local; everything public goes through HTTPS
```

On **your own computer**, once, create the firmware-signing key and back it up (password manager or encrypted drive). It never goes to the server:

```bash
make ota-keygen
```

## 5. Start it and obtain the certificate

On the server:

```bash
make up-public
```

This builds the server image, starts it, then starts **Caddy**. Caddy requests a Let's Encrypt certificate for `SMARTPAGER_DOMAIN` automatically: no certbot, no manual steps, and Caddy renews it automatically well before it expires. Watch it:

```bash
make logs-caddy
```

Look for `certificate obtained successfully`. If you see `timeout during connect` or `connection refused`, port 80 isn't reachable from the internet: recheck both firewall layers (§3) and DNS (§2). Caddy keeps retrying, and switches to Let's Encrypt's staging service while failing, so you won't hit rate limits.

Verify from **another network**, e.g. your laptop or a phone on mobile data:

```bash
curl https://pager.example.com/health
```

It should return `{"status":"ok",…}`.

These must return `Not Found`:

```bash
curl https://pager.example.com/admin
```

```bash
curl https://pager.example.com/api/v1/messages
```

The containers restart automatically after a reboot (`restart: unless-stopped`; Docker Engine starts at boot on Linux).

## 6. Connect a pager

### 6.1 Register it on the server

```bash
make device ID=pager-1
```

This prints `token: spd_…` **once**; only a hash is stored. If the device already exists, e.g. after importing data (Appendix B), issue a fresh token with `make rotate ID=pager-1`.

### 6.2 Flash it (first time only, from your computer over USB)

```bash
make fw-flash
```

The firmware contains the Let's Encrypt root certificates and your OTA public key. Nothing has to be pasted for HTTPS. See [FLASHING.md](FLASHING.md) for board details.

### 6.3 Enter the settings

A pager without settings starts **setup mode**. Its screen shows a Wi-Fi name `SmartPager-XXXX`, a one-time password, and `http://192.168.4.1`.

**Option A: phone or laptop**

1. Join `SmartPager-XXXX` with the password from the pager screen.
2. Open `http://192.168.4.1` and fill in:
   - **Wi-Fi network and password**: the network the pager will use day to day
   - **Server URL:** `https://pager.example.com`, with no port and no trailing slash
   - **Device ID:** `pager-1`
   - **Device token:** the `spd_…` value
   - leave *Custom root CA* **empty**: Let's Encrypt is built in
3. Tap **Save and restart**.

**Option B: from your computer over USB.** No typing of long tokens; run it while the setup screen is shown:

```bash
SMARTPAGER_DEVICE_TOKEN=spd_… WIFI_PASSWORD='…' \
  uv run --no-project --with pyserial python tools/provision.py --port /dev/cu.usbmodem1101 \
  --url https://pager.example.com --device pager-1 --ssid "MyWiFi" --restart
```

The secrets come from environment variables, so they stay out of your shell history. Find the port with `pio device list`. Without `uv`, use `pip install pyserial` and run `python3 tools/provision.py …`.

### 6.4 Switching a pager that already uses another server

For example a pager that was tested against a home server over `http://`:

1. **On the new server:**
   - New device: run §6.1.
   - Existing device: `make rotate ID=pager-1`. The old token, which travelled unencrypted, stops working immediately.
2. **On the pager:** *Settings* (short-press BOOT on the home screen) → *Setup mode* → hold **KEY**. The pager restarts into setup mode.
3. **Enter** the new `https://` URL and token with Option A or B of §6.3. Your Wi-Fi settings are kept: leave the Wi-Fi password field empty in the portal, or omit `--ssid` with USB.

When the server URL or device ID changes, the pager deletes its old cached inbox and downloads the new server's inbox. It never mixes the two.

### 6.5 Check the connection

- The pager footer shows *Synced just now*.
- On the server, `docker compose exec smartpager smartpager-admin devices` shows `last_seen_at` updating and `"firmware": "0.2.1"`.
- Send a test message (§7). It appears on the pager within about 1–2 s.

**More pagers:** repeat §6 with `ID=pager-2` and so on. All pagers share one inbox: reading or acknowledging on one marks it on all.

## 7. Send messages securely

Create a separate ingest token for **each** script or service. On the server:

```bash
make ingest-token NAME=backup-script
```

Then send from anywhere over HTTPS:

```bash
curl -X POST https://pager.example.com/api/v1/messages \
  -H "Authorization: Bearer spi_…" -H "Content-Type: application/json" \
  -d '{"source":"system","sender":"NAS","subject":"Backup completed","priority":"normal","external_id":"backup-2026-11-02"}'
```

- **Retries:** give each event a unique `external_id`, so a retried request never pages you twice.
- **More than one pager:** by default every message goes to every pager. Add `"to": ["pager-2"]` to send to specific ones. Or give a service a token that pages only certain pagers: `make ingest-token NAME=grafana TO=pager-2`. Change that later, keeping the same token, with `make ingest-route NAME=grafana TO=pager-1,pager-2` (or `TO=all`). In the console (§8) pick recipients under *To*. If one recipient reads or acknowledges a message, it shows as read or acknowledged on all of them.
- **No custom headers:** services that can't set headers can post to `https://pager.example.com/api/v1/webhooks/generic?token=spi_…`. Only ingest tokens work in URLs.
- **Leaked token:** revoke just that one, in the console (§8) or with `docker compose exec smartpager smartpager-admin ingest-revoke backup-script`.
- **More formats:** see [API.md](API.md) and [INTEGRATIONS.md](INTEGRATIONS.md) (Alertmanager, e-mail).

## 8. Use the admin console safely

The console and the admin API exist **only** on the server's local port; through the public address they return 404, by design. Reach them with an SSH tunnel from your computer:

```bash
ssh -L 8080:127.0.0.1:8080 you@your-server
```

Then open <http://127.0.0.1:8080/admin> locally. Paste the admin token, which you can print on the server with `grep ADMIN .env`. It's kept only in that browser tab; *Lock* forgets it. The tunnel encrypts the connection.

## 9. Firmware updates

Updates are signed on **your computer** (where the key is) and installed on the server. The server can't create or alter firmware; it only hosts it.

```bash
# on your computer: bump SMARTPAGER_VERSION in firmware/src/app_state.h (e.g. 0.2.1 → 0.2.2), then
make ota-sign
scp -r build/ota you@your-server:smartpager/build/
```

```bash
# on the server
make ota-install
```

On the pager: *Settings → Update* shows `0.2.2 ready`. The pager checks every 6 h and after boot. Hold **KEY** to install; it restarts with its inbox intact.

There is no automatic rollback: if a release doesn't boot, reflash over USB with `make fw-flash`. When the server runs on the same machine you sign on, `make ota-publish` does both steps.

## 10. Day-to-day operations

Run these on the server:

| Task | Command |
|---|---|
| Status | `make health`, `make ps`, `make logs`, `make logs-caddy` |
| Backup now | `make backup` (see [Backups](#backups-and-recovery) below) |
| Who did what | `make audit` |
| New admin token | `make rotate-secrets` |
| Lost or stolen pager | `make rotate ID=pager-1`, immediately |
| Remove a pager | `docker compose exec smartpager smartpager-admin device-delete pager-1` |
| Update the server software | `make backup && git pull && make up-public` (database upgrades run automatically on start) |
| Stop everything | `make down` (data and certificates are kept in Docker volumes) |

### Backups and recovery

**What needs protecting, and where it lives:**

| What | Where | Backed up by | If lost |
|---|---|---|---|
| Messages, devices, ingest tokens, audit log | the server's database (Docker volume) | `make backup` | gone, unless backed up |
| Admin token and settings | the server's `.env` | `make backup` | recreate with `make env` |
| Installed firmware update | the server (`/data/firmware`) | `make backup` | sign and install again |
| HTTPS certificate | Caddy's Docker volume | not needed | Caddy obtains a new one on start |
| **OTA signing key** | your computer, `secrets/ota_signing_key.pem` | **you**, see below | pagers accept no more updates until reflashed over USB |

**1. Nightly backups on the server.** Run once, on the server:

```bash
make backup-timer
```

Every night at about 03:30 this writes `backups/smartpager-<date>.tar.gz` (database snapshot checked for integrity, `.env`, firmware release) and keeps the newest 14 (`BACKUP_KEEP` in `.env`). It runs even if the server was off at 03:30 (catch-up on boot). Try it right away and look at the result:

```bash
sudo systemctl start smartpager-backup && journalctl -u smartpager-backup -n 20
```

To be paged when a backup fails, create a token for it and put it into `.env`:

```bash
make ingest-token NAME=backup            # prints spi_…
echo 'BACKUP_NOTIFY_TOKEN=spi_…' >> .env # paste the printed token
```

**2. Copy them off the server.** A backup that lives only on the VM disappears with it. On your computer, in the SmartPager checkout:

```bash
make backup-pull HOST=rocky@pager.example.com SSH_KEY=~/.ssh/your-server-key
```

It downloads only archives you don't have yet into `backups/pager.example.com/` (git-ignored, owner-only access). Run it now and then, or add it to your computer's scheduler.

The archives contain the server's `.env`, so **treat them like passwords**. Keep them on an encrypted disk (FileVault on macOS), and don't put them in a shared or unencrypted cloud folder.

**3. Back up the OTA signing key** (on your computer, once): copy `secrets/ota_signing_key.pem` into your password manager as a secure note, or to an encrypted USB stick. It never needs to go to the server.

**Restore**, on the same or a new server (§3–§5 done, `.env` optional):

```bash
scp backups/pager.example.com/smartpager-<date>.tar.gz rocky@pager.example.com:smartpager/backups/
```

```bash
make restore FILE=backups/smartpager-<date>.tar.gz   # on the server
make up-public
```

Restore shows what the archive contains and asks before replacing anything. It takes a safety backup of the current state first. If the server has no `.env` yet, it uses the one from the archive. Otherwise it keeps yours and saves the archive's copy as `.env.restored-<date>` for comparison. Pagers and integrations keep working with their existing tokens, because those are stored in the database.

To check that a restore works without touching your server, run it on a spare VM or your own computer every few months.

## 11. Security checklist

- [ ] `curl https://pager.example.com/health` works from outside, and `/admin` returns `Not Found`.
- [ ] `SMARTPAGER_BIND=127.0.0.1` on the server. Docker-published ports **bypass `ufw`**, so this setting, not the VM firewall, is what keeps port 8080 private. Check from outside: `curl -m 5 http://pager.example.com:8080/health` must fail.
- [ ] Pagers and integrations use `https://`, never `http://`.
- [ ] One ingest token per integration. Once nothing uses the shared `SMARTPAGER_INGEST_TOKEN`, empty it in `.env` and run `make up-public`.
- [ ] Tokens that ever travelled over plain HTTP are rotated (`make rotate-secrets`, `make rotate ID=…`).
- [ ] The OTA signing key is backed up and exists **only** on your computer.
- [ ] Nightly backups run (`make backup-timer`), copies are pulled off the server (`make backup-pull`), and the OTA signing key is stored safely. The archives contain secrets: keep them on encrypted storage, never shared or committed.
- [ ] SSH on the server uses keys (password login disabled), and the OS gets security updates (`unattended-upgrades` on Ubuntu).
- [ ] You know what isn't protected: messages are readable on the server and on the pager ([SECURITY.md](SECURITY.md#what-is-encrypted)).

---

## Appendix A: ports 80/443 are already in use

If an existing web server (e.g. nginx for your main website) already listens on 80/443, Caddy can't start. Let that web server handle HTTPS for the pager subdomain instead:

1. In the server's `.env`, add:

   ```bash
   SMARTPAGER_TRUSTED_PROXY=172.30.0.1
   ```

   This tells SmartPager to trust the client address forwarded by a proxy on the host. Keep `SMARTPAGER_BIND=127.0.0.1`.
2. Start SmartPager **without** Caddy:

   ```bash
   make up
   ```

3. Configure nginx using [`deploy/nginx-smartpager.conf`](../deploy/nginx-smartpager.conf). It has the same path allow-list as the bundled Caddy, and its comments explain the include file and the certificate.
4. Get the certificate with certbot:

   ```bash
   sudo certbot --nginx -d pager.example.com
   ```

   Then reload nginx (`sudo nginx -t && sudo systemctl reload nginx`).
5. Verify as in §5.

For another proxy, copy the allow-list from [`deploy/Caddyfile`](../deploy/Caddyfile). Set `X-Forwarded-For` to the client address, replacing any client-supplied value rather than appending, and allow at least 60 s for responses (long polls).

## Appendix B: moving existing data to the new server

To keep messages and devices from an earlier server:

```bash
make backup      # on the old machine: backups/smartpager-….tar.gz
```

```bash
scp backups/smartpager-<date>.tar.gz you@your-server:smartpager/backups/
```

```bash
make restore FILE=backups/smartpager-<date>.tar.gz    # on the new server; keeps its own .env
```

A bare SQLite file from elsewhere can be imported with `make import-db DB=path/to.db`.

Then rotate every device token (`make rotate ID=…`) and re-provision the pagers (§6.4). The tokens in that database may have travelled over plain HTTP.

## Appendix C: testing on a home network without HTTPS

1. Set `SMARTPAGER_BIND=0.0.0.0` in `.env` and run `make up`.
2. Use `http://<server-LAN-IP>:8080` as the pager's server URL.

**This is not encrypted:** anyone who can capture traffic on that network can read messages and the device token. Use it on a trusted network, for testing only. Never do this on an internet-facing server.
