from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from typing import Any
from uuid import uuid4


@dataclass(frozen=True)
class BridgeRequest:
    type: str
    request_id: str
    payload: dict[str, Any] = field(default_factory=dict)
    timestamp_utc: int = 0

    def to_dict(self) -> dict[str, Any]:
        return {
            "type": self.type,
            "request_id": self.request_id,
            "timestamp_utc": self.timestamp_utc or int(time.time()),
            "payload": self.payload,
        }


@dataclass(frozen=True)
class BridgeResponse:
    success: bool
    stage: str
    applied: int = 0
    failures: int = 0
    message: str = ""
    timing_ms: dict[str, float] = field(default_factory=dict)
    metadata: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "success": self.success,
            "stage": self.stage,
            "applied": self.applied,
            "failures": self.failures,
            "message": self.message,
            "timing_ms": self.timing_ms,
            "metadata": self.metadata,
        }


def make_request(command: str, payload: dict[str, Any] | None = None, request_id: str | None = None) -> BridgeRequest:
    return BridgeRequest(
        type=command,
        request_id=request_id or uuid4().hex,
        payload=payload or {},
        timestamp_utc=int(time.time()),
    )


def encode_request_line(request: BridgeRequest) -> bytes:
    return (json.dumps(request.to_dict(), ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def decode_request_line(line: bytes | str) -> BridgeRequest:
    text = line.decode("utf-8", errors="replace") if isinstance(line, bytes) else line
    data = json.loads(text)
    return BridgeRequest(
        type=str(data["type"]),
        request_id=str(data["request_id"]),
        timestamp_utc=int(data.get("timestamp_utc", 0)),
        payload=dict(data.get("payload") or {}),
    )


def encode_response_line(response: BridgeResponse) -> bytes:
    return (json.dumps(response.to_dict(), ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def decode_response_line(line: bytes | str) -> BridgeResponse:
    text = line.decode("utf-8", errors="replace") if isinstance(line, bytes) else line
    data = json.loads(text)
    return BridgeResponse(
        success=bool(data.get("success", False)),
        stage=str(data.get("stage", "")),
        applied=int(data.get("applied", 0)),
        failures=int(data.get("failures", 0)),
        message=str(data.get("message", "")),
        timing_ms={key: float(value) for key, value in dict(data.get("timing_ms") or {}).items()},
        metadata=dict(data.get("metadata") or {}),
    )
