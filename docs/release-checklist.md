# Release Checklist

This checklist is version-independent. Keep release decisions in GitHub issues
or release notes; keep this file focused on the repeatable process.

Do not tag a release until maintainer-owned game checks are complete.

## Local Checks

Run these before preparing a tag:

```bash
git status --branch --short
make review-dead-code
git diff --check
make clean
make package VERSION=vX.Y.Z
```

`make clean` removes `.build/` only. It does not remove `artifacts/`, because
review and research reports are often useful during investigation.

Use `make clean-artifacts` only when ignored review/research reports should be
discarded. Use `make clean-all` to remove both `.build/` and `artifacts/`.

Confirm:

- release artifact is a single EXE under `.build/package/`
- `ReleaseSingleFile` output and package output contain no `.pdb`, `.dbg`, or
  `.ilk` files
- the normal release build excludes the research command runner and cannot
  enable WebView DevTools through `MECCHA_RESEARCH_ARTIFACTS`
- app uses the installed shared Evergreen Runtime; no browser runtime files are embedded
- root directory has no generated `*.dll` or `*.exe`
- source tree has no generated `bin/` or `obj`
- shipped app resources are under `resources/`
- source code is under `src/`
- dead-code inventory contains no reviewed deletion candidate, or every such
  candidate was removed with focused verification; dynamic entries, Unreal
  reflection/layouts, and research-only paths are retained by default

## Publication

After the local and maintainer checks pass, create the annotated tag from the
release commit, push the commit and tag, then create the GitHub Release with the
single EXE from `.build/package/`.

```bash
git tag -a vX.Y.Z -m "vX.Y.Z"
git push origin main --follow-tags
gh release create vX.Y.Z .build/package/meccha-camouflage-vX.Y.Z.exe --title vX.Y.Z --notes-file <maintainer-notes.md>
```

Write release notes in GitHub before publishing. Do not regenerate, rewrite,
or commit a maintainer-owned changelog as part of the packaging command.

## Runtime Packaging Checks

The packaged EXE must include:

- native bridge DLL
- native injector EXE
- official WebView2 Evergreen bootstrapper
- Web UI assets
- mesh profile resources
- app icon resources

The app must extract these into LocalAppData and repair missing or corrupt
runtime cache files automatically.

## Maintainer Game Checks

These require MECCHA CHAMELEON.

- Start the app with no game running.
  - GUI initializes.
  - error state is clear and diagnostic.
  - no unhandled .NET dialog appears.
- Start the app with the game in menu or lobby.
  - bridge state is clear.
  - paint in lobby fails as a paint-time pawn/component error, not startup
    failure.
- Start the app in a valid paintable match.
  - preview applies.
  - unpreview restores.
  - test Preview Paint and Preview Fill separately. Front defaults to Fill, so
    changing Paint PBR alone must not be used to judge a Front Fill preview.
  - with a controlled manual value such as `M=.21/R=.83/E=.47`, the selected
    material-properties texture changes to approximately `R=54/G=212/B=120` in both
    Preview Paint and Preview Fill. Restore on the same bridge and verify the
    original snapshot returns.
  - with Auto Detect enabled, record `material_properties_candidates`,
    `material_properties_selection`, and the first-stroke M/R/E values. Auto
    Detect applies only to Paint regions; Fill remains manual. A global
    dominant `M=0/R=1/E=0` result is valid when the game returns that pattern.
  - repeated unpreview shows a guard warning.
  - cancel with no active paint shows a guard warning.
  - normal paint completes.
  - with any one region set to Fill, the initial fixed-100 Fill pass covers
    Front, Side, and Back (including Paint/Skip regions), then the single Paint pass
    overwrite only Paint regions.
  - progress reports native queue backpressure, submitted strokes, and
    completed strokes. It must not report completion while the queue is nonzero.
- Delete the LocalAppData runtime cache and restart.
  - cache rebuilds automatically.
  - WebView2 starts from the installed Evergreen Runtime, or prompts to install it with the embedded bootstrapper.
- Restart the controller against the same game process.
  - the new controller instance starts and authenticates its own direct bridge.
  - older direct bridges or any other resident module do not produce a restart-required state.
  - an indeterminate injector timeout is diagnosed and requires an explicit retry; it does not trigger unload or thread termination.
- Cancel and then shut down separate paints while each is still in the planning
  phase. Each command must report one active job, the paint must reach a
  cancelled terminal reply, shutdown must report
  `active_paint_quiescent=true`, and an immediate fresh injection must complete
  without restarting the game.

## v1.6 Direct Bridge and WebView2 Gates

- Test direct injection on Windows 10 and Windows 11 with a clean game,
  multiple same-name game processes, target exit during injection, concurrent
  host launches, and old direct bridges/modules already resident in the game.
- Run 25 sequential direct injections into one game process. Every successful
  connection must match the selected PID, generated instance GUID, token, and
  bridge hash; no attempt may become restart-required because an old module is
  present.
- Verify a timeout never frees remote path/start-block memory before the
  corresponding remote thread exits. The production path must contain no
  `TerminateThread`, unload, switch, or loader fallback. The source tree and
  packaged output contain no loader component.
- Verify Evergreen WebView2 with a runtime already present, absent with a
  successful bootstrapper install, bootstrapper failure/offline behavior, a
  clean user-data folder, two rapid app launches, and a forced WebView process
  failure. Diagnostics must identify the failed startup stage.

## Multiplayer Checks

Collect these separately for painter-as-host and painter-as-joining-client:

- whether other players see the paint
- painter-side completion time
- other-client visible completion time
- delay between painter completion and other-client completion
- start a normal paint, then enter freecam or spectator mode while the captured
  paint component remains valid; the job must continue rather than stop solely
  because the controller pawn changed
- crashes, disconnects, lobby returns, freezes, missing paint, or partial paint
- event-watch counts if available:
  - `PaintAtUVWithBrush > 0`
  - no alternate paint sender is present
- production-route metadata:
  - `local_route_mode == "native_recorded_paint"`
  - `local_paint_rpc == "PaintAtUVWithBrush"`
  - `local_texture_import_started == false`
  - `local_render_target_write_budget`, `local_cpu_budget_yields`, and
    `local_dispatch_total_ms` are captured with the run. Do not lower the
    recurring scheduler below its 1 ms safety floor merely to improve this
    measurement.
  - missing `PaintAtUVWithBrush` fails explicitly before any stroke is sent;
    it must not silently choose texture import or another paint transport
- before/after texture probes show a nontrivial changed-texel area on the
  painter's resolved component after terminal completion. Record
  `texture_delta.pixels_changed_any_channel` and its ratio; checksum inequality
  alone is insufficient because a few tiny dots also change the hash. Collect
  corresponding remote-client changed-texel evidence after replication and
  require both replication queues to return to zero. Inspect the coordinate
  dump or the character as well: a regular point grid with gaps is a failure
  even when changed-texel count is nontrivial
  - normal Paint must not apply the completed Preview texture at start. Confirm
    the painter progresses through Fill and the Paint pass while the
    game-owned recorded-paint queue follows submitted direct work. When investigating
  FPS drops, record `local_dispatch_total_ms`, `local_cpu_budget_yields`, and
  `local_write_budget_yields`; do not remove the 1 ms scheduler yield to make
  a one-off benchmark faster.

Do not release if joining-client paint crashes the game, or if other clients
remain visibly behind after the native queue reaches zero.
