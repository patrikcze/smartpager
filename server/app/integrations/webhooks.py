"""Inbound webhook adapters. Each maps a third-party payload onto MessageCreate.

POST /api/v1/webhooks/generic        {"title": "...", "message": "...", "sender": "...", "priority": "high", "id": "..."}
POST /api/v1/webhooks/alertmanager   Prometheus Alertmanager webhook (version 4)

Both use an ingest token: as `Authorization: Bearer` header or, for senders that can't set
headers, as `?token=` (ingest tokens only, never the admin token). Re-deliveries are
deduplicated through external_id.

Recipients: `?to=pager-1,pager-2` (or "to" in the generic body) sends to those pagers only;
without it the token's default recipients apply, else every pager.
"""
from __future__ import annotations

import hashlib
import sqlite3
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Query, Request, Response, status
from pydantic import BaseModel, ConfigDict, Field, ValidationError

from ..api.deps import get_conn, require_ingest_webhook
from ..models.schemas import MAX_BODY, MAX_SENDER, MAX_SUBJECT, MessageCreate, Priority, Source
from ..services import messages as svc
from ..services import security

router = APIRouter(prefix="/api/v1/webhooks", tags=["webhooks"])


def _clip(text: str, limit: int) -> str:
    return text if len(text) <= limit else text[: limit - 1] + "…"


class GenericHook(BaseModel):
    model_config = ConfigDict(extra="ignore")  # tolerate extra fields from arbitrary senders
    title: str = Field(default="", max_length=1000)
    message: str = Field(default="", max_length=20000)
    sender: str = Field(default="Webhook", max_length=500)
    priority: Priority = Priority.normal
    id: str | None = Field(default=None, max_length=200)
    to: list[str] | str | None = None  # device ids, as a list or "pager-1,pager-2"


class AlertmanagerHook(BaseModel):
    model_config = ConfigDict(extra="ignore")
    status: str = Field(max_length=20)
    receiver: str = Field(default="", max_length=200)
    groupKey: str = Field(default="", max_length=2000)
    commonLabels: dict[str, str] = Field(default_factory=dict)
    commonAnnotations: dict[str, str] = Field(default_factory=dict)
    alerts: list[dict[str, Any]] = Field(default_factory=list, max_length=200)


def _recipients(value: list[str] | str | None) -> list[str] | None:
    if value is None or value == "":
        return None
    items = value.split(",") if isinstance(value, str) else value
    return [i.strip() for i in items if i.strip()] or None


def _message(**fields: Any) -> MessageCreate:
    try:
        return MessageCreate(**fields)
    except ValidationError as e:  # e.g. a malformed device id in `to`
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY,
                            "; ".join(f"{'.'.join(map(str, x['loc']))}: {x['msg']}" for x in e.errors()))


def _store(conn: sqlite3.Connection, request: Request, response: Response, msg: MessageCreate,
           actor: str) -> dict:
    try:
        row, duplicate = svc.create_message(conn, msg, origin=actor,
                                            default_recipients=security.ingest_recipients(conn, actor))
    except svc.UnknownDevices as e:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, str(e))
    if not duplicate:
        request.app.state.notifier.notify()
        response.status_code = status.HTTP_201_CREATED
    return {"id": row["id"], "duplicate": duplicate}


@router.post("/generic")
def generic(hook: GenericHook, request: Request, response: Response,
            to: str | None = Query(default=None, max_length=1000),
            actor: str = Depends(require_ingest_webhook), conn: sqlite3.Connection = Depends(get_conn)):
    if not hook.title and not hook.message:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "title or message required")
    msg = _message(source=Source.webhook, external_id=hook.id, sender=_clip(hook.sender, MAX_SENDER),
                   subject=_clip(hook.title, MAX_SUBJECT), body=_clip(hook.message, MAX_BODY),
                   priority=hook.priority, to=_recipients(hook.to) or _recipients(to))
    return _store(conn, request, response, msg, actor)


@router.post("/alertmanager")
def alertmanager(hook: AlertmanagerHook, request: Request, response: Response,
                 to: str | None = Query(default=None, max_length=1000),
                 actor: str = Depends(require_ingest_webhook), conn: sqlite3.Connection = Depends(get_conn)):
    labels = hook.commonLabels
    severity = labels.get("severity", "").lower()
    firing = hook.status == "firing"
    priority = (Priority.critical if severity in ("critical", "page") else Priority.high) if firing else Priority.low
    name = labels.get("alertname", "Alert")
    subject = f"[{hook.status.upper()}] {name}"
    lines = []
    for a in hook.alerts[:20]:
        ann = a.get("annotations") or {}
        lab = a.get("labels") or {}
        text = ann.get("summary") or ann.get("description") or lab.get("instance") or ""
        lines.append(f"• {a.get('status', '')}: {text}".strip())
    if len(hook.alerts) > 20:
        lines.append(f"… and {len(hook.alerts) - 20} more")
    # Alertmanager re-sends the same group repeatedly; one message per group+status+alert set.
    digest = hashlib.sha256(
        (hook.groupKey + hook.status + "".join(sorted(str(a.get("fingerprint", "")) + str(a.get("startsAt", ""))
                                                  for a in hook.alerts))).encode()
    ).hexdigest()[:32]
    msg = _message(source=Source.monitoring, external_id=f"am:{digest}",
                   sender=_clip(hook.receiver or "Alertmanager", MAX_SENDER),
                   subject=_clip(subject, MAX_SUBJECT), body=_clip("\n".join(lines), MAX_BODY),
                   priority=priority, metadata={"alertname": name[:100], "status": hook.status},
                   to=_recipients(to))
    return _store(conn, request, response, msg, actor)
