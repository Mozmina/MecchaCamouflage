# Meccha Camouflage v1.4.0 Mesh-First Paint Plan

## Summary

v1.4.0 formal implementation should replace the current dense runtime hit-test
paint path with a mesh-first pipeline. The current approach creates recurring
sampling, progress, precision, and server-load issues because it discovers paint
points by probing the live screen. The new pipeline should derive front, side,
and back paint targets from the actual skinned mesh pose, then send only the
final validated strokes through the existing multiplayer-safe server API.

No fallback path should remain for the old dense hit-test sampler. If mesh,
mapping, runtime identity, skinned pose, or server API validation fails, paint
must fail clearly instead of silently falling back to the old behavior.

## Hard Requirements

- Multiplayer support is mandatory.
- Paint replication must use `RuntimePaintableComponent.ServerPaintBatch`.
- Stroke payload must continue to use `FPaintStrokeBatch` / `FPaintStroke`.
- UV strokes should use `FPaintStroke.Uv`, `TargetChannel=Albedo`, and
  `bHasWorldPosition=false` unless a validated server-side reason requires
  otherwise.
- Local-only paint, local texture mutation, `PaintAtScreenPosition`, and
  client-only workarounds are not acceptable replacement paths.
- Skinned pose resolving is mandatory. Bind-pose projection is useful for
  offline research, but not sufficient for final implementation.
- Old dense runtime hit-test sampling should be removed once the mesh-first
  path is validated.

## Current Research Facts

- Runtime paint target identity is confirmed:
  - component: `BP_FirstPersonCharacter_cLeon_Character_C.Mesh`
  - asset: `/Game/3Dmodel/cLeon/charactor/paintman/skeltal/paintman.paintman`
  - offline package:
    `Chameleon/Content/3Dmodel/cLeon/charactor/paintman/skeltal/paintman.uasset`
- `paintman` LOD0 export exists:
  - `1660` vertices
  - `8352` indices
  - `2784` triangles
  - `28` bones
  - one UV channel
- Research front sample artifact works:
  - latest front samples: `20029`
  - latest side/back target samples: `3105`
  - raw nearest-UV color transfer is too loose:
    - p95 source UV distance around `0.20`
    - max around `0.317`
- Mapping, package load, skeletal mesh conversion, runtime identity, and
  offline colorized UV plan generation are unblocked.
- Current blocker: filtered mesh/UV transfer and runtime skinned pose resolver.

## Target Pipeline

### 1. Runtime Resolve

Resolve the live paint target and collect only stable runtime state:

- target `RuntimePaintableComponent`
- owning actor / pawn
- `SkeletalMeshComponent`
- `SkeletalMesh` asset identity
- component transform
- camera transform and projection parameters
- current skinned pose or bone/component-space transforms
- `ServerPaintBatch` function pointer

This stage should not generate paint samples. It only supplies identity and
pose data to the planner.

### 2. Mesh Data Load

Load the matching mesh data from the research-derived asset profile:

- vertices
- indices
- UVs
- sections/material slots
- bone weights
- reference skeleton
- UV triangle adjacency
- UV island IDs
- bone/body-region labels

The runtime package should not depend on CUE4Parse. Use generated, versioned
mesh profile data produced by research tooling, committed only if it is small
and license-safe. Large/generated game assets remain uncommitted.

### 3. Skinned Pose Resolver

Resolve current pose and produce current skinned vertex positions.

Required outputs:

- per-bone current transform
- per-vertex skinned position
- per-triangle world normal
- per-triangle world bounds or center
- mapping back to UV triangle

Preferred resolver order:

1. Read component/bone transforms through stable Unreal reflection or known
   `USkinnedMeshComponent`/`USkeletalMeshComponent` fields.
2. Validate transform count against the exported `paintman` skeleton.
3. Skin vertices in our planner using exported weights and current bone
   transforms.
4. Reject paint if pose data is missing, stale, or inconsistent.

No bind-pose fallback in production.

### 4. Front Projection And Color Sampling

Project skinned mesh triangles into screen/capture space:

- classify visible front-facing triangles
- rasterize or sample triangle interiors in UV space
- map sampled UV points to screen pixels
- read color from scene capture / render target
- store front UV/color samples as the authoritative visible color source

This replaces dense `HitTestAtScreenPosition` sampling. Runtime hit-test may
remain temporarily only as a research assertion, not as a production fallback.

### 5. Side And Back Candidate Generation

Generate side/back candidates from mesh topology:

- classify triangle facing using camera direction and skinned normal
- use triangle adjacency to expand from visible/front regions
- identify side regions as front-adjacent high-angle surfaces
- identify back regions conservatively through connected UV/mesh regions
- compute candidate UV points from triangle area and brush spacing

Do not use orbit capture or user-driven multi-view capture for production.

### 6. Color Transfer

Transfer color from front samples to side/back candidates with constraints:

- same UV island
- same or compatible bone/body region
- bounded source UV distance
- bounded mesh-geodesic or adjacency distance when available
- optional normal-angle continuity
- optional color smoothing only within the same region

Candidates failing constraints are dropped. They are not filled by fallback
colors.

### 7. Stroke Plan

Build a deterministic stroke plan:

