from __future__ import annotations

import json
from time import perf_counter
from datetime import datetime, timezone
from pathlib import Path

from src.adapters.base import ApplyResult, PaintAdapter
from src.core.models import PaintPlan
from src.plan_io import plan_to_dict


class Ue4ssStubAdapter(PaintAdapter):
    """Stub adapter that writes plan payload for external UE4SS bridge consumption."""

    name = "ue4ss"

    def __init__(self, queue_dir: str = "artifacts/ue4ss_stub") -> None:
        self.queue_dir = Path(queue_dir)

    def apply(self, plan: PaintPlan) -> ApplyResult:
        start = perf_counter()
        self.queue_dir.mkdir(parents=True, exist_ok=True)
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        queue_file = self.queue_dir / f"paint_plan_{timestamp}.json"

        t_dump_start = perf_counter()
        payload = {"timestamp_utc": timestamp}
        payload.update(plan_to_dict(plan))
        queue_file.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        t_dump = (perf_counter() - t_dump_start) * 1000.0
        duration_ms = (perf_counter() - start) * 1000.0

        # NOTE:
        # This is intentionally a local-file bridge placeholder.
        # Replace this with a named-pipe or dll-triggered integration.
        return ApplyResult(
            adapter=self.name,
            success=True,
            requested=plan.total_samples,
            applied=0,
            message="queued plan for UE4SS bridge",
            duration_ms=duration_ms,
            timing_ms={
                "serialize_ms": t_dump,
                "write_ms": duration_ms - t_dump,
                "apply_ms": 0.0,
            },
            metadata={"queue_file": str(queue_file), "status": "queued"},
        )
