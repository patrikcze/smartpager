"""REST API v1."""
from __future__ import annotations

import json
import os
import sqlite3
import time

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request, Response, status
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import FileResponse
from pydantic import ValidationError

from ..models.schemas import (
    DeviceCreate,
    DeviceCreated,
    DeviceEventsRequest,
    DeviceEventsResponse,
    DeviceInfo,
    AuditEntry,
    FirmwareManifest,
    IngestTokenCreate,
    IngestTokenCreated,
    IngestTokenInfo,
    Message,
    MessageCreate,
    MessageCreated,
    MessageList,
    Source,
    SyncResponse,
)
from ..services import messages as svc
from ..services import security
from .deps import client_ip, get_conn, require_admin, require_device, require_ingest

router = APIRouter(prefix="/api/v1")


def _notify(request: Request) -> None:
    request.app.state.notifier.notify()


def _audit(conn: sqlite3.Connection, request: Request, actor: str, action: str, target: str = "") -> None:
    security.audit(conn, actor, action, target, client_ip(request))


# ---------------------------------------------------------------- messages (admin / ingest)

@router.post("/messages", response_model=MessageCreated, status_code=status.HTTP_201_CREATED)
def create_message(body: MessageCreate, request: Request, response: Response,
                   idempotency_key: str | None = Header(default=None, max_length=100),
                   actor: str = Depends(require_ingest), conn: sqlite3.Connection = Depends(get_conn)):
    row, duplicate = svc.create_message(conn, body, idempotency_key, origin=actor)
    if duplicate:
        response.status_code = status.HTTP_200_OK
    else:
        _notify(request)
    return MessageCreated(message=svc.row_to_message(row), duplicate=duplicate)


@router.get("/messages", response_model=MessageList)
def list_messages(source: Source | None = None, unread: bool = False, include_deleted: bool = False,
                  before_id: int | None = Query(default=None, ge=1),
                  limit: int = Query(default=50, ge=1, le=200),
                  _: str = Depends(require_admin), conn: sqlite3.Connection = Depends(get_conn)):
    rows = svc.list_messages(conn, source=source.value if source else None, unread_only=unread,
                             include_deleted=include_deleted, before_id=before_id, limit=limit)
    return MessageList(messages=[svc.row_to_message(r) for r in rows],
                       next_before_id=rows[-1]["id"] if len(rows) == limit else None)


def _existing(conn: sqlite3.Connection, message_id: int) -> sqlite3.Row:
    row = svc.get_message(conn, message_id)
    if row is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "message not found")
    return row


@router.get("/messages/{message_id}", response_model=Message)
def get_message(message_id: int, _: str = Depends(require_admin),
                conn: sqlite3.Connection = Depends(get_conn)):
    return svc.row_to_message(_existing(conn, message_id))


@router.post("/messages/{message_id}/read", response_model=Message)
def mark_read(message_id: int, request: Request, actor: str = Depends(require_admin),
              conn: sqlite3.Connection = Depends(get_conn)):
    _existing(conn, message_id)
    row = svc.mark_message(conn, message_id, "read")
    _audit(conn, request, actor, "message.read", f"message:{message_id}")
    _notify(request)
    return svc.row_to_message(row)


@router.post("/messages/{message_id}/ack", response_model=Message)
def mark_ack(message_id: int, request: Request, actor: str = Depends(require_admin),
             conn: sqlite3.Connection = Depends(get_conn)):
    _existing(conn, message_id)
    row = svc.mark_message(conn, message_id, "ack")
    _audit(conn, request, actor, "message.ack", f"message:{message_id}")
    _notify(request)
    return svc.row_to_message(row)


@router.delete("/messages/{message_id}", status_code=status.HTTP_204_NO_CONTENT)
def delete_message(message_id: int, request: Request, actor: str = Depends(require_admin),
                   conn: sqlite3.Connection = Depends(get_conn)):
    if not svc.delete_message(conn, message_id):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "message not found")
    _audit(conn, request, actor, "message.delete", f"message:{message_id}")
    _notify(request)


# ---------------------------------------------------------------- devices (admin)

@router.get("/devices", response_model=list[DeviceInfo])
def list_devices(_: str = Depends(require_admin), conn: sqlite3.Connection = Depends(get_conn)):
    return [svc.device_to_dict(r) for r in svc.list_devices(conn)]


@router.post("/devices", response_model=DeviceCreated, status_code=status.HTTP_201_CREATED)
def create_device(body: DeviceCreate, request: Request, actor: str = Depends(require_admin),
                  conn: sqlite3.Connection = Depends(get_conn)):
    try:
        row, token = svc.create_device(conn, body.id, body.name)
    except sqlite3.IntegrityError:
        raise HTTPException(status.HTTP_409_CONFLICT, "device already exists")
    _audit(conn, request, actor, "device.create", f"device:{body.id}")
    return DeviceCreated(device=svc.device_to_dict(row), token=token)


