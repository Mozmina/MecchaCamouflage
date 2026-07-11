# Runtime Paint Replication Research

This document tracks the reverse-engineering surface for multiplayer paint
replication. It is separate from normal runtime docs because these entrypoints
are investigative and may be useful even when they are not part of production
paint behavior.

## Current Production Route

Normal paint uses the direct component route:

- `RuntimePaintableComponent.ServerPackedPaintBatch`
- painter-side submission through the native packed receiver implementation
  behind `MulticastPackedPaintBatch`, called directly rather than through the
  reflected multicast UFunction
- identical server/local packed boundaries and pacing (1--20 strokes,
  50--500 ms; default 20/50)
- exact PE/text identity plus reflected payload, UFunction thunk, vtable slot,
  decoder, component-to-manager resolver, and enqueue-chain validation before
  the first packed submission
- one packed-radius scale is derived per validated mesh/job from the
  UV-area-weighted world-triangle/UV Jacobian and live mesh bounds diameter,
  then multiplied by the host-A/B conservative fold/seam safety factor; it is
  frozen across Fill/Coarse/Fine and does not change planner spacing
- mesh-anchor `EffectiveBrushWorldRadius` remains the non-positive conversion
  sentinel. Compact expansion derives the world radius only for `<= 0`;
  copying normalized UV radius here collapses strokes to dots
- the effective subdivision tail is exactly `level=0`, `pixel-size=0`,
  `template-resolution=0`, allowing receiver preflight to select the component
  defaults. These fields are not brush diameter bytes
- an exact manager/component queue probe before every server commit and an
  exact queue-count increase after the paired local receiver call
- no fallback to old compact/adaptive `SendCustom` path
- no fallback to the per-stroke internal common or reflected
  `PaintAtUVWithBrush` routes when validation fails
- failure stops paint with explicit metadata

`local_packed_queue_calls_returned` proves only that the receiver implementation
returned. `local_packed_queue_last_queue_delta` proves observed queue growth on
the exact manager. Neither field proves a render-thread write or pixel coverage;
even a few dots change a whole-texture checksum. Release evidence must separately
observe queue drain and compare exported channel bytes by changed-texel count (or
perform an equivalent visual-coverage check), not hash inequality alone.

The game renderer's homogeneous-batch key includes quantized ChannelData.
Different camouflage colors therefore commonly turn one packed network batch
into per-stroke surface generation. Network batch size and render batch size
must not be treated as equivalent in FPS analysis.

Painter-local receiver backlog is not remote-peer pressure and does not back
off the outgoing server lane. The game-owned receiver/render budgets drain it
asynchronously without modifying any game limit. The production route must
remain small and deterministic. Research probes and artifact collection must
not change normal route selection; research mode names explicitly choose their
A/B route.

## Research Entry Points

The bridge currently exposes these investigation-only command types:

- `paint_replication_probe`
  - resolves the current paint component and replication functions/properties
  - reports reflected schema and queue metadata when available
- `paint_replication_pressure_probe`
  - samples global/component replication pressure and drain-related values
  - used to compare old queue/drain behavior with packed route behavior
- `paint_packed_replay_probe`
  - submits a caller-provided packed payload through the selected packed route
  - dangerous enough that scripts should require an explicit replay opt-in
- event-watch sidecar
  - samples selected `ProcessEvent` calls when enabled by sidecar/config
  - useful for discovering host/client route differences

These commands are classified as `RESEARCH_ONLY` in
`docs/runtime-bridge-map.md`.

## Authenticated research access

The old fixed-port, unauthenticated probe scripts are removed. A research
client must use the same per-instance endpoint and GUID/token HELLO handshake
as the controller; see [`runtime-direct-bridge.md`](runtime-direct-bridge.md).
Do not scan ports or send a command before HELLO. Keep generated output under
`artifacts/research/` or a local temp directory.

## Multiplayer Verification Questions

When a route changes, collect these facts separately for host and joining
client:

- who initiated paint: host or joining client
- whether other players see the paint
- painter-side completion time
- other-client visible completion time
- delay between painter completion and other-client completion
- crashes, disconnects, lobby returns, freezes, missing paint, or partial paint
- event-watch counts for old send path, packed component route, and relay route

The important distinction is not just whether the painter finishes quickly. The
other normal client must also receive the result without falling back to the old
2-3 minute replication drain path.

## Cleanup Rules

- Do not delete research helpers only because static analysis shows few
  references.
- Do not expose research probes in normal UI unless they become supported user
  behavior.
- Do not let probe output change production paint decisions.
- Keep `ProcessEvent`, RPC payload layout, and SDK padding changes behind live
  verification.
- Move repeatable one-off experiments into `scripts/research/` before adding
  more bridge command strings.

## Artifacts Not To Commit

- event-watch output
- raw replay payloads
- game archives or cooked assets
- generated mappings
- dumps, traces, injected DLLs, and local diagnostics bundles
