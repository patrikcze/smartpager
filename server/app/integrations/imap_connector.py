"""Optional IMAP connector: forwards unread e-mail notifications to SmartPager.

Runs as its own process, independent of the server and the firmware:

    IMAP_HOST=imap.example.com IMAP_USER=me@example.com IMAP_PASSWORD=... \\
    SMARTPAGER_URL=http://127.0.0.1:8080 SMARTPAGER_INGEST_TOKEN=... \\
    python -m app.integrations.imap_connector

Guarantees:
* The mailbox is opened read-only (EXAMINE) and bodies are fetched with BODY.PEEK, so the
  original e-mail is never marked as read or modified.
* Only sender, subject, received time and a short text preview are forwarded.
* Message-ID is used as external_id, so restarts and re-polls never create duplicates.

Accounts that do not allow password/app-password IMAP (e.g. Microsoft 365 with basic auth
disabled, or Gmail without app passwords) need an OAuth-based connector; see docs/INTEGRATIONS.md.
"""
from __future__ import annotations

import email
import email.policy
import hashlib
import imaplib
import json
import logging
import os
import re
import signal
import ssl
import time
import urllib.request
from datetime import datetime, timedelta, timezone
from email.message import EmailMessage
from email.utils import parseaddr, parsedate_to_datetime

log = logging.getLogger("smartpager.imap")
PREVIEW_CHARS = 400
MAX_FETCH_BYTES = 512 * 1024


def _text_preview(msg: EmailMessage) -> str:
    part = msg.get_body(preferencelist=("plain", "html"))
    if part is None:
        return ""
    try:
        text = part.get_content()
    except (LookupError, UnicodeDecodeError):
        text = part.get_payload(decode=True).decode("utf-8", errors="replace")
    if part.get_content_type() == "text/html":
        text = re.sub(r"(?is)<(script|style).*?</\1>", " ", text)
        text = re.sub(r"(?s)<[^>]+>", " ", text)
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub(r"[ \t\f\v]+", " ", text)
    text = re.sub(r" *\n[\s]*", "\n", text).strip()  # trim line ends, collapse blank lines
    return text[:PREVIEW_CHARS] + ("…" if len(text) > PREVIEW_CHARS else "")


def to_payload(raw: bytes) -> dict:
    """Map an RFC 5322 message to a SmartPager MessageCreate payload."""
    msg = email.message_from_bytes(raw, policy=email.policy.default)
    name, addr = parseaddr(str(msg.get("From", "")))
    sender = name or addr or "unknown"
    message_id = str(msg.get("Message-ID", "")).strip()
    if not message_id:  # stable fallback so duplicates are still detected
        message_id = "sha256:" + hashlib.sha256(raw[:4096]).hexdigest()
    created = None
    try:
        dt = parsedate_to_datetime(str(msg.get("Date")))
        if dt.tzinfo is not None:
            created = dt.astimezone(timezone.utc).isoformat()
    except (TypeError, ValueError):
        pass
    payload = {
        "source": "email",
        "external_id": message_id[:200],
        "sender": sender[:120],
        "subject": str(msg.get("Subject", ""))[:200],
        "body": _text_preview(msg),
        "priority": "high" if str(msg.get("X-Priority", "")).startswith(("1", "2")) else "normal",
        "metadata": {"from": addr[:120]},
    }
    if created:
        payload["created_at"] = created
    return payload


def sender_allowed(payload: dict, allow: list[str], deny: list[str]) -> bool:
    who = (payload["metadata"].get("from", "") + " " + payload["sender"]).lower()
    if any(d in who for d in deny):
        return False
    return not allow or any(a in who for a in allow)


def post_message(base_url: str, token: str, payload: dict) -> bool:
    req = urllib.request.Request(
        base_url.rstrip("/") + "/api/v1/messages", data=json.dumps(payload).encode(), method="POST",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=15) as resp:  # TLS verified by default for https
        return json.loads(resp.read()).get("duplicate") is False


