"""Request/response models and payload limits."""
from __future__ import annotations

import json
from datetime import datetime
from enum import Enum
from typing import Annotated, Any, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator

MAX_SENDER = 120
MAX_SUBJECT = 200
MAX_BODY = 4000
MAX_METADATA_BYTES = 2048
SYNC_SCHEMA = 1
MAX_RECIPIENTS = 20
DEVICE_ID_PATTERN = r"^[a-z0-9][a-z0-9-]+$"

DeviceId = Annotated[str, Field(min_length=3, max_length=40, pattern=DEVICE_ID_PATTERN)]
# None = every device; otherwise the listed device ids (at least one).
Recipients = Annotated[list[DeviceId] | None, Field(default=None, min_length=1, max_length=MAX_RECIPIENTS)]


def _unique_sorted(v: list[str] | None) -> list[str] | None:
    return sorted(set(v)) if v is not None else None


class Source(str, Enum):
    manual = "manual"
    webhook = "webhook"
    email = "email"
    imessage = "imessage"
    system = "system"
    monitoring = "monitoring"


class Priority(str, Enum):
    low = "low"
    normal = "normal"
    high = "high"
    critical = "critical"


PRIORITY_TO_INT = {Priority.low: 0, Priority.normal: 1, Priority.high: 2, Priority.critical: 3}
INT_TO_PRIORITY = {v: k for k, v in PRIORITY_TO_INT.items()}


def _no_control_chars(value: str) -> str:
    # keep newlines/tabs, drop other C0 controls and DEL which the device font cannot render
    return "".join(ch for ch in value if ch in "\n\t" or (ord(ch) >= 0x20 and ord(ch) != 0x7F))


class MessageCreate(BaseModel):
    model_config = ConfigDict(extra="forbid")

    source: Source = Source.manual
    external_id: str | None = Field(default=None, min_length=1, max_length=200)
    sender: str = Field(default="", max_length=MAX_SENDER)
    subject: str = Field(default="", max_length=MAX_SUBJECT)
    body: str = Field(default="", max_length=MAX_BODY)
    priority: Priority = Priority.normal
    created_at: datetime | None = None
    expires_at: datetime | None = None
    metadata: dict[str, Any] = Field(default_factory=dict)
    to: Recipients = None  # device ids; omitted = the sending token's default, else every device

    @field_validator("to")
    @classmethod
    def _dedupe(cls, v: list[str] | None) -> list[str] | None:
        return _unique_sorted(v)

    @field_validator("sender", "subject", "body")
    @classmethod
    def _clean(cls, v: str) -> str:
        return _no_control_chars(v)

    @field_validator("metadata")
    @classmethod
    def _bounded_metadata(cls, v: dict[str, Any]) -> dict[str, Any]:
        if len(json.dumps(v, ensure_ascii=False).encode()) > MAX_METADATA_BYTES:
            raise ValueError(f"metadata exceeds {MAX_METADATA_BYTES} bytes")
        return v

    @field_validator("created_at", "expires_at")
    @classmethod
    def _tz_aware(cls, v: datetime | None) -> datetime | None:
        if v is not None and v.tzinfo is None:
            raise ValueError("timestamps must include a timezone offset (e.g. 2026-11-02T14:00:00Z)")
        return v


class Message(BaseModel):
    id: int
    seq: int
    source: str
    external_id: str | None
    sender: str
    subject: str
    body: str
    priority: Priority
    created_at: str
    received_at: str
    expires_at: str | None
    delivered_at: str | None
    read_at: str | None
    acknowledged_at: str | None
    deleted_at: str | None
    metadata: dict[str, Any]
    origin: str | None = None  # which credential created it: "admin", "ingest", "token:<name>"
    recipients: list[str] | None = None  # None = every device
    delivered_to: dict[str, str] = Field(default_factory=dict)  # device id -> delivery time


class MessageCreated(BaseModel):
    message: Message
    duplicate: bool


class MessageList(BaseModel):
    messages: list[Message]
    next_before_id: int | None


class SyncItem(BaseModel):
    """Compact representation sent to the device. Timestamps are unix seconds UTC, 0 = unset."""
    id: int
    seq: int
    deleted: bool = False
    src: str = ""
    from_: str = Field(default="", serialization_alias="from")
    subj: str = ""
    body: str = ""
    trunc: bool = False
    pri: int = 1
    ts: int = 0
    read: int = 0
    ack: int = 0


class SyncResponse(BaseModel):
    schema_: int = Field(default=SYNC_SCHEMA, serialization_alias="schema")
    epoch: str
    server_time: int
    reset: bool
    cursor: int
    has_more: bool
    unread: int
    items: list[SyncItem]


class DeviceEvent(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str = Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9._:-]+$")
    type: Literal["read", "ack"]
    message_id: int = Field(ge=1)
    at: int | None = Field(default=None, ge=0, description="unix seconds when the user acted")


class DeviceEventsRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    ack_cursor: int | None = Field(default=None, ge=0)
    events: list[DeviceEvent] = Field(default_factory=list, max_length=50)
    status: dict[str, Any] = Field(default_factory=dict)

    @field_validator("status")
    @classmethod
    def _bounded_status(cls, v: dict[str, Any]) -> dict[str, Any]:
        if len(json.dumps(v).encode()) > 1024:
            raise ValueError("status exceeds 1024 bytes")
        return v


class DeviceEventsResponse(BaseModel):
    accepted: list[str]
    duplicates: list[str]
    unknown_messages: list[str]


class DeviceCreate(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: DeviceId
    name: str = Field(default="SmartPager", max_length=60)


class DeviceInfo(BaseModel):
    id: str
    name: str
    created_at: str
    last_seen_at: str | None
    last_ip: str | None
    firmware: str | None
    acked_cursor: int
    status: dict[str, Any]
    disabled: bool


class DeviceCreated(BaseModel):
    device: DeviceInfo
    token: str


class IngestTokenCreate(BaseModel):
    model_config = ConfigDict(extra="forbid")
    name: str = Field(min_length=2, max_length=40, pattern=r"^[A-Za-z0-9][A-Za-z0-9 ._-]+$")
    recipients: Recipients = None  # default `to` for messages sent with this token

    @field_validator("recipients")
    @classmethod
    def _dedupe(cls, v: list[str] | None) -> list[str] | None:
        return _unique_sorted(v)


class IngestTokenUpdate(BaseModel):
    model_config = ConfigDict(extra="forbid")
    recipients: Recipients = None  # null = every device

    @field_validator("recipients")
    @classmethod
    def _dedupe(cls, v: list[str] | None) -> list[str] | None:
        return _unique_sorted(v)


class IngestTokenInfo(BaseModel):
    id: int
    name: str
    created_at: str
    last_used_at: str | None
    revoked_at: str | None
    recipients: list[str] | None = None


class IngestTokenCreated(BaseModel):
    token_info: IngestTokenInfo
    token: str


class AuditEntry(BaseModel):
    id: int
    at: str
    actor: str
    action: str
    target: str
    ip: str | None


class FirmwareManifest(BaseModel):
    """Signed OTA manifest. The signature (ECDSA P-256 / SHA-256, DER, base64) covers
    the string "smartpager-ota|<version>|<size>|<sha256>" so the version can't be altered."""
    model_config = ConfigDict(extra="ignore")
    version: str = Field(pattern=r"^\d{1,3}\.\d{1,3}\.\d{1,3}$")
    size: int = Field(gt=0, le=6_500_000)
    sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    signature: str = Field(min_length=16, max_length=200)
