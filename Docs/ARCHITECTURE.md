# Architecture — the native subsystem map (v0.19.0)

How `native/plugin.cpp` (~2320 lines, one translation unit) is actually
organized: per subsystem, what it does, the load-bearing functions with line
refs, and the engine flow each one rides. **DESIGN.md** says what MAO is;
**ENGINE_NOTES.md** says what the engine does; this file says where OUR code
does it. Line refs are against v0.19.0 — they drift, the function names
don't. The rules this code must obey live in **INVARIANTS.md**; the mistakes
it must not repeat in **ANTI_PATTERNS.md**.

## 0. Identity & data structures (plugin.cpp:73-352)

The whole mod is FOUR pieces of state plus one frozen ESP contract:

```
g_pouch      : Pouch { base, catalyst, apex : u32 }        (:101-106)
g_discovered : unordered_set<FormID>  (analyzed variants)  (:114, lock :115)
g_flasks[6]  : Flask { blueprint MGEF, repPotion, charges} (:124-129)
g_flaskCount / g_chargesPerFlask  (perk-scaled capacity)   (:130-131, lock :132)
```

- **Co-save**: `kSerID 'MAO1'`, records `'POCH'`/`'BLPT'`/`'FLSK'`,
  **schema v3** (:75-79) — v3 stores discovered potion FORMS (variants).
- **Frozen MAO.esp forms** (ESL, `LookupForm(localID, "MAO.esp")`):
  0x801 Open Field Kit SPEL (dormant power, :85), **0x810–0x815 the six
  dedicated flask ALCH forms** (`g_flaskForms` :94 — slot i ↔ form i), the
  perk band (:160-174, §4). The flask item is the STABLE identity of a slot;
  reconfiguring renames its contents only, so Favorites/Wheeler bindings
  survive (:88-93).
- **`g_discovered`** holds SPECIFIC AlchemyItem FormIDs — a Superior potion
  is usable only once you've actually found/bought one, never derived (:108-113).
- **Perk state** (:175-192): `g_perkForm[PK_COUNT]`, `g_treeMode`,
  `g_perkMask`, the MCM debug pair `g_perkDebug`/`g_perkWantMask`, and cached
  effect atomics (`g_alchemistRank`, `g_hasCapstone/Benefactor/Experimenter/
  Vanguard`). `kCoatingsRequireVanguard = false` (:199) is a compile-time
  TESTING relaxation of the coating gate — flip back before the tree ships.
- **Economy tables** (:297-323): `g_effectRecipe` (cheapest 2-ingredient
  recipe per effect, each ingredient with its own tier), `g_catalystUnit`/
  `g_apexUnit` surcharge units, `g_effectPotion` (effect → strongest/cleanest
  load-order potion) and `g_effectMinMag` ("standard" magnitude per effect).
  Tuning globals (`g_essenceTax`, `g_costRate`, `g_catalystLevel`,
  `g_apexLevel` :307-314; the config block :392-399) are **atomics** —
  ReadConfig re-runs mid-session while other threads read them (v0.17 review).
- **Menu state** (:334-346): `MenuState g_menu` atomics (open/cursorInit/
  station), `kFieldChangeLimit = 1` + `g_fieldChanges` for the limited field
  mode. **`g_conversionEnabled`** (:352) — the MCM master toggle for
  ingredient→essence conversion.

## 1. The gathering loop — ContainerSink (:1296-1422)

**The SOLE essence-credit point.** Anything an ingredient or potion enters
the player's inventory through funnels here.

- Gate order: `g_conversionEnabled` early-out at the head (:1304-1306 — OFF
  means ingredients/potions stay ordinary items, nothing else in the mod is
  affected), then **only items ENTERING the player** (`newContainer ==
  kPlayerID`, `itemCount > 0` :1310-1312) — so the sink's own `RemoveItem`
  can never re-trigger it.
- **Ingredient → essence** (:1321-1352): quest-ingredient exclusion (:1324,
  `kExclusions` :446 — Crimson Nirnroot), tier via `TierOfForm` (:522 — the
  installer's `mao_tiers.json` map :478 wins, name-substring heuristic
  :419-441 is the fallback), `YieldFor` = ceil(value × tier rate) min 1
  (:542), Field Extraction +10% (:1332), then a TASK does `RemoveItem` +
  `CreditPouch` + notify (:1336-1350).
- **Potion → blueprint discovery** (:1359-1418): food exits (:1360), a flask
  item being (re)granted exits (:1365 — never analyze our own items). The
  task destroys the potion, credits Base essence, and inserts the potion
  form into `g_discovered` **unless it's a runtime-created 0xFF form**
  (:1396 — crafted potions convert but are never persisted as variants).
  First-time discovery trains Alchemy (`kDiscoverXp` :692, :1402).
