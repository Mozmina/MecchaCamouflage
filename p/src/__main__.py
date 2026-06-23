from __future__ import annotations

import traceback

from src.cli import main
from src.diagnostics import RuntimeDiagnostics


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SystemExit:
        raise
    except Exception as exc:  # noqa: BLE001
        diagnostics = RuntimeDiagnostics()
        diagnostics.record_error(
            stage="top_level_exception",
            message=str(exc),
            details={
                "exception": type(exc).__name__,
                "traceback": traceback.format_exc(),
            },
        )
        raise SystemExit(1)
