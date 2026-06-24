<p align="center">
  <img src="assets/meccha-camouflage-banner.png" alt="Meccha Camouflage banner" width="100%" />
</p>

# Meccha Camouflage Runtime

This repository is centered on the Xenos-injected native runtime.

- `native/`: C++ controller, injected bridge, and SDK-backed paint runtime.
- `scripts/`: build, deploy, package, and SDK dump workflows.
- `dumper-sdk/`: managed Dumper7 SDK output for the target game build.
- `tools/Dumper-7/`: local Dumper-7 tool source.

The runtime release artifact is a single `meccha-camouflage.exe`. The bridge DLL is embedded in that EXE and extracted under `%LOCALAPPDATA%\MecchaCamouflage\runtime\native\` at startup before injection.

## Build

From the repository root:

```bash
./scripts/dev_flow.sh -Action build
```

Build output is written under `.build/`:

```text
.build/
  native/bin/meccha-camouflage.exe       # controller/runtime EXE
  native/bin/meccha-xenos-bridge.dll     # embedded into the controller at build time
  native/bin/meccha-xenos-injector.exe   # development helper for SDK tooling
  native/obj/                            # native object/resource files
```

## Deploy

```bash
./scripts/dev_flow.sh -Action deploy -GameRoot 'C:\Program Files (x86)\Steam\steamapps\common\MECCHA CHAMELEON'
```

The deploy script installs `.build/native/bin/meccha-camouflage.exe` into:

```text
C:\Program Files (x86)\Steam\steamapps\common\MECCHA CHAMELEON\Chameleon\Binaries\Win64
```

If the target EXE is locked, deploy stages a `.pending.exe` and starts the replacement watcher.

## Run

The default runtime mode is the native Xenos service path:

```bash
./scripts/dev_flow.sh -Action run
```

Useful direct modes:

```bash
.build/native/bin/meccha-camouflage.exe --mode service
.build/native/bin/meccha-camouflage.exe --mode probe
.build/native/bin/meccha-camouflage.exe --mode apply --native-apply-mode texture_sync_strict_probe
.build/native/bin/meccha-camouflage.exe --mode shutdown
```

Runtime diagnostics are written to:

```text
%LOCALAPPDATA%\MecchaCamouflage\runtime\events.jsonl
%LOCALAPPDATA%\MecchaCamouflage\runtime\last_status.json
%LOCALAPPDATA%\MecchaCamouflage\runtime\runtime.log
```

## Route policy

Current route policy is documented in `native/README.md`.

High-level rules:

- Active multiplayer candidates must go through Xenos/native SDK routes.
- Local-only texture import is not a default runtime path.
- Material swap, synthetic UV placement, and memory-scan fallback are forbidden.
- Paint and texture quality changes are intentionally separate from runtime/controller refactors.