- **HarvestSink** (:1428-1445) fires alongside the container event on
  harvests: it TAGS (for future perk procs) and **never credits** — the
  double-credit guard from P0.

## 2. The essence economy (:297-767)

Built once at kDataLoaded (:2260-2262), in this order: `LoadTierMap` →
`BuildRecipeTable` → `BuildEffectPotionTable` (the recipe table tiers
ingredients per the map, so the map must load first).

- **`LoadTierMap`** (:486): `Data/SKSE/Plugins/MAO/mao_tiers.json`
  (installer-written, availability-based); resolves each `(plugin, fid)` via
  `LookupForm<IngredientItem>`, counts unresolved; parse failure clears the
  map and falls back to the name heuristic — never half-applies.
- **`BuildRecipeTable`** (:558): per effect, the two cheapest ingredients
  carrying it = the vanilla recipe, kept as (value, tier) pairs so the base
  cost splits per ingredient IN ITS OWN TIER; `g_catalystUnit`/`g_apexUnit`
  = median Catalyst/Apex ingredient values (:587-607).
- **`BuildEffectPotionTable`** (:729): per effect, the CLEANEST embodiment
  (fewest effects, then highest value) and the weakest primary magnitude
  (= "standard", the concentration denominator).
- **`VariantCost`** (:630) — the compound per-charge cost (Marth's model):
  concentration = magnitude / effect min-magnitude (:659); basis = conc ×
  `g_costRate` × `g_essenceTax` (:660); BASE = each recipe ingredient's
  value × basis added to THAT ingredient's tier pool (:662-665); +CATALYST
  surcharge one tier above the recipe's rarest at conc ≥ `fCatalystLevel`,
  doubled at ≥ `fApexLevel` (:666-669); any multi-effect variant adds an
  Apex unit (:671-673); Apex Stabilization −35% on the apex component
  (:674-676).
- **Spending trains Alchemy**: `SpendCost` (:702) awards
  `kAlchemyXpPerEssence` (0.1/essence, :691) via `AwardAlchemyXP` (:693) —
  configures AND automatic refills both count as potion-making.

## 3. Flasks: configure, sync, drink, coat, refill (:789-1161)

- **`ConfigureFlask(slot, potion, field)`** (:812) — task thread only, so it
  serializes with gathering/discovery. Checks the variant is in
  `g_discovered` (:817-821), resolves it, coating perk gate (:834), aborts
  if the slot's flask form didn't resolve — never spend into nothing (:841-847).
  Station mode pays `VariantCost × g_chargesPerFlask` up front and fills to
  max (:849, :859); **field mode** (power opener) is capped at
  `kFieldChangeLimit` changes per trip and starts the flask EMPTY with no
  upfront essence — it fills from the refill loop (:850-867). Reassigning a
  charged flask purges the charges, no refund (DESIGN §3.1). Then
  `RenameFlask` + grant the item if absent (:881-885).
- **`SyncFlaskItems`** (:904) — post-load: heals pre-P1d slots (blueprint,
  no variant) from `RepPotionFor`, re-applies every configured slot's name,
  re-grants missing items, and **resets the names of unconfigured slots**
  (:941-947) — form state is session-global, a stale "Flask: X" from another
  save would otherwise carry over. Snapshot under `g_flasksLock`, act
  OUTSIDE it (:912-917): grants fire `TESContainerChangedEvent` whose sink
  re-locks the same non-recursive mutex.
- **`DrinkPotionHook`** (:1049, installed by `InstallDrinkHook` :1151):
  `write_vfunc(0x10F)` on `PlayerCharacter::VTABLE[0]` (:1158) — the funnel
  all drink paths reach, including auto-potion mods; **VR bails** (:1154,
  the Actor vtable shifts). For a configured flask: re-verify the slot under
  the lock (:1057-1064), resolve the variant BEFORE spending a charge
  (:1077-1082 — unresolvable = inert, no charge burned), then either
  - **coating branch** (detrimental primary effect :1070-1073, Vanguard gate
    :1083): decrement, `ApplyCoating` (:1020 — replaces `ExtraPoison` on
    each equipped weapon's worn xList, `kCoatingHits = 10` interim until the
    45s/120s time model lands), full refund if no weapon was coated
    (:1098-1113);
  - **drink branch**: `CastSpellImmediate(variant)` on self (:1121-1123) +
    `PlayDrinkSound` (:771 — the variant's consumption sound, else
    `ITMPotionUse`).
  Every flask form — configured or not — returns `true` (handled, :1133,
  :1138-1144): **the permanent item is never vanilla-consumed**.
- **Refill** (:953): dry/partial flasks top up one `VariantCost` charge at a
  time from essence, on `TESSleepStopEvent` (:987) and a detached real-time
  timer thread (:1000-1012, `iRefillSeconds`) — both only queue a task.

## 4. Perk vehicle & capacity (:145-282)

The MEO vehicle, ported verbatim: ALL perks are MAO's OWN flag records in
MAO.esp — vanilla alchemy perks are never touched.

- **`kPerkDefs[PK_COUNT]`** (:160-174): label, frozen FormID, MCM debug key,
  skill requirement. PK_ALCH1..5 = the Kit Calibration NNAM chain (0x818-
  0x81C), capstone Master's Crucible 0x816, the choice perks 0x81D-0x823.
  Indices are FIXED — capacity logic and the MCM debug keys reference them.
- **Two modes** (:176-179): `g_treeMode` = "MAO - Patch.esp" present
  (:2252 — perks live in the installer-rewritten AVAlchemy constellation,
  bought with points); otherwise `RecomputeCapacity` AUTO-GRANTS each perk
  at its skill threshold (:224-233) — the patch-less interim vehicle.
- **`RecomputeCapacity(why)`** (:213) — task thread; callers: field-kit open
  (:375), MCM close (:2015-2018), post-load (:2279). Reads real perks into a
  mask via `HasPerk` (:234-239); the **MCM debug override** substitutes
  `g_perkWantMask` for the mask only (:240-242) — no AddPerk/RemovePerk, so
  it reverts cleanly. **The hold guard** (:253-257): if MAO.esp's Kit
  Calibration chain didn't resolve (stale/missing ESP), HOLD the co-saved
  counts instead of forcing the baseline — EVEN in debug (the co-save
  contamination fix, INVARIANTS 16). Ladder (:259-267): 2/2 baseline; rank 1
  → 2/3; rank 2 → 3/3; rank 4 → 4/5; capstone → 6/9; ranks 3/5 are P2
  efficiency perks with no capacity. Clamps every flask to the new cap under
  the lock (:270-277).

## 5. The Field Kit menu (:334-388, :1447-1939)

- **Openers**: (a) **alchemy-station takeover** — `MenuSink` (:1978) catches
  `CraftingMenu` opening, checks the occupied furniture's bench type
  (`kAlchemy`/`kAlchemyExperiment` :1993-1995), `kHide`s the vanilla menu
  (:1998-2000), opens station mode, and **ejects the player from the
  furniture** (:2008, `IdleForceDefaultState`) — seated with no crafting
  menu, the engine re-activates CraftingMenu every few seconds and each
  ~1-frame re-open flashes vanilla chrome through the overlay (the v0.14
  station glitch). `CloseFieldKit` ejects again on close (:377-388).
  (b) The **lesser power** via `CastSink` (:1962) = FIELD mode: view + at
  most one flask change per trip, change starts empty. (c) Optional
  keyboard/pad openers (:392-397; pad default 0 — Deck View=Select collision).
- **`OpenFieldKit`** (:356) resets the field counter, dumps every slot's
  charge state to the log (the field-open diagnostic), and queues
  `RecomputeCapacity("open")`.
- **`DrawFieldKit`** (:1575) — render thread: essence stores, flask slots
  (charge cap SNAPSHOTTED under `g_flasksLock` :1629-1633 — RecomputeCapacity
  writes it from the task thread), discovered variants under
  `g_discoveredLock` (:1660) with per-variant cost and coating/affordability
  gating; a click queues `ConfigureFlask` onto a task (:1699-1705). Four
  skins (:1479-1508, `iMenuStyle`), real typefaces baked at backbuffer scale
  in D3DInit (:1749-1770).

## 6. Hooks, threads, locks

MAO installs **three trampoline call-hooks** (`menuhook::Install` :1930,
`AllocTrampoline(64)`, called from `SKSEPluginLoad` :2305 before the renderer
exists) **plus one vtable write** at kDataLoaded:

| Hook | Site | Thread it puts us on |
|---|---|---|
| D3DInit | RelocationID(75595, 77226) + VariantOffset(0x9, 0x275) (:1782-1783) | init (once) — ImGui standup |
| DXGIPresent | (75461, 77246) + 0x9 (:1814-1815) | **render** — draws the kit while open |
| InputDispatch | (67315, 68617) + 0x7B (:1919-1920) | input — feeds ImGui, swallows all input while open (:1913-1915) |
| DrinkPotion | `PlayerCharacter::VTABLE[0]` vfunc **0x10F** `write_vfunc` (:1158-1159) | caller's thread (game main) — the consume intercept |

The render/input site IDs are MEO's proven 1.6.1170 addresses; the DrinkPotion
index is SSE/AE-only (VR guard :1154). Everything else is **event sinks**
registered at kDataLoaded (:2223-2228): `TESContainerChangedEvent`,
`TESSleepStopEvent`, `TESSpellCastEvent`, `MenuOpenCloseEvent`,
`TESHarvestedEvent::ItemHarvested`. No other vanilla-function detours.

**Thread/lock map:**

- **Engine mutation happens ONLY on the main thread** via
  `SKSE::GetTaskInterface()->AddTask` — sinks compute then queue (:1336,
  :1384), menu clicks queue (:1703), the refill timer queues (:1009),
  MenuSink's takeover/config work queues (:1987, :2015).
- **`g_flasksLock`** (non-recursive): task thread writes, render thread and
  the drink hook read; snapshot-then-act around anything that can re-enter a
  sink (SyncFlaskItems :912-927).
- **`g_discoveredLock`**, **`g_effectCostLock`**, **`g_effectPotionLock`**:
  short, data-only critical sections.
- **Atomics** for everything ReadConfig can rewrite mid-session (:307-314,
  :392-399), the menu open/station state (:334-339), and the cached perk
  flags (:180-192).
- Known exemption: **`g_pouch`'s three counters are plain u32s** — written on
  the task thread, read raw by the render thread (:1620) and the sink log
  lines. Aligned-scalar reads don't tear on x86-64, but this is the same
  class the tuning globals were atomicized for; fold it in on the next pass.
- The detached refill sleeper (:1005-1011) does nothing but sleep and
  re-queue onto a task.

## 7. Configuration (:1176-1251)

- Two files, last-wins (:1241): `Data/SKSE/Plugins/MAO.ini` (dev/seed) then
  `Data/MCM/Settings/MAO.ini` (MCM Helper's persisted store). Re-read live
  on **JournalMenu close** (:2011-2018 — MCM Helper flushes on close), then
  `RecomputeCapacity("mcm")`. Also read at SKSEPluginLoad (:2304, seeds the
  hotkey before the render hooks) and kDataLoaded (:2222).
- **Reset-then-parse** (:1236-1240): `g_perkDebug`, `g_perkWantMask`,
  `g_conversionEnabled`, `g_notify` are reset to defaults each pass so an
  ABSENT key reverts instead of sticking at its last live value.
- `ApplyIniLine` (:1176): BOM strip (:1180), every numeric value CLAMPED at
  parse (:1206-1217 — garbage can't become an out-of-range 0.0); unknown
  `bPerk*` keys map to debug want-mask bits (:1221-1230).

## 8. Co-save & load (:2027-2217)

**Serialization** (`'MAO1'`, schema v3):

| Record | Contents |
|---|---|
| `'POCH'` | base, catalyst, apex (3 × u32) (:2035-2037) |
| `'BLPT'` | count + discovered potion FormIDs (v3; v1 = effect-keyed, v2 = +rep potion — both still readable, migrated at :2117-2144) |
| `'FLSK'` | flaskCount, chargesPerFlask, then ALL 6 slots × {blueprint, repPotion (v2+), charges} (:2054-2060) |

`LoadCallback` (:2067) — the MEO disciplines, adopted wholesale (v0.18.2):

1. **`readOk`** verifies every `ReadRecordData` against `sizeof` (:2077);
   short reads reset POCH (:2087-2090), drop BLPT's remainder (:2110-2113),
   leave FLSK's remaining slots empty (:2168-2171).
2. **Bound counts**: BLPT count sanity-capped at 65536 (:2099-2105 — a
   corrupt count would otherwise drive 2^32 iterations); FLSK header clamped
   (`g_flaskCount ≤ kMaxFlaskSlots`, charges [1,99] :2157-2158 — flaskCount
   bounds hot loops and indexes `g_flasks[]`).
3. **`ResolveFormID` on every stored FormID** (:2114, :2133-2135,
   :2173-2174); unresolved → drop, never guess.
4. **Newer-version records**: skipped with `g_newerCoSave` set (:2079-2085);
   kPostLoadGame shows an ON-SCREEN box (:2284-2289) — SKSE does not
   round-trip unread records; saving destroys them.
5. Flask reps are backfilled into `g_discovered` (:2181-2197) so a migrated
   slot's variant stays re-selectable.

`RevertCallback` (:2202) zeroes everything save-scoped and re-arms the
warning. **Post-load task order** (:2277-2290): `GrantFieldKitPower` →
`RecomputeCapacity("load")` **BEFORE** `SyncFlaskItems` — capacity is
authoritative before items are granted, so a shrunk kit can't grant now-dead
slots.

## 9. Generator, installer, release (the off-DLL half)

- **ESP generator** `MAO_GenerateESP.py` (byte-built, never hand-edited;
  MANUAL_MOD_CREATION_GUIDE.md is the format reference). ESL-flagged, master
  Skyrim.esm. **The FormID band is a DLL contract** (generator :75-82 ↔ DLL
  :84-86/:160-174): **0x800** inert MGEF + **0x801** Open Field Kit SPEL
  (dormant), **0x810–0x815** the six flask ALCH forms, **0x816** capstone
  PERK, **0x817** MCM QUST (VMAD → `MAO_MCM.pex`, the SkyUI registration
  vehicle), **0x818–0x823** the flag perks in `MAO_PERKS` order (:243-256),
  `NEXT_OBJECT_ID` 0x824 — forms are only ever ADDED. Each perk carries a
  `CTDA GetBaseActorValue(Alchemy) ≥ req` (:259-267) mirroring the DLL's
  auto-grant thresholds; ranked Calibration perks chain via NNAM. The
  generator also reads `kPluginVersion` out of plugin.cpp and **stamps it
  into the MCM config's Debug page** (:29-69) so the MCM readout can never
  drift from the DLL.
- **Installer** (`installer/`, C# Mutagen): `MAO.Installer` (standalone CLI:
  stats/tree/tree-effects/perk/write-tiers/write-patch, MO2-profile AND
  plain-game-root load-order resolution incl. `Skyrim.ccc`) and
  `MAO.Synthesis` share `Commands.Patch.cs` + `Commands.Tiers.cs` via
  `<Compile Include>` — no fork, byte-identical outputs.
  - **`ApplyPatch`** (Commands.Patch.cs:40) rewrites the winning AVAlchemy
    tree into `MAO - Patch.esp` (ESL): craft perks are classified **BY
    BEHAVIOR** (entry points `ModAlchemyEffectiveness`, `ModPotionsCreated`,
    `ModInitialIngredientEffectsLearned`, `PurifyAlchemyIngredients`,
    `ModPoisonDoseCount`, plus `GetIsObjectType == Ingredient` effect
    conditions — the census is Docs/PERK_TREE_RECON.md), never by name.
    NNAM rank chains walked with a cycle guard (:87-97); survivors go
    through keep/drop curation persisted in a choices JSON (Synthesis =
    non-interactive keep-all); kept perks with dangling HasPerk conditions
    get overrides; kept orphans reparent to root; MAO's nine nodes lay out
    with choices in parallel at gridY=1 and the two sequential lines
    (Vanguard→Corrosive, FieldExtract→Pouch) growing downward (:226-276).
  - **`WriteTiers`** (Commands.Tiers.cs:31) writes `mao_tiers.json`:
    obtainability = harvest×60 + container×20 + min(lvli,3)×4 (:80-83);
    0-source ingredients go straight to Base (:97-99, quest leftovers);
    rarity = **0.15 × avail-rank + 0.85 × value-rank** (:117-121, the tuned
    blend); percentile cuts Apex 12% / Catalyst next 30% (:126-132).
  - Both self-exclude `MAO - Patch.esp` from the read (Program.cs:63,
    Synthesis:84); MAO.Synthesis builds its OWN tier load order including
    `Skyrim.ccc` (Synthesis omits CC plugins — MEO measured ~24% loss and
    CC packs carry many MAO ingredients).
  - **Root `MAO.sln` contains ONLY the Synthesis project; `assets/MAO.synth`
    SelectedProject matches the .sln entry VERBATIM (backslashes)** — the
    trap that shipped MEO's Synthesis path broken for three versions. CI
    (`installer.yml`) asserts the agreement and builds through the solution
    exactly as Synthesis does.
- **Release / deploy**: `tools/release_native.sh` — immutable
  `releases/<ver>/` (refuses an existing dir), CI DLL + regenerated ESP +
  fonts + FOMOD info, completeness gate. `tools/package_deck.sh` — the
  mod-manager zip for the Deck modlist (same gate; version read from
  `kPluginVersion`). `tools/deploy_deck.sh` — flat test deploy; its Settings
  step **backfills new MCM keys** via `tools/merge_mcm_settings.py` (MCM
  Helper reads a key absent from an existing Settings ini as OFF/zero, so
  new controls look dead without the backfill). `mao_tiers.json` is
  per-load-order and deliberately NOT in the zips — the installer generates
  it on the user's machine.