- front strokes
- side strokes
- back strokes
- source sample references
- source distance metrics
- dropped candidate counts and reasons
- estimated server batch count
- estimated elapsed time from saved pacing settings

The plan should be inspectable in Trace/research output before replay is
enabled.

### 8. Server Replay

Send only validated final strokes through `ServerPaintBatch`.

Rules:

- keep current conservative pacing defaults
- never exceed configured batch limit
- preserve `server_batch_delay_ms`
- stop on first batch failure
- write actionable error to Log and raw details to Trace
- do not retry by switching API

## Runtime Code Direction

### Keep

- bridge injection and request loop
- `ServerPaintBatch` call path
- `FPaintStroke` layout validation
- GUI settings for brush radius, spacing, batch limit, and batch delay
- Log / Trace split
- research artifact toggle while the migration is in progress

### Replace

- dense screen-space hit-test sampling
- current `template_phase0_base/dense` flow
- `sample_pool` generation from hit-test points
- progress model tied to hit-test percent
- raw nearest-UV transfer as replay input

### Add

- mesh profile loader
- skinned pose resolver
- skinned mesh projector
- UV island/adjacency builder
- body-region classifier
- filtered transfer planner
- stroke plan serializer
- server replay executor for precomputed plans

### Remove Before Release

- old dense hit-test production path
- old path-specific progress messages
- any fallback to local/client-only paint
- research-only raw views from release UI unless explicitly behind a runtime
  research toggle

## Proposed Phases

### Phase 1: Planner Filtering

- Add `max_source_distance_uv`.
- Add UV island detection from UV triangle adjacency.
- Add per-triangle dominant bone / body-region labels.
- Filter target samples by distance, island, and region.
- Output kept/dropped summary.
- No server replay.

Acceptance:

- Colorized plan contains only kept side/back targets.
- Dropped targets include reason counts.
- High-distance samples are removed.

### Phase 2: Skinned Pose Research

- Locate runtime bone transform source for `SkeletalMeshComponent`.
- Validate transform count and bone names against exported `paintman`.
- Dump one pose artifact with bone transforms only.
- Do not paint from this yet.

Acceptance:

- Pose artifact is stable across several poses.
- No crashes.
- Missing/inconsistent pose is detected as a hard error.

### Phase 3: Skinned Projection

- Skin exported vertices using current pose.
- Compute world normals and projected screen coordinates.
- Compare projected front regions against current scene capture visually and
  numerically.
- Remove dependency on runtime hit-test for front point discovery.

Acceptance:

- Projected silhouette roughly matches the visible character.
- Front UV/color samples can be produced from projection plus scene capture.
- Old hit-test sampler is no longer needed for sample generation.

### Phase 4: Unified Front/Side/Back Plan

- Generate front, side, and back strokes from one mesh-first planner.
- Front uses projected visible triangles.
- Side/back use filtered transfer from front samples.
- Produce one deterministic stroke plan.

Acceptance:

- Plan can estimate stroke count and server batch count before replay.
- Plan rejects unsafe transfer instead of filling everything.
- Plan is reproducible for the same pose/camera/settings.

### Phase 5: Server Replay

- Add bridge command path that replays a validated stroke plan through
  `ServerPaintBatch`.
- Keep pacing conservative.
- Stop on first server failure.
- Surface all replay metrics in Log/Trace.

Acceptance:

- Multiplayer paint uses `ServerPaintBatch`.
- No local-only paint path is called.
- Server batch pressure remains controlled.

### Phase 6: Remove Old Path

- Delete dense hit-test production code.
- Delete old template progress model.
- Delete old fallback assumptions from docs and UI.
- Keep research tools separate.

Acceptance:

- No production code path advertises or uses the old sampler.
- Paint fails clearly when mesh/pose planning cannot run.

## Validation Plan

- `make build`
- `make package`
- `git diff --check`
- run asset probe after game update
- run mesh planner with and without front samples
- verify release package does not include generated research artifacts
- verify bridge capabilities remain limited to supported production commands
- verify replay path calls only `ServerPaintBatch`
- inspect Log/Trace for:
  - mesh identity
  - pose resolver result
  - planned stroke counts
  - dropped candidate counts
  - server batch metrics

## Stop Rules

- Stop after any game crash.
- Stop if pose transform count does not match expected skeleton.
- Stop if server function signature or `FPaintStroke` layout changes.
- Stop if projected mesh cannot be aligned with scene capture.
- Stop if filtered transfer still has large cross-region color bleeding.
- Stop if server batch failures appear during replay.

## Open Questions

1. Should generated `paintman` mesh profile data be committed as a compact
   derived profile, or regenerated locally from research tools after each game
   update?
2. What is the maximum acceptable skipped side/back coverage if filters reject
   unsafe samples?
3. Should back paint be enabled only after side paint is stable, or should the
   unified planner output all regions but replay only regions explicitly
   enabled in settings?
4. What UI should expose this migration: a simple `Mesh-first paint` status, or
   a research/debug view with pose, filter, and replay metrics?
5. Should `MECCHA_RESEARCH_ARTIFACTS` remain as an environment toggle, or move
   to an in-app research setting while the migration is in progress?
