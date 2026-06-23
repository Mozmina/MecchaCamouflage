from __future__ import annotations

import json
import socket
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from time import perf_counter
from typing import Any
from uuid import uuid4

from src.adapters.base import ApplyResult, PaintAdapter
from src.core.models import PaintPlan
from src.plan_io import plan_to_dict
from src.protocol import make_request


@dataclass
class _BridgeResponse:
    success: bool
    message: str
    applied: int
    failures: int
    metadata: dict[str, Any]


class XenosBridgeAdapter(PaintAdapter):
    """Adapter target for a xenos-style external bridge.

    Supports:
    - HTTP endpoint (http://host:port/path)
    - TCP endpoint (host:port)
    - file-based queue directory or file
    """

    name = "xenos"

    def __init__(
        self,
        bridge_path: str | None = None,
        timeout_seconds: float = 5.0,
        bridge_transport: str = "auto",
        wait_for_response: bool = False,
        max_retries: int = 0,
        retry_delay_ms: float = 250.0,
    ) -> None:
        self.bridge_path = bridge_path
        self.bridge_transport = bridge_transport
        self.timeout_seconds = max(0.1, float(timeout_seconds))
        self.wait_for_response = wait_for_response
        self.max_retries = max(0, int(max_retries))
        self.retry_delay_ms = max(0.0, float(retry_delay_ms))
        self._resolved_mode: str | None = None

    def _infer_transport(self) -> str:
        if self._resolved_mode:
            return self._resolved_mode

        if not self.bridge_path:
            return "missing"

        text = str(self.bridge_path)
        if text.startswith("http://") or text.startswith("https://"):
            self._resolved_mode = "http"
            return "http"

        # Keep URL handling first; if bridge path looks like a file path and exists,
        # file transport wins by default.
        if self.bridge_transport == "tcp":
            self._resolved_mode = "tcp"
            return "tcp"

        bridge_path = Path(text)
        if bridge_path.exists():
            self._resolved_mode = "file"
            return "file"

        if ":" in text:
            host, _, _ = text.partition(":")
            if host and _ and not (len(host) == 1 and host.isalpha()):
                self._resolved_mode = "tcp"
                return "tcp"

        if self.bridge_transport != "auto":
            self._resolved_mode = self.bridge_transport
            return self.bridge_transport

        self._resolved_mode = "file"
        return "file"

    def _build_payload(self, plan: PaintPlan) -> dict[str, Any]:
        request = make_request(
            "paint_full_route",
            {
                "route": "f10_full_body_metallic_then_front",
                "plan": plan_to_dict(plan),
            },
            request_id=uuid4().hex,
        )
        return request.to_dict()

    def _http_send(self, payload: dict[str, Any]) -> _BridgeResponse:
        endpoint = str(self.bridge_path)
        if not endpoint:
            raise RuntimeError("http transport needs a URL as bridge path")
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(
            endpoint,
            data=data,
            method="POST",
            headers={
                "content-type": "application/json",
                "accept": "application/json",
                "user-agent": "meccha-camouflage-xenos/1.0",
            },
        )
        with urllib.request.urlopen(req, timeout=self.timeout_seconds) as response:
            body = response.read().decode("utf-8", errors="replace")
            status = getattr(response, "status", 200)
            if status < 200 or status >= 300:
                raise RuntimeError(f"bridge responded with status {status}")
            try:
                decoded = json.loads(body) if body else {}
            except Exception:
                decoded = {"message": body}

        success = bool(decoded.get("success", True))
        message = str(decoded.get("message", "ok"))
        applied = int(decoded.get("applied", 0))
        failures = int(decoded.get("failures", 0))
        return _BridgeResponse(
            success=success,
            message=message,
            applied=applied,
            failures=failures,
            metadata={"http_status": status, "http_response": decoded},
        )

    def _tcp_send(self, payload: dict[str, Any]) -> _BridgeResponse:
        if not self.bridge_path:
            raise RuntimeError("tcp transport needs a bridge path in host:port format")
        text = str(self.bridge_path)
        host, colon, port_text = text.partition(":")
        if not colon or not port_text:
            raise RuntimeError(f"invalid tcp bridge path: {text}")
        try:
            port = int(port_text)
        except ValueError as exc:
            raise RuntimeError(f"invalid tcp port in bridge path: {text}") from exc

        message = json.dumps(payload, ensure_ascii=False)
        data = (message + "\n").encode("utf-8")
        timeout = self.timeout_seconds
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.sendall(data)
            if not self.wait_for_response:
                plan_payload = payload["payload"]
                estimated = len(plan_payload.get("front_samples", []))
                estimated += len(plan_payload.get("side_samples", []))
                estimated += len(plan_payload.get("back_samples", []))
                return _BridgeResponse(
                    success=True,
                    message="sent",
                    applied=estimated,
                    failures=0,
                    metadata={"tcp": f"{host}:{port}", "payload_length": len(data)},
                )

            sock.settimeout(timeout)
            resp_bytes = b""
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                resp_bytes += chunk
                if b"\n" in resp_bytes:
                    break
            if not resp_bytes:
                raise RuntimeError("bridge returned empty response")
            line = resp_bytes.split(b"\n", 1)[0].decode("utf-8", errors="replace")
            decoded = json.loads(line)

        success = bool(decoded.get("success", False))
        message = str(decoded.get("message", "ok"))
        return _BridgeResponse(
            success=success,
            message=message,
            applied=int(decoded.get("applied", 0)),
            failures=int(decoded.get("failures", 0)),
            metadata={"tcp": f"{host}:{port}", "raw_response": line},
        )

    def _file_send(self, payload: dict[str, Any], request_id: str) -> _BridgeResponse:
        if not self.bridge_path:
            raise RuntimeError("file transport needs a valid path")
        bridge = Path(self.bridge_path)
        if bridge.is_dir():
            request_path = bridge / "in" / f"{request_id}.json"
            response_path = bridge / "out" / f"{request_id}.json"
            request_path.parent.mkdir(parents=True, exist_ok=True)
            response_path.parent.mkdir(parents=True, exist_ok=True)
        else:
            request_path = bridge
            request_path.parent.mkdir(parents=True, exist_ok=True)
            response_path = request_path.with_suffix(".response.json")

        tmp_path = request_path.with_suffix(".tmp")
        request_path.parent.mkdir(parents=True, exist_ok=True)
        tmp_path.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
        tmp_path.replace(request_path)

        if not self.wait_for_response:
            return _BridgeResponse(
                success=True,
                message="queued in xenos file transport",
                applied=0,
                failures=0,
                metadata={
                    "file_request": str(request_path),
                    "request_id": request_id,
                    "request_bytes": request_path.stat().st_size,
                },
            )

        deadline = perf_counter() + self.timeout_seconds
        while perf_counter() < deadline:
            if response_path.exists():
                try:
                    data = json.loads(response_path.read_text(encoding="utf-8"))
                    return _BridgeResponse(
                        success=bool(data.get("success", False)),
                        message=str(data.get("message", "")),
                        applied=int(data.get("applied", 0)),
                        failures=int(data.get("failures", 0)),
                        metadata={
                            "file_request": str(request_path),
                            "file_response": str(response_path),
                            "request_id": request_id,
                        },
                    )
                except (OSError, ValueError, TypeError):
                    return _BridgeResponse(
                        success=False,
                        message="failed to parse response payload",
                        applied=0,
                        failures=1,
                        metadata={
                            "file_request": str(request_path),
                            "file_response": str(response_path),
                            "request_id": request_id,
                        },
                    )
            time.sleep(0.05)

        return _BridgeResponse(
            success=False,
            message=f"file response timeout ({self.timeout_seconds:.2f}s)",
            applied=0,
            failures=1,
            metadata={
                "file_request": str(request_path),
                "file_response": str(response_path),
                "request_id": request_id,
                "timeout_seconds": self.timeout_seconds,
            },
        )

    def _send_payload(self, payload: dict[str, Any]) -> _BridgeResponse:
        mode = self._infer_transport()
        if mode == "missing":
            raise RuntimeError("bridge_path not configured")
        if mode == "http":
            return self._http_send(payload)
        if mode == "tcp":
            return self._tcp_send(payload)
        if mode == "file":
            return self._file_send(payload, payload["request_id"])
        raise RuntimeError(f"unsupported transport: {mode}")

    def _attempt_send(self, payload: dict[str, Any]) -> _BridgeResponse:
        retries_left = self.max_retries + 1
        last_err: str | None = None
        while retries_left > 0:
            try:
                return self._send_payload(payload)
            except Exception as exc:  # noqa: BLE001
                last_err = str(exc)
                retries_left -= 1
                if retries_left <= 0:
                    break
                if self.retry_delay_ms > 0:
                    time.sleep(self.retry_delay_ms / 1000.0)

        raise RuntimeError(last_err or "xenos bridge send failed")

    def apply(self, plan: PaintPlan) -> ApplyResult:
        if not self.bridge_path:
            return ApplyResult(
                adapter=self.name,
                success=False,
                requested=plan.total_samples,
                applied=0,
                failures=1,
                message="bridge_path not configured",
                duration_ms=0.0,
                timing_ms={"serialize_ms": 0.0, "send_ms": 0.0, "response_ms": 0.0},
                metadata={"requires": "bridge_path"},
            )

        payload_start = perf_counter()
        payload = self._build_payload(plan)
        t_serialize_ms = (perf_counter() - payload_start) * 1000.0

        send_start = perf_counter()
        try:
            result = self._attempt_send(payload)
            send_ms = (perf_counter() - send_start) * 1000.0
            total_ms = t_serialize_ms + send_ms
            return ApplyResult(
                adapter=self.name,
                success=result.success,
                requested=plan.total_samples,
                applied=result.applied,
                failures=result.failures,
                message=result.message,
                duration_ms=total_ms,
                timing_ms={
                    "serialize_ms": t_serialize_ms,
                    "send_ms": send_ms,
                },
                metadata=dict(result.metadata),
            )
        except Exception as exc:
            send_ms = (perf_counter() - send_start) * 1000.0
            return ApplyResult(
                adapter=self.name,
                success=False,
                requested=plan.total_samples,
                applied=0,
                failures=1,
                message=str(exc),
                duration_ms=t_serialize_ms + send_ms,
                timing_ms={
                    "serialize_ms": t_serialize_ms,
                    "send_ms": send_ms,
                    "response_ms": 0.0,
                },
                metadata={"bridge_error": str(exc)},
            )


XenosStubAdapter = XenosBridgeAdapter
