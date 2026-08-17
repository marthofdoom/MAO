# NOTE — tree-mode detection is name-based; breaks only when the Synthesis output is renamed/merged

**Not a shipped bug.** 100+ users run MAO with no "perks unlocked on new game" reports, because a
normal install's Synthesis patcher emits a standalone plugin literally named **`MAO - Patch.esp`**,
which the DLL's tree-mode check finds → TREE mode → no auto-grant. Working as designed.

**When it misfires (observed 2026-08-14, Terminal Destiny "requiem-removal" Default profile):** if
MAO's AVAlchemy patch is delivered under a DIFFERENT plugin name — e.g. that build's Default Synthesis
run **merged all patcher output into one generic `Synthesis.esp`** instead of emitting a separate
`MAO - Patch.esp` — then no plugin of that name is loaded, so:

`native/plugin.cpp:3738`
```cpp
g_treeMode.store(dh->LookupModByName("MAO - Patch.esp") != nullptr);  // false when output was merged/renamed
```
→ `g_treeMode = false` → the auto-grant block (`native/plugin.cpp:~287-298`, gated on
`!g_treeMode && g_capacityPerksResolved`) fires and grants the capacity ladder
(`native/plugin.cpp:186-190`: Kit Calibration I @ Alchemy 0, etc.). The `pl` name-list loop at ~3743
has the same name dependency.

## Fixes, in order of preference for THAT build (not urgent — no community impact)
1. **Config (correct + no code change):** make the profile's Synthesis pipeline emit MAO's AVAlchemy
   patch as a standalone plugin named `MAO - Patch.esp` (what the Requiem profile + every normal user
   already gets) rather than folding it into the merged `Synthesis.esp`. Removes the misfire and the
   interim dummy below.
2. **Optional DLL hardening (robustness, not a bug fix):** make tree-mode detection read the WINNING
   `AVAlchemy (0x000456)` AVIF and set `g_treeMode` if MAO's capacity-perk forms (the `186-190` ladder)
   appear as nodes — regardless of which plugin supplied the tree — exactly as MEO.dll does for
   AVEnchanting. Makes MAO tolerant of any output naming/merging. Weigh against the risk of changing
   code that ships fine to everyone.
3. Add a startup log line: `tree-mode = <TREE|auto-grant> (via <name|tree-scan>)` so any future
   misdetection is obvious in MAO.log.

## Interim workaround deployed on that Deck build (remove once #1 is done)
A header-only ESL dummy named `MAO - Patch.esp` (masters MAO.esp, zero records) was added to the
Terminal Destiny Default load order purely to satisfy the name check. The real AVAlchemy tree still
comes from `Synthesis.esp`. Delete that dummy mod once the pipeline emits a proper `MAO - Patch.esp`.
