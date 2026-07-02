# Research Tools

This project does not vendor game-research tool source by default. Keep these
tools as local checkouts under `third_party/` when a game update requires asset,
mapping, or SDK investigation.

The runtime build only requires the tracked `third_party/imgui` source. The
tools below are for update recovery and profile regeneration, not normal app
builds.

## CUE4Parse

- Upstream: https://github.com/FabianFG/CUE4Parse
- Purpose: inspect cooked Unreal assets and regenerate mesh/profile data after
  a MECCHA CHAMELEON update.
- Local path expected by research scripts: `third_party/CUE4Parse`

Setup:

```bash
mkdir -p third_party
git clone https://github.com/FabianFG/CUE4Parse third_party/CUE4Parse
```

If the checkout already exists elsewhere, a local symlink or junction is fine.
Do not commit that symlink; it is machine-specific.

## UnrealMappingsDumper

- Upstream: https://github.com/TheNaeem/UnrealMappingsDumper
- Reference commit used during current investigation:
  `4da8c66c23ce66ef86d75962d66b12cf39185092`
- License: MIT
- Purpose: generate or inspect UE mappings when reflected property offsets,
  `FProperty` layouts, or runtime SDK assumptions break after a game update.
- Local path: `third_party/UnrealMappingsDumper`

Setup:

```bash
mkdir -p third_party
git clone https://github.com/TheNaeem/UnrealMappingsDumper third_party/UnrealMappingsDumper
```

Build from source. Do not rely on upstream prebuilt DLLs for this project; local
crash guards and logging are often needed when offsets are wrong.

## Update Workflow

When a game update breaks painting, use this order:

1. Run the app once and keep `%LOCALAPPDATA%\MecchaCamouflage\runtime\` logs.
2. Check runtime reflection metadata first: function availability, reflected
   offsets, `RuntimePaintable` state, and mesh profile identity.
3. Use UnrealMappingsDumper only when runtime reflection cannot resolve a
   trustworthy layout or the engine-side mapping changed.
4. Use CUE4Parse to inspect cooked mesh assets and regenerate mesh profiles.
5. Copy regenerated shipping profiles into `.build/bin/mesh-profiles/` before a
   release package. `scripts/release.ps1` includes that directory when present.

Commit only reviewed project artifacts:

- source changes under `runtime/`, `scripts/`, or docs
- small regenerated profile JSON files that are intended to ship
- pinned patch files or instructions if a research tool needs local changes

Do not commit:

- game content (`.pak`, `.ucas`, `.utoc`, `.sig`)
- generated mappings (`*.usmap`)
- research logs, dumps, injected DLLs, or build output
- local third-party checkouts or symlinks under `third_party/CUE4Parse` and
  `third_party/UnrealMappingsDumper`
