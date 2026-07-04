# v1.5.0 Refactor Audit

This file is the implementation checklist and architecture record for the v1.5.0 WPF refactor.

## Goals

- Keep the C++ bridge DLL and injector as the game-facing layer.
- Move the desktop controller GUI to C# WPF targeting `net10.0-windows`.
- Add region modes: `paint`, `fill`, `skip`.
- Add Fill Material with default sRGB `#FFFFFF`, metallic `1.0`, roughness `0.0`.
- Add GUI-only localization with English as the default language.
- Keep logs and runtime diagnostics in English.
- Consolidate per-version config, logs, runtime binaries, progress files, and debug artifacts.

## Phase Checklist

- [x] Phase 1: Define C# core contracts, settings schema, bridge payload schema, and runtime folder layout.
- [x] Phase 2: Add bridge compatibility for region modes and fill material.
- [x] Phase 3: Add WPF controller shell with responsive layout, process/service/bridge status, settings, hotkeys, and logs.
- [x] Phase 4: Add Fill UI and 16 GUI locales.
- [~] Phase 5: Polish build/package/runtime artifacts, reduce release log noise, and remove obsolete UI paths after parity.

Phase 5 status:

- [x] Build WPF as self-contained `win-x64`.
- [x] Package native bridge/injector under `native\`.
- [x] Package mesh profiles under `mesh-profiles\`.
- [x] Stop compiling the C++/ImGui controller in `scripts/build.ps1`.
- [x] Remove the `Using application font: Arial` build path.
- [x] Remove stale `runtime\bin\<hash>` folders by age/retention on startup.
- [x] Move bridge progress JSON from DLL sidecar into `runtime\progress\` for the WPF path, with sidecar fallback for old callers.
- [ ] Delete C++/ImGui controller sources after WPF parity is verified in game.
- [x] Remove unused font archives after no release path references them.
- [ ] Further reduce bridge trace/log verbosity after multiplayer smoke testing.

## Settings Schema

Current config path:

`%LOCALAPPDATA%\MecchaCamouflage\versions\<version>\config\config.json`

Legacy config path still accepted for migration:

`%LOCALAPPDATA%\MecchaCamouflage\versions\<version>\config.json`

Important fields:

- `layout_version`: current WPF schema version.
- `language`: one of `en`, `ja`, `ko`, `es`, `fr`, `de`, `pt-BR`, `zh-Hans`, `zh-Hant`, `ru`, `pl`, `tr`, `id`, `it`, `nl`, `vi`.
- `front_region_mode`, `side_region_mode`, `back_region_mode`: `paint`, `fill`, or `skip`.
- `fill_color`: sRGB hex color, default `#FFFFFF`.
- `fill_metallic`: `0.0` to `1.0`, default `1.0`.
- `fill_roughness`: `0.0` to `1.0`, default `0.0`.
- `auto_material_properties`: kept for compatibility; GUI label is `Auto Material`.

Legacy region migration:

- `enable_*_paint: true` maps to `<region>_region_mode: paint`.
- `enable_*_paint: false` maps to `<region>_region_mode: fill`.
- Missing legacy fields default to `paint`.

## Bridge Payload Contract

The WPF controller sends JSON lines to the bridge TCP server.

Required paint command:

- `type`: `paint_full_route`
- `native_apply_mode`: `mesh_first_paint`
- `route`: `f10_mesh_first_paint`
- `preview_only`: boolean
- `unpreview_only`: boolean
- `front_region_mode`, `side_region_mode`, `back_region_mode`: `paint`, `fill`, `skip`
- `enable_front_paint`, `enable_side_paint`, `enable_back_paint`: compatibility booleans, true only for `paint`
- `fill_color_r`, `fill_color_g`, `fill_color_b`: sRGB unit values
- `fill_metallic`, `fill_roughness`: unit values
- paint tuning fields: brush size, coverage step, stroke delay, Auto Material, metallic, roughness

## Runtime Layout

Per version root:

`%LOCALAPPDATA%\MecchaCamouflage\versions\<version>\`

Subdirectories:

- `config\`
- `logs\`
- `runtime\bin\<hash>\`
- `runtime\progress\`
- `debug\` only for research/debug artifacts

The WPF controller copies packaged native files into `runtime\bin\<hash>\`, writes `<bridge>.dll.port` and `<bridge>.dll.progress.path`, injects the bridge DLL, and sends commands over localhost. The bridge writes progress into `runtime\progress\` when `.progress.path` is present and falls back to `<bridge>.dll.progress.json` for older callers.

## Cleanup Targets

- Remove or gate noisy trace/debug artifacts in release mode.
- Remove stale `runtime\bin\<hash>` folders by age/hash on startup.
- Remove unused bundled fonts and the `Using application font` build message.
- Keep C++ ImGui controller source until WPF parity is verified, then delete it in a separate cleanup commit.
