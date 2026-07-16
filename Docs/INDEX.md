# MAO Documentation Index — start here

Marth's Alchemy Overhaul: flask-based alchemy preparation for Skyrim SE.
Single-use potions are removed from loot and vendors; players charge permanent,
reusable **Flasks** with abstracted **Material Essences** gathered in the
field. This project is designed so any capable model or person can continue it
from these docs alone. Load documents on demand, not all at once.

## Read order for a fresh session

1. **DESIGN.md** (always) — what MAO is: the three-tier essence spine, the
   flask/charge kit model, the perk override matrix, blueprint discovery via
   potion-pickup interception, the magnitude ceiling rules (standalone caps at
   Requiem max; over-cap is MRO-gated), and the automated-potion-mod
   compatibility contract. This is the spec.
2. **ARCHITECTURE.md** (before touching `native/plugin.cpp`) — the subsystem
   map with line refs: identity/data structures, the gathering ContainerSink
   (the sole essence-credit point), the recipe-based essence economy, the
   flask system (dedicated forms, DrinkPotion hook, coatings, refill), the
   perk vehicle + capacity ladder, the Field Kit menu, the hook inventory +
   thread/lock map, the co-save schema, and the generator/installer/release
   contracts (frozen FormID bands).
3. **INVARIANTS.md** (before ANY code change) — the load-bearing rules, each
   an imperative + the concrete failure mode that produced it: MAO's own
   incidents (furniture eject, co-save contamination hold guard, capacity-
   before-sync, MCM Settings backfill, reset-then-parse, self-deadlock/TOCTOU)
   plus the inherited MEO persistence/threading/release disciplines.
4. **ANTI_PATTERNS.md** (the one-sitting audit list, before repeating
   history) — the portable "never again" catalog adopted from MEO's
   release-proven digest, trimmed to what has a MAO analog and extended with
   MAO's own entries; one line per rule on how it bit MEO or MAO.
5. **P1_PLAN.md** (current phase) — the core build: milestone breakdown
   (P1a–P1f), status, and the two design shifts (alchemy-station opener;
   flasks embody specific discovered variants). P1a–P1d + station takeover are
   done; P1e (perks) is next. `Docs/BUILD.md` tracks per-milestone CI/test
   status.
6. **P0_PLAN.md** (done) — the gathering-loop prototype: the full hook
   inventory for the whole design, the P0 architecture (harvest +
   container-changed sinks, `'POCH'` co-save, double-credit guard,
   quest-ingredient exclusion) plus the read-only essence viewer.

## Sibling projects (the toolchain lives there — reuse, don't re-derive)

MAO is the third project on a shared toolchain. Both siblings were built first
and their docs are the working references until MAO grows its own:

- **MEO** (`../marth-enchanting-overhaul/`, github.com/marthofdoom/MEO) — the
  closest structural template. Its `Docs/INDEX.md` read order applies here
  almost verbatim:
  - `Docs/MANUAL_MOD_CREATION_GUIDE.md` — the binary format reference
    (record/group/subrecord encoding, verified recipes, PO3 event rules,
    Papyrus-on-Linux compilation, FOMOD rules). Consult BEFORE creating or
    altering any record.
  - `Docs/DEBUGGING.md` — symptom → cause → fix for every failure class hit
    across MRO and MEO, plus the universal diff-against-vanilla method.
  - `Docs/DYNAMIC_OR_DROP.md` — the portability rule: anything baked from THIS
    machine's load order at generation time must become runtime-dynamic or be
    dropped before 1.0. MAO's potion→blueprint interception and essence
    sourcing will bite on this.
- **MRO** (`../Requiem-modification/`, github.com/marthofdoom/MRO) — the
  proven native/DLL reference: `docs/NATIVE_REWRITE_PLAN.md` (CI-built
  CommonLibSSE-NG DLL via GitHub Actions, hook doctrine, SKSE co-save
  serialization) and `docs/DEBUGGING.md`. MAO is native-first (flask state,
  harvest interception, and inventory-count faking all live in the DLL), so
  this matters here even more than it did for MEO.

## Tools (bring over, don't rewrite)

When record generation starts, port from the siblings:

- MEO `MEO_GenerateESP.py` — the byte-level ESP generator pattern
  (`MAO_GenerateESP.py` will follow it).
- MRO `tools/dump_record.py` — inspect any record vs its vanilla twin — THE
  diagnostic.
- MRO `tools/audit_esp.py` — wiring + FormID audit.
- MEO `tools/compile.sh` / `release.sh` / `release_native.sh` — Papyrus
  compilation under Proton wine and the immutable-release flow.

## The one principle

When touching the binary format: **copy a working vanilla record, never trust
documentation** — including this documentation. If a record misbehaves, dump
it and its vanilla twin and diff subrecords. Every multi-day bug across the
sibling projects (TES4 flags, FOMOD wrapper, SPIT type, PERK layout, FormID
prefix, SEQ, MGEF fortify archetype) ended the moment we compared bytes
against something that worked.
