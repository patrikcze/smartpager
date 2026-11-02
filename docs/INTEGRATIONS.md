# Integrations

Integrations are optional and separate from the core. All of them inject messages through the same `POST /api/v1/messages` (or `/webhooks/*`) path using the **ingest** token.

## Webhooks (implemented, tested)

```bash
curl -X POST http://127.0.0.1:8080/api/v1/webhooks/generic -H "Authorization: Bearer $SMARTPAGER_INGEST_TOKEN" \
  -H 'Content-Type: application/json' -d '{"title":"Deploy done","message":"v1.2.3 live","sender":"CI","id":"deploy-123"}'
```

Prometheus Alertmanager receiver:

```yaml
receivers:
  - name: pager
    webhook_configs:
      - url: http://smartpager:8080/api/v1/webhooks/alertmanager
        http_config:
          authorization: {type: Bearer, credentials_file: /etc/alertmanager/smartpager-ingest-token}
```

Most tools (Home Assistant `rest_command`, Grafana contact points, GitHub Actions, cron jobs) can call the generic webhook or the plain messages endpoint directly.

## E-mail via IMAP (implemented, tested offline)

`server/app/integrations/imap_connector.py` runs as its own process. With Docker: `docker compose --profile email up -d`. Configure it with the `IMAP_*` variables in `.env.example`.

- It opens the mailbox **read-only** (`EXAMINE`) and fetches with `BODY.PEEK`, so your e-mail is never marked as read or changed.
- It forwards sender, subject, received time, a 400-character plain-text preview, and priority from `X-Priority`.
- `Message-ID` becomes `external_id`, so re-polls and restarts never create duplicates.
- Sender filtering: `IMAP_ALLOW_SENDERS` and `IMAP_DENY_SENDERS` (comma-separated substrings).
- Credentials come only from the environment. Use an **app password** where your provider supports one.

**Not tested against a live mailbox:** no account credentials were available in this environment. The MIME mapping, filters and API acceptance are covered by unit tests.

### Providers that need OAuth (not implemented)

- **Gmail without app passwords:** use the Gmail API with an OAuth client you create in your own Google Cloud project, with the `gmail.readonly` scope, plus `users.watch` or history polling.
- **Microsoft 365 / Exchange Online:** basic-auth IMAP is disabled in most tenants. Use Microsoft Graph with delegated `Mail.Read`, registered in your tenant and subject to your organisation's Conditional Access. Don't work around these policies.

Either connector would reuse `to_payload()`-style mapping and post to the same endpoint.

## macOS Messages / iMessage (evaluated, not implemented)

- Apple offers **no public API for receiving iMessages**.
- The AppleScript "message received" handlers that older Messages versions supported have been removed. Messages' scripting dictionary can still *send* messages but can't subscribe to incoming ones.
- The only receive-side path is to read `~/Library/Messages/chat.db` (SQLite) from a local companion process. macOS protects that file with TCC. It needs **Full Disk Access**, granted by the user in System Settings → Privacy & Security, for the specific binary that reads it.
- The schema is undocumented and changes between macOS releases. Newer releases also store message text in an `attributedBody` blob rather than plain text.

**Decision.** This is technically possible with the user's explicit permission, but it is fragile and privacy-sensitive. It is left out of the initial implementation so it can't delay the core pager. A companion would:
- poll `chat.db` read-only,
- track the last `ROWID`,
- apply a sender allow-list,
- send a short preview with `source: "imessage"` and `external_id = message GUID`, and
- run as a user LaunchAgent with Full Disk Access granted only to that interpreter or binary.
