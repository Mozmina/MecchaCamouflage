from __future__ import annotations

import socket
import subprocess
from dataclasses import dataclass
from pathlib import Path
from time import perf_counter
from typing import Any

from src.protocol import BridgeResponse, decode_response_line, encode_request_line, make_request


DEFAULT_BRIDGE_HOST = "127.0.0.1"
DEFAULT_BRIDGE_PORT = 47654


@dataclass(frozen=True)
class NativeAssets:
    root: Path
    injector: Path
    bridge_dll: Path

    def to_status(self) -> dict[str, Any]:
        return {
            "root": str(self.root),
            "injector": str(self.injector),
            "bridge_dll": str(self.bridge_dll),
            "injector_exists": self.injector.exists(),
            "bridge_dll_exists": self.bridge_dll.exists(),
        }


def runtime_root() -> Path:
    return Path(__file__).resolve().parents[1]


def native_assets(root: Path | None = None) -> NativeAssets:
    base = root or runtime_root()
    native_root = base / "native" / "bin"
    return NativeAssets(
        root=native_root,
        injector=native_root / "meccha-xenos-injector.exe",
        bridge_dll=native_root / "meccha-xenos-bridge.dll",
    )


def choose_bridge_port(requested_port: int, host: str = DEFAULT_BRIDGE_HOST) -> int:
    port = int(requested_port)
    if port > 0:
        return port
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return int(sock.getsockname()[1])


def bridge_port_file(assets: NativeAssets) -> Path:
    return Path(str(assets.bridge_dll) + ".port")


def write_bridge_port_file(assets: NativeAssets, port: int) -> Path:
    path = bridge_port_file(assets)
    path.write_text(f"{int(port)}\n", encoding="ascii")
    return path


class NativeBridgeClient:
    def __init__(self, host: str = DEFAULT_BRIDGE_HOST, port: int = DEFAULT_BRIDGE_PORT, timeout_seconds: float = 240.0) -> None:
        self.host = host
        self.port = int(port)
        self.timeout_seconds = max(0.1, float(timeout_seconds))

    @property
    def address(self) -> str:
        return f"{self.host}:{self.port}"

    def request(self, command: str, payload: dict[str, Any] | None = None) -> tuple[BridgeResponse, float]:
        req = make_request(command, payload)
        start = perf_counter()
        with socket.create_connection((self.host, self.port), timeout=self.timeout_seconds) as sock:
            sock.sendall(encode_request_line(req))
            sock.settimeout(self.timeout_seconds)
            raw = b""
            while b"\n" not in raw:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                raw += chunk
        elapsed_ms = (perf_counter() - start) * 1000.0
        if not raw:
            return BridgeResponse(False, command, failures=1, message="empty bridge response"), elapsed_ms
        return decode_response_line(raw.split(b"\n", 1)[0]), elapsed_ms

    def ping(self) -> tuple[BridgeResponse, float]:
        return self.request("ping", {})

    def capabilities(self) -> tuple[BridgeResponse, float]:
        return self.request("capabilities", {})

    def sdk_probe(self) -> tuple[BridgeResponse, float]:
        return self.request("sdk_probe", {})

    def paint_full_route(self, payload: dict[str, Any]) -> tuple[BridgeResponse, float]:
        return self.request("paint_full_route", payload)

    def shutdown(self) -> tuple[BridgeResponse, float]:
        return self.request("shutdown", {})


def inject_bridge(
    process_name: str,
    assets: NativeAssets,
    timeout_seconds: float = 10.0,
    bridge_port: int | None = None,
) -> tuple[bool, dict[str, Any]]:
    if not assets.injector.exists() or not assets.bridge_dll.exists():
        return False, {
            "stage": "native_assets_missing",
            "injector": str(assets.injector),
            "bridge_dll": str(assets.bridge_dll),
        }
    port_file = None
    if bridge_port is not None:
        try:
            port_file = write_bridge_port_file(assets, bridge_port)
        except OSError as exc:
            return False, {
                "stage": "bridge_port_write_failed",
                "message": str(exc),
                "win32_error": getattr(exc, "winerror", None),
                "bridge_port": int(bridge_port),
                "bridge_dll": str(assets.bridge_dll),
            }
    try:
        result = subprocess.run(
            [str(assets.injector), process_name, str(assets.bridge_dll)],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=max(1.0, timeout_seconds),
        )
    except OSError as exc:
        return False, {
            "stage": "injector_launch_failed",
            "message": str(exc),
            "win32_error": getattr(exc, "winerror", None),
        }
    except subprocess.TimeoutExpired as exc:
        return False, {
            "stage": "injector_timeout",
            "message": str(exc),
            "stdout": exc.stdout or "",
            "stderr": exc.stderr or "",
        }
    return result.returncode == 0, {
        "stage": "injector_exit",
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "bridge_port": int(bridge_port) if bridge_port is not None else None,
        "port_file": str(port_file) if port_file is not None else None,
    }
