"""Inbound webhook adapters. Each maps a third-party payload onto MessageCreate.

POST /api/v1/webhooks/generic        {"title": "...", "message": "...", "sender": "...", "priority": "high", "id": "..."}
POST /api/v1/webhooks/alertmanager   Prometheus Alertmanager webhook (version 4)

Both use the ingest token. Re-deliveries are deduplicated through external_id.
"""
from __future__ import annotations

import hashlib
import sqlite3
from typing import Any

from fastapi import APIRouter, Depends, HTTPException, Request, Response, status
from pydantic import BaseModel, ConfigDict, Field

from ..api.deps import get_conn, require_ingest
from ..models.schemas import MAX_BODY, MAX_SENDER, MAX_SUBJECT, MessageCreate, Priority, Source
from ..services import messages as svc

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


class AlertmanagerHook(BaseModel):
    model_config = ConfigDict(extra="ignore")
    status: str = Field(max_length=20)
    receiver: str = Field(default="", max_length=200)
    groupKey: str = Field(default="", max_length=2000)
    commonLabels: dict[str, str] = Field(default_factory=dict)
    commonAnnotations: dict[str, str] = Field(default_factory=dict)
    alerts: list[dict[str, Any]] = Field(default_factory=list, max_length=200)


def _store(conn: sqlite3.Connection, request: Request, response: Response, msg: MessageCreate) -> dict:
    row, duplicate = svc.create_message(conn, msg)
    if not duplicate:
        request.app.state.notifier.notify()
        response.status_code = status.HTTP_201_CREATED
    return {"id": row["id"], "duplicate": duplicate}


@router.post("/generic")
def generic(hook: GenericHook, request: Request, response: Response, _: str = Depends(require_ingest),
            conn: sqlite3.Connection = Depends(get_conn)):
    if not hook.title and not hook.message:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "title or message required")
    msg = MessageCreate(source=Source.webhook, external_id=hook.id, sender=_clip(hook.sender, MAX_SENDER),
                        subject=_clip(hook.title, MAX_SUBJECT), body=_clip(hook.message, MAX_BODY),
                        priority=hook.priority)
    return _store(conn, request, response, msg)


@router.post("/alertmanager")
def alertmanager(hook: AlertmanagerHook, request: Request, response: Response, _: str = Depends(require_ingest),
                 conn: sqlite3.Connection = Depends(get_conn)):
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
    msg = MessageCreate(source=Source.monitoring, external_id=f"am:{digest}",
                        sender=_clip(hook.receiver or "Alertmanager", MAX_SENDER),
                        subject=_clip(subject, MAX_SUBJECT), body=_clip("\n".join(lines), MAX_BODY),
                        priority=priority, metadata={"alertname": name[:100], "status": hook.status})
    return _store(conn, request, response, msg)