@router.post("/devices/{device_id}/token", response_model=DeviceCreated)
def rotate_token(device_id: str, request: Request, actor: str = Depends(require_admin),
                 conn: sqlite3.Connection = Depends(get_conn)):
    token = svc.rotate_device_token(conn, device_id)
    if token is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "device not found")
    _audit(conn, request, actor, "device.rotate_token", f"device:{device_id}")
    row = next(r for r in svc.list_devices(conn) if r["id"] == device_id)
    return DeviceCreated(device=svc.device_to_dict(row), token=token)


# ---------------------------------------------------------------- device protocol

@router.get("/devices/{device_id}/sync", response_model=SyncResponse, response_model_by_alias=True)
async def device_sync(device_id: str, request: Request,
                      cursor: int = Query(default=0, ge=0),
                      epoch: str | None = Query(default=None, max_length=16),
                      limit: int = Query(default=10, ge=1, le=50),
                      max_body: int = Query(default=1024, ge=64, le=4096),
                      wait: int = Query(default=0, ge=0, le=60),
                      fw: str | None = Query(default=None, max_length=32),
                      device: sqlite3.Row = Depends(require_device),
                      conn: sqlite3.Connection = Depends(get_conn)):
    """Incremental sync. `wait` > 0 enables long polling: when there is nothing new the
    request is held until a change is committed or the wait expires."""
    ip = request.client.host if request.client else None
    await run_in_threadpool(svc.touch_device, conn, device_id, ip, fw)
    notifier = request.app.state.notifier
    deadline = time.monotonic() + min(wait, request.app.state.settings.max_long_poll_s)
    while True:
        token = notifier.token()
        result = await run_in_threadpool(svc.sync, conn, cursor=cursor, epoch=epoch,
                                         limit=limit, max_body=max_body)
        remaining = deadline - time.monotonic()
        if result.items or result.reset or remaining <= 0:
            return result
        if await request.is_disconnected():
            return result
        await notifier.wait(token, min(remaining, 5.0))


@router.post("/devices/{device_id}/events", response_model=DeviceEventsResponse)
def device_events(device_id: str, body: DeviceEventsRequest, request: Request,
                  device: sqlite3.Row = Depends(require_device),
                  conn: sqlite3.Connection = Depends(get_conn)):
    result = svc.apply_device_events(conn, device_id, body)
    if result.accepted:
        _notify(request)
    return result


# ---------------------------------------------------------------- integration tokens & audit (admin)

@router.get("/tokens", response_model=list[IngestTokenInfo])
def list_tokens(_: str = Depends(require_admin), conn: sqlite3.Connection = Depends(get_conn)):
    return [security.token_to_dict(r) for r in security.list_ingest_tokens(conn)]


@router.post("/tokens", response_model=IngestTokenCreated, status_code=status.HTTP_201_CREATED)
def create_token(body: IngestTokenCreate, request: Request, actor: str = Depends(require_admin),
                 conn: sqlite3.Connection = Depends(get_conn)):
    """Create a named ingest token for one integration. The token is returned only here."""
    try:
        row, token = security.create_ingest_token(conn, body.name)
    except sqlite3.IntegrityError:
        raise HTTPException(status.HTTP_409_CONFLICT, "a token with this name exists")
    _audit(conn, request, actor, "token.create", f"token:{body.name}")
    return IngestTokenCreated(token_info=security.token_to_dict(row), token=token)


@router.delete("/tokens/{token_id}", response_model=IngestTokenInfo)
def revoke_token(token_id: int, request: Request, actor: str = Depends(require_admin),
                 conn: sqlite3.Connection = Depends(get_conn)):
    row = security.revoke_ingest_token(conn, token_id)
    if row is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "token not found")
    _audit(conn, request, actor, "token.revoke", f"token:{row['name']}")
    return security.token_to_dict(row)


@router.get("/audit", response_model=list[AuditEntry])
def audit_log(limit: int = Query(default=100, ge=1, le=1000), _: str = Depends(require_admin),
              conn: sqlite3.Connection = Depends(get_conn)):
    return security.list_audit(conn, limit)


# ---------------------------------------------------------------- signed OTA firmware (device)

def _manifest(request: Request) -> tuple[FirmwareManifest, str]:
    """Load the published manifest. The server only distributes; the device verifies the
    signature with its built-in public key, so a compromised server can't push firmware."""
    base = request.app.state.settings.firmware_dir
    image = os.path.join(base, "firmware.bin")
    try:
        with open(os.path.join(base, "manifest.json"), encoding="utf-8") as f:
            manifest = FirmwareManifest.model_validate(json.load(f))
    except (OSError, ValueError, ValidationError):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no firmware published")
    if not os.path.isfile(image) or os.path.getsize(image) != manifest.size:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no firmware published")
    return manifest, image


@router.get("/devices/{device_id}/firmware", response_model=FirmwareManifest)
def firmware_manifest(device_id: str, request: Request, device: sqlite3.Row = Depends(require_device)):
    return _manifest(request)[0]


@router.get("/devices/{device_id}/firmware/image")
def firmware_image(device_id: str, request: Request, device: sqlite3.Row = Depends(require_device),
                   conn: sqlite3.Connection = Depends(get_conn)):
    manifest, image = _manifest(request)
    _audit(conn, request, f"device:{device_id}", "firmware.download", f"version:{manifest.version}")
    return FileResponse(image, media_type="application/octet-stream")
