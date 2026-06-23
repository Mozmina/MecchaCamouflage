from __future__ import annotations

import csv
import os
import platform
import subprocess
from dataclasses import dataclass
from io import StringIO
from pathlib import Path


DEFAULT_GAME_PROCESS_NAME = "PenguinHotel-Win64-Shipping.exe"


@dataclass(frozen=True)
class ProcessInfo:
    pid: int
    name: str
    session_name: str = ""
    memory_usage: str = ""
    source: str = "tasklist"

    def to_status(self) -> dict[str, object]:
        return {
            "pid": self.pid,
            "name": self.name,
            "session_name": self.session_name,
            "memory_usage": self.memory_usage,
            "source": self.source,
        }


def normalize_process_name(name: str) -> str:
    text = name.strip().strip('"').replace("\\", "/")
    return Path(text).name.lower()


def process_name_matches(candidate: str, expected: str) -> bool:
    return normalize_process_name(candidate) == normalize_process_name(expected)


def find_process_by_name(name: str = DEFAULT_GAME_PROCESS_NAME) -> ProcessInfo | None:
    if os.name == "nt":
        return _find_windows_process(name)
    return _find_procfs_process(name)


def _find_windows_process(name: str) -> ProcessInfo | None:
    expected = normalize_process_name(name)
    try:
        result = subprocess.run(
            ["tasklist", "/fo", "csv", "/nh"],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    reader = csv.reader(StringIO(result.stdout))
    for row in reader:
        if len(row) < 2:
            continue
        image_name = row[0]
        if normalize_process_name(image_name) != expected:
            continue
        try:
            pid = int(row[1])
        except ValueError:
            continue
        return ProcessInfo(
            pid=pid,
            name=image_name,
            session_name=row[2] if len(row) > 2 else "",
            memory_usage=row[4] if len(row) > 4 else "",
        )
    return None


def _find_procfs_process(name: str) -> ProcessInfo | None:
    expected = normalize_process_name(name)
    proc = Path("/proc")
    if not proc.exists():
        return None
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        comm = entry / "comm"
        try:
            proc_name = comm.read_text(encoding="utf-8", errors="replace").strip()
        except OSError:
            continue
        if normalize_process_name(proc_name) != expected:
            continue
        return ProcessInfo(pid=int(entry.name), name=proc_name, source=f"procfs:{platform.system()}")
    return None