def poll_once(cfg: dict, seen: set[str]) -> int:
    since = (datetime.now(timezone.utc) - timedelta(days=cfg["lookback_days"])).strftime("%d-%b-%Y")
    forwarded = 0
    with imaplib.IMAP4_SSL(cfg["host"], cfg["port"], ssl_context=ssl.create_default_context()) as imap:
        imap.login(cfg["user"], cfg["password"])
        typ, _ = imap.select(cfg["folder"], readonly=True)  # EXAMINE: flags are never changed
        if typ != "OK":
            raise RuntimeError(f"cannot open folder {cfg['folder']!r}")
        typ, data = imap.uid("SEARCH", None, "UNSEEN", "SINCE", since)
        uids = data[0].split()[-cfg["max_per_poll"]:] if typ == "OK" and data and data[0] else []
        for uid in uids:
            key = uid.decode()
            if key in seen:
                continue
            typ, sz = imap.uid("FETCH", uid, "(RFC822.SIZE)")
            size_match = re.search(rb"RFC822\.SIZE (\d+)", sz[0] if sz and sz[0] else b"")
            part = "BODY.PEEK[]" if size_match and int(size_match.group(1)) <= MAX_FETCH_BYTES else "BODY.PEEK[HEADER]"
            typ, msg_data = imap.uid("FETCH", uid, f"({part})")
            raw = next((p[1] for p in msg_data if isinstance(p, tuple)), None)
            if typ != "OK" or raw is None:
                continue
            payload = to_payload(raw)
            if sender_allowed(payload, cfg["allow"], cfg["deny"]):
                if post_message(cfg["url"], cfg["token"], payload):
                    forwarded += 1
            seen.add(key)
        imap.logout()
    if len(seen) > 5000:  # bounded; the server dedupes anyway
        seen.clear()
    return forwarded


def _csv(name: str) -> list[str]:
    return [s.strip().lower() for s in os.environ.get(name, "").split(",") if s.strip()]


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    cfg = {
        "host": os.environ.get("IMAP_HOST", ""), "port": int(os.environ.get("IMAP_PORT") or "993"),
        "user": os.environ.get("IMAP_USER", ""), "password": os.environ.get("IMAP_PASSWORD", ""),
        "folder": os.environ.get("IMAP_FOLDER", "INBOX"),
        "lookback_days": int(os.environ.get("IMAP_LOOKBACK_DAYS", "2")),
        "max_per_poll": int(os.environ.get("IMAP_MAX_PER_POLL", "25")),
        "allow": _csv("IMAP_ALLOW_SENDERS"), "deny": _csv("IMAP_DENY_SENDERS"),
        "url": os.environ.get("SMARTPAGER_URL", "http://127.0.0.1:8080"),
        "token": os.environ.get("SMARTPAGER_INGEST_TOKEN", ""),
    }
    if not cfg["token"] or not cfg["host"] or not cfg["user"]:
        raise SystemExit("IMAP connector: set IMAP_HOST, IMAP_USER, IMAP_PASSWORD and an ingest token "
                         "(IMAP_INGEST_TOKEN in .env; create it with: make ingest-token NAME=imap)")
    interval = int(os.environ.get("IMAP_POLL_SECONDS", "60"))
    stop = False

    def _stop(*_):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)
    seen: set[str] = set()
    delay = interval
    while not stop:
        try:
            n = poll_once(cfg, seen)
            if n:
                log.info("forwarded %d new e-mail notification(s)", n)  # counts only, no contents
            delay = interval
        except Exception as exc:  # network/auth errors: back off, never crash-loop
            log.warning("poll failed: %s", type(exc).__name__)
            delay = min(delay * 2, 900)
        for _ in range(delay):
            if stop:
                break
            time.sleep(1)


if __name__ == "__main__":
    main()
