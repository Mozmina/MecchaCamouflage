# meccha-camouflage / p

`p` is the formal runtime for the Python-first MecchaCamouflage flow.

Goals:
- keep planning/execution logic in Python for faster edits and lightweight iteration
- allow the exe to run from any Windows folder
- detect `PenguinHotel-Win64-Shipping.exe` automatically
- keep runtime diagnostics useful from both the terminal and log files
- avoid UE4SS workflow coupling inside `p`

## Components

- `src/`  
  Pure Python runtime modules and CLI (`python -m src`).
- `adapters/`
  Runtime backends for local testing and external bridge compatibility.
- `native/`
  Repo-owned injector and injected bridge source for the Xenos-style runtime path.
- `scripts/`  
  Build and deploy scripts for local workflow.

## Standard workflow

From `p` directory:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File ./scripts/build_exe.ps1
```

Then place or run `dist\meccha-camouflage.exe` from any Windows folder.

Direct exe launch defaults to:

```text
--mode service --adapter xenos --game-process-name PenguinHotel-Win64-Shipping.exe
```

Runtime diagnostics are written to:

```text
%LOCALAPPDATA%\MecchaCamouflage\runtime\events.jsonl
%LOCALAPPDATA%\MecchaCamouflage\runtime\last_status.json
%LOCALAPPDATA%\MecchaCamouflage\runtime\runtime.log
```

WSL で `pwsh` がない場合は build script を次で実行します：

```bash
./scripts/dev_flow.sh -Action build
```

Service behavior:
- waits for `PenguinHotel-Win64-Shipping.exe`
- logs `waiting_for_process` while the game is missing
- attempts native bridge injection when native artifacts exist
- press `F10` to request one full route paint

Compatibility aliases:

- `scripts/build_dev.ps1` -> `scripts/build_exe.ps1`
- `scripts/install_dev_to_game.ps1` -> `scripts/deploy_to_game.ps1`

## Targeted steps

### build only

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File ./scripts/build_dev.ps1
```

### run only

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File ./scripts/dev_flow.ps1 `
  -Action run `
  -Adapter xenos `
  -RunForever 0 `
  -RuntimeArgString "--mode service --service-max-frames 20"
```

### package runtime zip

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File ./scripts/package_release.ps1 -Version 0.1.0
```

## service mode

`--mode service` keeps runtime alive and is suitable for long-running local profiling.

- trigger one apply: `--service-trigger-key f10`
- trigger from a bridge/file watcher: `--service-trigger-file path/to/trigger.txt`
- legacy continuous apply: `--service-apply-every-frame`
- target process override: `--game-process-name SomeGame-Win64-Shipping.exe`
- log directory override: `--log-dir C:\tmp\meccha-runtime-logs`
- stop signals: Ctrl+C / SIGTERM
- timeout with `--service-max-duration-seconds`
- frame count cap with `--service-max-frames`
- optional stop-file with `--service-stop-file`

## direct CLI

```powershell
python -m src --mode generate --print-summary --out-plan out/paint_plan.json
python -m src --mode apply --input-plan out/paint_plan.json --adapter noop --print-summary
python -m src --mode loop --loop-frames 20 --frame-delay-ms 16 --print-summary --timeline out/timeline.jsonl
python -m src --mode service --adapter xenos
python -m src --mode service --adapter xenos --bridge-path "127.0.0.1:8765" --bridge-transport tcp --bridge-wait-response
python -m src --mode service --adapter noop --service-trigger-file out/trigger.txt --log-dir out/logs
```

Note:
- Direct `meccha-camouflage.exe` launch writes terminal output and persistent logs.

## Notes

- `run_full_workflow.ps1` and `dev_flow.ps1` are thin wrappers around these scripts.
- For quick smoke tests, prefer `--quick` in loop mode instead of full frame count.
- If game/exe is locked during build, artifacts are staged as `dist/meccha-camouflage.pending.exe`.
