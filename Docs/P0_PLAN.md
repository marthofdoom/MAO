# P0 Plan — the Gathering Loop

P0 validates MAO's load-bearing assumptions before any flask, blueprint, or
perk work begins: **ingredient gathering interception**, **essence storage in
the SKSE co-save**, and the **full hook inventory** for everything the design
will eventually need. If gathering can't be intercepted cleanly on every
acquisition path, the whole "no ingredient inventory" premise fails — find
that out first.

Doctrine inherited from MRO (`../Requiem-modification/docs/NATIVE_REWRITE_PLAN.md`):
never invent — every hook maps to a published open-source reference. Event
sinks before code hooks (lowest crash surface). Every hook individually
toggleable via `MAO.ini`. One hook per release; a CTD bisects to one change.
Any future `write_call` thunk site must pass MRO's
`tools/verify_hook_site_live.py` against the running game before shipping
(offline verify is impossible — Steam DRM encrypts the on-disk exe).

## Hook inventory (feature → reference source → phase)

The complete map for the design. P0 builds only the first three — note they
are **all event sinks and SKSE interfaces, zero code hooks**, so P0 carries
MRO-M1-level risk, not M2-level.

| # | Feature | Hook | Reference (public source) | Phase |
|---|---------|------|---------------------------|-------|
| 1 | Flora/creature harvest detection (source tagging for Field Extraction +10% / Pouch Expansion 15% Tier II proc) | `RE::TESHarvestedEvent::ItemHarvested` sink | po3 PapyrusExtender (`OnItemHarvested`), CommonLibSSE-NG header | **P0** |
| 2 | Universal ingredient acquisition → essence conversion (harvest, corpse loot, container, world pickup, barter, script `AddItem`) | `RE::TESContainerChangedEvent` sink via `ScriptEventSourceHolder` | The most documented CommonLibSSE event; QuickLoot EE, po3 both sink it. MEO `native/plugin.cpp` shows our own sink registration pattern | **P0** |
| 3 | Essence pouch persistence | SKSE `SerializationInterface` versioned record (`'POCH'`) | MEO `native/plugin.cpp` `SaveCallback`/`LoadCallback` — proven live incl. a v3→v4 migration | **P0** |
| 4 | Potion pickup → destroy + blueprint unlock + essence yield | Same container-changed sink, `AlchemyItem` branch | Mechanism proven by #2 in P0; only the branch is new | P1 |
| 5 | Flask drink intercept (apply payload natively, decrement charge) | Equip/consume pipeline hook (`write_vfunc` preferred over call-site thunks per MRO #1 rejection) | valhallaCombat hook style; po3 equip events as the fallback detection layer | P1 |
| 6 | Refill triggers | `RE::TESSleepStopEvent` sink + SKSE task/update timer | Standard sink + the MEO background-timer pattern | P1 |
| 7 | `GetItemCount` faking + `EquipItem` consumption intercept for automation mods | Inventory-count vfunc hooks — the riskiest surface in the design | To be sourced during P3; if no maintained reference exists, the compat contract gets renegotiated rather than reverse-engineered | P3 |

## P0 architecture

One DLL (`MAO.dll`), built on MEO's CI pipeline (`.github/workflows/native.yml`,
CMake + vcpkg + CommonLibSSE-NG, windows-latest runner — copy, don't rewrite).
No ESP required for P0 if notifications come from the DLL
(`RE::DebugNotification`); decide when scaffolding.

- **The pouch**: three `std::uint32_t` counters (Base / Catalyst / Apex) in the
  DLL, serialized as a versioned `'POCH'` record. Revert callback zeroes them
  (new game / load ordering safety).
- **Conversion rule**: when a `TESObjectREFR`-to-player container-changed event
  carries an `IngredientItem`, remove it from the player and credit the pouch.
  **Tier picks the bucket; the ingredient's gold value picks the amount** — a
  rarer, more valuable ingredient yields more essence than a cheap one *within
  the same tier* (Nightshade credits more Catalyst than Deathbell if it's worth
  more). Yield derives from `IngredientItem` value (`goldValue`), passed
  through a per-tier curve (`essence = ceil(value * tierRate)`, rates TBD and
  MCM-tunable later). Applies identically on pickup and on vendor purchase, so
  buying an expensive ingredient costs gold for proportionally more essence.
  Log every conversion to `MAO.log`; fire a per-pickup debug notification
  (`+3 Base Essence (Blue Mountain Flower)`) — see notification note below.
- **Double-credit guard**: a harvest fires #1 *and* lands the item in
  inventory, firing #2. Rule: **#2 is the only place that credits**; #1 only
  tags the acquisition as "harvested" for the perk bonuses that will care
  later. P0 must prove one harvest = one credit.
- **Tier map**: hardcoded for P0 — everything Tier I except a token Tier II
  set (Deathbell, Nightshade) and Tier III set (Daedra Heart) to exercise all
  three buckets. Pick the Tier II tokens with *different* gold values so the
  value-weighted yield is visible in testing. P1 generates the real map from
  the masters (MEO catalog pattern; `DYNAMIC_OR_DROP.md` applies).
- **Exclusion list**: quest-critical ingredients must NOT convert (Crimson
  Nirnroot / "A Return To Your Roots" is the canonical case; also anything
  quest-flagged). P0 ships a hardcoded FormID exclusion list + a quest-item
  flag check, and tests it.

## In-game test matrix (gates P1)

Console reads pouch state via `MAO.log` + on-screen notifications.

1. **Load**: DLL loads on 1.6.1170, logs its version and game version. (M0)
2. **Harvest flora** (Blue Mountain Flower): no ingredient in inventory,
   `+N Base` notification, exactly one credit (double-credit guard).
3. **Loot a corpse ingredient** (e.g. Skeever Tail from a kill): converts.
4. **Take from a container** (chest with an ingredient): converts.
5. **Pick up a world-placed loose ingredient**: converts.
6. **Buy an ingredient from an alchemy vendor**: converts on transfer, same
   value-weighted yield as pickup.
7. **`player.additem` an ingredient**: converts (proves script-path coverage).
8. **Tier routing**: Deathbell → Catalyst; Daedra Heart → Apex.
9. **Value weighting**: two same-tier ingredients of different gold value
   yield different essence amounts (cheaper < pricier).
10. **Follower scope**: a follower looting an ingredient converts nothing —
    player pouch unchanged, and the ingredient does not linger as a player
    item (essence is never an inventory object; it exists only in the pouch
    surfaced on menus).
11. **Exclusion**: Crimson Nirnroot enters inventory normally, no conversion.
12. **Persistence**: gather, save, quit to desktop, reload — counts survive.
13. **New game / revert**: counters start at 0; loading an older save after a
    newer one doesn't leak counts across (revert callback).
14. **Co-save absence**: loading a pre-MAO save = empty pouch, no crash, log
    line notes fresh init.
15. **Field Kit power**: the "Open Field Kit" power is granted to the player and
    appears in the Powers list; casting it opens the ImGui menu (no CTD — the
    present-hook gate).
16. **Essence viewer**: the open menu shows the three essence counts matching
    the co-save / log; gather more, reopen, counts update. Menu is read-only
    (no flask actions yet) and closes cleanly, game resumes.

## The Field Kit UI framework (see DESIGN §3.3)

The Flask Kit is a **lesser power → ImGui menu**, reusing MEO's render/input
framework wholesale (D3D11 present thunk, power-cast `TESSpellCastEvent` open,
task-deferred, MCM-via-INI). Only the menu content is MAO's. This is a **code
hook**, not an event sink — higher risk than the gathering sinks, must pass
`verify_hook_site_live.py`, and per MRO doctrine ("one hook per release, a CTD
bisects to one change") it should land as its own isolated step, not tangled
into the gathering sinks.

**P0 is combined** (user, 2026-07-08): the gathering loop **and** a read-only
essence-viewer menu ship as one P0 — the Field Kit power opens the reused ImGui
menu and displays the three essence counts gathering produces. No flask setup
yet (that's P1); the viewer is read-only and cannot corrupt game state.

Build order within P0 (preserves clean CTD diagnosis despite the single
milestone): **get the gathering sinks + `'POCH'` co-save green and tested
first, THEN wire the render/present hook + viewer on top.** Commit and, ideally,
grab an in-game smoke test at the sink stage before the code hook goes in — so
if a crash appears once the present thunk lands, it bisects to the render hook,
not the sinks. Shipping them as one P0 doesn't mean writing them as one blob.

## Explicitly NOT in P0

Flask *setup*/configuration, charges, drinking, blueprints, potion
stripping/interception, perk overrides, refill timers, the full MCM option set,
FOMOD, Requiem patch. The ESP (if one exists at all in P0) contains nothing but
identity. (The read-only essence *viewer* menu **is** in P0 — see above — but
flask *setup* is not; the menu shows essence counts only.)

## Decisions (resolved 2026-07-08)

1. **Vendor barter → convert, value-weighted.** Bought ingredients convert on
   the same footing as picked-up ones. Essence yield scales with the
   ingredient's gold value within its tier (rarer/pricier = more essence), so
   the vendor path is an essence-for-gold trade priced by rarity, not a flat
   shop. Yield model lives in the conversion rule above.
2. **Player-only conversion.** Only ingredients reaching the *player* convert;
   followers/NPCs are ignored. Rationale (user): essence must never be an
   inventory item — it exists solely in the abstracted pouch shown on menus,
   so there is no cross-actor essence object to move around.
3. **Per-pickup notifications for P0**, but treat this (and most surface
   behavior like it) as INI/MCM-toggleable — the toggle framework arrives with
   the P1 MCM; P0 hardcodes per-pickup + always-on `MAO.log`.
4. **Flask Kit is a power → ImGui menu, reusing MEO's framework** (DESIGN
   §3.3). Same power-cast-opens-menu model and MCM-via-INI surface as MEO's Gem
   Pouch; MAO re-skins the layout to show essence stores + flask slots +
   blueprints. Resolved 2026-07-08.

5. **Combined P0 with read-only essence viewer** (user, 2026-07-08). Gathering
   loop + viewer menu ship as one P0; flask setup stays P1. Build sinks first,
   render hook second (see Field Kit UI framework section for the bisection
   rationale).
