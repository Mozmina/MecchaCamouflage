from __future__ import annotations

import abc
from dataclasses import dataclass, field
from time import perf_counter
from typing import Any

from src.core.models import PaintPlan


@dataclass
class ApplyResult:
    adapter: str
    success: bool
    requested: int
    applied: int
    failures: int = 0
    message: str = ""
    duration_ms: float = 0.0
    timing_ms: dict[str, float] = field(default_factory=dict)
    metadata: dict[str, Any] = field(default_factory=dict)

    @property
    def summary(self) -> str:
        status = "ok" if self.success else "error"
        return f"{self.adapter}: {status} requested={self.requested} applied={self.applied} failures={self.failures}"


class AdapterError(RuntimeError):
    pass


class PaintAdapter(abc.ABC):
    name = "base"

    @abc.abstractmethod
    def apply(self, plan: PaintPlan) -> ApplyResult:
        raise NotImplementedError

    def _measure(self, fn) -> tuple[Any, float]:
        start = perf_counter()
        result = fn()
        return result, (perf_counter() - start) * 1000.0
