# Feature Roadmap

Ideas for future work on Zemi Mecchamouflage. Nothing here is scheduled or
committed — this is a running list to pick from, not a promise.

## Up next (picked from the idea list below)

- **Pass-complete sound/notification** (idea 14).
- **More human-like painting** (idea 16) — make the painted result look less
  mechanical/uniform.

## All ideas

1. **Preview reflects the second pass.** Today Preview (F2) always renders
   using the first-pass geometry settings (brush size, color compression
   tolerance) and never the second-pass ones, because Preview stops before
   any per-stroke geometry is built at all — it only applies material
   channels locally. Preview looks identical whether or not a second pass has
   ever been run. A toggle (or an auto-detect based on whether a second pass
   has completed) could make Preview approximate the second pass's look
   instead.
2. **Configurable number of passes.** Generalize the current two-pass
   pipeline (start hotkey + second-pass hotkey) into an ordered list of N
   passes, each with its own brush size, tolerance, and region scope, instead
   of hardcoding exactly two.
3. **Per-region brush/tolerance overrides.** Let Front/Side/Back each have
   independent brush size and color compression tolerance instead of one
   shared pair per pass.
4. **Named presets.** Save and load full paint configurations (geometry,
   material, regions, fill) as named profiles, switchable from a dropdown,
   instead of one single always-current configuration.
5. **Undo last paint.** Restore the mesh to its pre-paint state directly,
   independent of the existing Preview/UnPreview flow.
6. **Auto second pass.** An optional setting to automatically trigger the
   second pass right after the first pass completes, without a separate
   hotkey press.
7. **In-game overlay progress.** Render pass/progress information as a small
   on-screen overlay in the game window itself, not only in the app's log
   panel.
8. **Fill color swatches / history.** Keep a small history of recently used
   fill colors as quick-pick swatches next to the color picker.
9. **Export/import settings file.** Let users export their full settings as
   a shareable JSON file and import someone else's camo profile.
10. **Randomize material.** A "randomize" button that jitters
    Metallic/Roughness/Emissive within a range for a more natural,
    less uniform-looking camo.
11. **Bridge/version mismatch warning.** Detect when the injected native
    bridge DLL version doesn't match the running exe and warn the user
    instead of failing silently.
12. **Compact/streaming UI mode.** A minimal window layout (status + log
    only) for streamers or low-screen-space setups.
13. **Quick region-mode cycling hotkey.** A hotkey to cycle Front/Side/Back
    through Paint -> Fill -> Skip without entering Settings edit mode.
14. **Pass-complete sound/notification.** Optional audio or Windows
    notification when a pass finishes, useful since players are usually
    alt-tabbed into the game while it runs.
15. **Suggested second-pass defaults.** Auto-suggest sensible second-pass
    brush/tolerance values based on the first pass's settings (for example,
    half the brush size and zero tolerance) instead of fixed defaults.
16. **More human-like painting.** The current pipeline paints in a very
    mechanical way: every stroke in a pass uses the exact same brush size and
    tolerance, so the result can look too uniform/perfect compared to a real
    hand-painted camo job. Possible directions: small per-stroke jitter on
    brush size/position, slightly varying blend strength stroke to stroke,
    or mixing a couple of brush sizes within one pass so edges look less
    like a machine pass.
