# MecchaCamouflage Native Bridge

This directory is owned by the `p` runtime.

- `src/meccha_xenos_bridge.cpp`: injected bridge DLL source.
- `src/meccha_xenos_injector.cpp`: process injector source.
- `bin/`: expected compiled artifacts bundled by PyInstaller.

The current bridge implements the newline JSON protocol surface:

- `ping`
- `capabilities`
- `paint_full_route`

`paint_full_route` is currently switchable between the release-oriented replicated paint route and temporary diagnostic routes.

## Temporary diagnostic route

`texture_import_diagnostic` is a temporary F10 default used only to isolate atlas/capture/UV alignment from replicated paint API behavior.

- It may call `ExportChannelToBytes` and `ImportChannelFromBytes` for albedo-only diagnostics.
- It must not call `ServerPaintBatch`, `PaintAtUVWithBrush`, metallic base paint, local echo, material swap, or texture/material replacement.
- It is not a release runtime route and must not be treated as multiplayer-safe success.
- It must emit `temporary_diagnostic_only=true` and `replicated_paint_used=false` in metadata.
- Success only means `after_hash == atlas_hash` and `after_hash != before_hash`, proving the CPU atlas was imported locally for visual inspection.

## Forbidden implementation paths

These paths are not valid release-runtime solutions for `p/native`:

- Material swap. It changes assets/material references instead of using the game's paint system, so it is not an acceptable fallback.
- `ImportChannelFromBytes` or direct texture import as F10 success. It can create local texture state but does not prove multiplayer-safe replicated paint.
- Local-only echo as success. `PaintAtUVWithBrush` may be useful for diagnostics, but F10 success requires replicated server paint RPC.
- Relay-only routing without parity evidence. The UE4SS reference path uses `RuntimePaintableComponent.ServerPaintBatch`; relay APIs are diagnostics-only until proven equivalent.
- Python-generated front UV samples without game surface/world-position mapping. Synthetic UVs do not identify the player's visible front paint surface and must not be dispatched as the formal front paint path.
- Fixed/equal-spaced front dots or nearest-payload-color fill. Front camouflage must come from native game surface sampling plus hidden/background capture/readback; arbitrary UV interpolation only creates misleading dots and is not product behavior.
- Front paint without sRGB capture evidence. If capture/readback parity is unavailable, F10 must fail closed instead of guessing colors.
- Native `CreateRenderTarget2D` / `SceneCapture2D` from the reflection bridge. This repeatedly caused UE D3D12 `CreateCommittedResource` `E_INVALIDARG` crashes even with bounded render targets and byte-width enum writes. F10 must not call this backend; capture/readback needs a typed Dumper7 SDK implementation before it can be re-enabled.
- Metallic-only side effects when the full front route is impossible. If the required front capture backend is disabled, F10 must not dispatch the metallic base.
