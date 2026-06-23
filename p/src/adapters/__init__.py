"""Adapter layer for paint execution backends."""

from .base import AdapterError, ApplyResult, PaintAdapter
from .noop import NoopAdapter
from .ue4ss_stub import Ue4ssStubAdapter
from .xenos_stub import XenosBridgeAdapter, XenosStubAdapter

__all__ = [
    "AdapterError",
    "ApplyResult",
    "PaintAdapter",
    "NoopAdapter",
    "Ue4ssStubAdapter",
    "XenosBridgeAdapter",
    "XenosStubAdapter",
]
