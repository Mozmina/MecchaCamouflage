from __future__ import annotations

import json
import os
import sys
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def default_log_dir() -> Path:
    root = os.environ.get("LOCALAPPDATA")
    if root:
        return Path(root) / "MecchaCamouflage" / "runtime"
    return Path.home() / ".meccha-camouflage" / "runtime"


@dataclass
class RuntimeStatus:
    process: dict[str, Any] = field(default_factory=dict)
    bridge: dict[str, Any] = field(default_factory=dict)
    hotkey: dict[str, Any] = field(default_factory=dict)
    last_run: dict[str, Any] = field(default_factory=dict)
    last_error: dict[str, Any] | None = None
    log_path: str = ""


class RuntimeDiagnostics:
    def __init__(self, log_dir: str | Path | None = None, console: bool = True) -> None:
        self.log_dir = Path(log_dir) if log_dir else default_log_dir()
        self.console = console
        self.events_path = self.log_dir / "events.jsonl"
        self.status_path = self.log_dir / "last_status.json"
        self.runtime_log_path = self.log_dir / "runtime.log"
        self.status = RuntimeStatus(log_path=str(self.log_dir))
        self.log_dir.mkdir(parents=True, exist_ok=True)

    def event(
        self,
        event: str,
        *,
        level: str = "info",
        stage: str = "",
        message: str = "",
        run_id: str = "",
        details: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        entry = {
            "timestamp": utc_timestamp(),
            "level": level,
            "event": event,
            "run_id": run_id,
            "stage": stage,
            "message": message,
            "details": details or {},
        }
        self._append_jsonl(entry)
        self._append_text(entry)
        if self.console:
            self._print_console(entry)
        return entry

    def update_status(self, **updates: Any) -> None:
        for key, value in updates.items():
            if not hasattr(self.status, key):
                continue
            setattr(self.status, key, value)
        self.write_status()

    def merge_status(self, **updates: dict[str, Any]) -> None:
        for key, value in updates.items():
            if not hasattr(self.status, key):
                continue
            current = getattr(self.status, key)
            if isinstance(current, dict):
                current.update(value)
            else:
                setattr(self.status, key, value)
        self.write_status()

    def record_error(
        self,
        *,
        stage: str,
        message: str,
        details: dict[str, Any] | None = None,
        run_id: str = "",
    ) -> None:
        error = {
            "timestamp": utc_timestamp(),
            "stage": stage,
            "message": message,
            "details": details or {},
            "run_id": run_id,
        }
        self.status.last_error = error
        self.write_status()
        self.event("runtime_error", level="error", stage=stage, message=message, run_id=run_id, details=details)

    def write_status(self) -> None:
        self.log_dir.mkdir(parents=True, exist_ok=True)
        tmp = self.status_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(asdict(self.status), indent=2, ensure_ascii=False), encoding="utf-8")
        tmp.replace(self.status_path)

    def _append_jsonl(self, entry: dict[str, Any]) -> None:
        self.log_dir.mkdir(parents=True, exist_ok=True)
        with self.events_path.open("a", encoding="utf-8") as file:
            file.write(json.dumps(entry, ensure_ascii=False, separators=(",", ":")))
            file.write("\n")

    def _append_text(self, entry: dict[str, Any]) -> None:
        text = (
            f"{entry['timestamp']} {entry['level'].upper()} {entry['event']} "
            f"stage={entry['stage']} run_id={entry['run_id']} {entry['message']}"
        ).rstrip()
        with self.runtime_log_path.open("a", encoding="utf-8") as file:
            file.write(text)
            details = entry.get("details") or {}
            if details:
                file.write(" ")
                file.write(json.dumps(details, ensure_ascii=False, sort_keys=True))
            file.write("\n")

    def _print_console(self, entry: dict[str, Any]) -> None:
        message = (
            f"[{entry['timestamp']}] {entry['level'].upper()} {entry['event']} "
            f"stage={entry['stage']} {entry['message']}"
        ).rstrip()
        print(message, flush=True)
        if entry.get("level") in {"warning", "error"} and entry.get("details"):
            print(
                "  details="
                + json.dumps(entry["details"], ensure_ascii=False, sort_keys=True),
                file=sys.stderr,
                flush=True,
            )
        if entry.get("level") == "error":
            print(f"  log_dir={self.log_dir}", file=sys.stderr, flush=True)
