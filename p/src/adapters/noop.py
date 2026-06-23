from __future__ import annotations

import json
import random
from pathlib import Path
from time import perf_counter

from src.adapters.base import ApplyResult, PaintAdapter
from src.core.models import PaintPlan
from src.plan_io import plan_to_dict


class NoopAdapter(PaintAdapter):
    """No-op adapter for local testing without game integration."""

    name = "noop"

    def __init__(self, out_dir: str = "out") -> None:
        self.out_dir = Path(out_dir)

    def apply(self, plan: PaintPlan) -> ApplyResult:
        start = perf_counter()
        self.out_dir.mkdir(parents=True, exist_ok=True)
        preview = self.out_dir / "plan_preview.txt"
        preview_json = self.out_dir / "plan_preview.json"

        lines = [
            f"samples={plan.total_samples}",
            f"front={len(plan.front_samples)}",
            f"side={len(plan.side_samples)}",
            f"back={len(plan.back_samples)}",
        ]
        preview.write_text("\n".join(lines) + "\n", encoding="utf-8")

        payload = {
            "summary": lines,
            "plan": plan_to_dict(plan),
            "metadata": {
                "request_id": f"noop-{random.randint(100000, 999999)}",
                "requesting_adapter": self.name,
            },
        }
        serialize_start = perf_counter()
        payload_json = json.dumps(payload, ensure_ascii=False)
        t_serialize_ms = (perf_counter() - serialize_start) * 1000.0

        write_start = perf_counter()
        preview_json.write_text(payload_json, encoding="utf-8")
        t_write_ms = (perf_counter() - write_start) * 1000.0
        duration_ms = (perf_counter() - start) * 1000.0

        return ApplyResult(
            adapter=self.name,
            success=True,
            requested=plan.total_samples,
            applied=plan.total_samples,
            message="noop execution",
            duration_ms=duration_ms,
            timing_ms={
                "serialize_ms": t_serialize_ms,
                "write_ms": t_write_ms,
                "apply_ms": 0.0,
            },
            metadata={
                "preview": str(preview),
                "preview_json": str(preview_json),
            },
        )
