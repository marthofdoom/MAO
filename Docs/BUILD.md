# BUILD — staged milestone tracker

MAO ships in small, individually **CI-green** builds. Each milestone is one
downloadable `MAO.dll` artifact you test in-game before the next is written.
The ordering rule is inherited from MRO: **one hook class per build, so a CTD
bisects to exactly one change.** Event sinks (low risk) come before code hooks
(render/present thunk — higher risk, must pass `verify_hook_site_live.py`).

The toolchain is copied from the sibling MEO project, not re-derived: CMake +
vcpkg + CommonLibSSE-NG, built on GitHub Actions (`.github/workflows/native.yml`,
windows-latest). Linux cannot link the MSVC ABI, so the DLL is always a CI
artifact — `gh run download` pulls it.

## P0 — the gathering loop + read-only essence viewer

| # | Build | Hook class | State | Gate |
|---|-------|-----------|-------|------|
| **M0** | Skeleton DLL — loads, logs version + runtime, zero hooks | none | ✅ **CI green** | CI green + `MAO.log` shows the version line |
| **M1** | Gathering loop — `TESContainerChanged` credit sink + `TESHarvested` tag sink, value-weighted essence, `'POCH'` co-save | event sinks | ✅ **CI green** | Test matrix 1–14 (P0_PLAN) |
| **M2** | Field Kit → ImGui read-only essence viewer | render/present + input code hooks | ✅ **CI green** | Test matrix 15–16 |

**M2 opens via a hotkey, not the power (interim).** A castable lesser power
needs a `SPEL` form from an ESP, and standing up `MAO_GenerateESP.py` is its
own untested surface — so M2 is kept to the single render-hook risk and opens
the viewer with `iOpenHotkey` (MAO.ini). DESIGN §3.3 still governs: the viewer
becomes the flask UI opened by the lesser power in **P1**, when the ESP that
grants the power exists for flasks/blueprints anyway. The render framework
(D3D11 present thunk, ImGui init, input dispatch) is ported from MEO's
`menuhook` namespace at its proven address-library IDs (77226 / 77246 / 68617);
only the panel content is MAO's, and it's read-only (no mouse/font/skin code).

## Staged ahead — P1+

These are scoped in DESIGN.md and will each decompose into their own M-series
of one-hook builds when their phase begins. Listed so the runway is visible:

- **P1 — core.** Flask lifecycle (configure / drink / deplete / refill),
  potion-pickup → blueprint interception (same container sink, `AlchemyItem`
  branch), the blueprint catalog generated from the masters
  (`MAO_GenerateESP.py` grows a catalog like MEO's), the 13-perk override
  matrix, the automated refill pipeline (`TESSleepStop` + background timer),
  and the full MCM option set (the INI surface seeded in M1 grows here). Flask
  *configuration* is built onto the M2 viewer screen.
- **P2 — coatings & polish.** Weapon-coating conversion (Vanguard Coating /
  Corrosive Retention), manual-drink handling, menu-skin styling.
- **P3 — packaging & compat.** FOMOD with load-order-aware install (DESIGN
  §6.2 — detect the existing alchemy overhaul and smartly replace it), the
  Requiem/LoreRim compatibility plugin, and the automated-potion-mod
  compatibility contract (inventory-count faking, consumption intercept —
  the riskiest hooks, sourced then).
- **P4 — MRO integration.** Over-cap magnitude framework gated behind MRO.

## Releasing a build

`tools/release_native.sh <version> "desc"` pulls the latest successful `native`
run's DLL and wraps it MO2-style (zip root = virtual `Data/`). Releases are
immutable under `releases/<version>/`. Until M2 adds the ESP, releases are
DLL + `MAO.ini` only.

## Per-build checklist

1. Write one milestone's hook class only.
2. `git push` → watch `gh run watch` until green; fix compile errors and repush.
3. `gh run download` the artifact (or `release_native.sh`) → install → run the
   milestone's slice of the P0_PLAN test matrix in-game.
4. Only then start the next build.
