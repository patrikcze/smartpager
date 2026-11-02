"""REST API v1."""
from __future__ import annotations

import sqlite3
import time

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request, Response, status
from fastapi.concurrency import run_in_threadpool

from ..models.schemas import (
    DeviceCreate,
    DeviceCreated,
    DeviceEventsRequest,
    DeviceEventsResponse,
    DeviceInfo,
    Message,
    MessageCreate,
    MessageCreated,
    MessageList,
    Source,
    SyncResponse,
)
from ..services import messages as svc
from .deps import get_conn, require_admin, require_device, require_ingest

router = APIRouter(prefix="/api/v1")


def _notify(request: Request) -> None:
    request.app.state.notifier.notify()


# ---------------------------------------------------------------- messages (admin / ingest)

@router.post("/messages", response_model=MessageCreated, status_code=status.HTTP_201_CREATED)
def create_message(body: MessageCreate, request: Request, response: Response,
                   idempotency_key: str | None = Header(default=None, max_length=100),
                   _: str = Depends(require_ingest), conn: sqlite3.Connection = Depends(get_conn)):
    row, duplicate = svc.create_message(conn, body, idempotency_key)
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
def mark_read(message_id: int, request: Request, _: str = Depends(require_admin),
              conn: sqlite3.Connection = Depends(get_conn)):
    _existing(conn, message_id)
    row = svc.mark_message(conn, message_id, "read")
    _notify(request)
    return svc.row_to_message(row)


@router.post("/messages/{message_id}/ack", response_model=Message)
def mark_ack(message_id: int, request: Request, _: str = Depends(require_admin),
             conn: sqlite3.Connection = Depends(get_conn)):
    _existing(conn, message_id)
    row = svc.mark_message(conn, message_id, "ack")
    _notify(request)
    return svc.row_to_message(row)


@router.delete("/messages/{message_id}", status_code=status.HTTP_204_NO_CONTENT)
def delete_message(message_id: int, request: Request, _: str = Depends(require_admin),
                   conn: sqlite3.Connection = Depends(get_conn)):
    if not svc.delete_message(conn, message_id):
        raise HTTPException(status.HTTP_404_NOT_FOUND, "message not found")
    _notify(request)


# ---------------------------------------------------------------- devices (admin)

@router.get("/devices", response_model=list[DeviceInfo])
def list_devices(_: str = Depends(require_admin), conn: sqlite3.Connection = Depends(get_conn)):
    return [svc.device_to_dict(r) for r in svc.list_devices(conn)]


@router.post("/devices", response_model=DeviceCreated, status_code=status.HTTP_201_CREATED)
def create_device(body: DeviceCreate, _: str = Depends(require_admin),
                  conn: sqlite3.Connection = Depends(get_conn)):
    try:
        row, token = svc.create_device(conn, body.id, body.name)
    except sqlite3.IntegrityError:
        raise HTTPException(status.HTTP_409_CONFLICT, "device already exists")
    return DeviceCreated(device=svc.device_to_dict(row), token=token)


@router.post("/devices/{device_id}/token", response_model=DeviceCreated)
def rotate_token(device_id: str, _: str = Depends(require_admin),
                 conn: sqlite3.Connection = Depends(get_conn)):
    token = svc.rotate_device_token(conn, device_id)
    if token is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "device not found")
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
