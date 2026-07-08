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
  carries an `IngredientItem`, remove it from the player and credit the pouch
  by tier. Log every conversion to `MAO.log`; fire a debug notification
  (`+2 Base Essence (Blue Mountain Flower)`).
- **Double-credit guard**: a harvest fires #1 *and* lands the item in
  inventory, firing #2. Rule: **#2 is the only place that credits**; #1 only
  tags the acquisition as "harvested" for the perk bonuses that will care
  later. P0 must prove one harvest = one credit.
- **Tier map**: hardcoded for P0 — everything Tier I except a token Tier II
  set (Deathbell, Nightshade) and Tier III set (Daedra Heart) to exercise all
  three buckets. P1 generates the real map from the masters (MEO catalog
  pattern; `DYNAMIC_OR_DROP.md` applies).
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
6. **Buy an ingredient from an alchemy vendor**: converts on transfer.
7. **`player.additem` an ingredient**: converts (proves script-path coverage).
8. **Tier routing**: Deathbell → Catalyst; Daedra Heart → Apex.
9. **Exclusion**: Crimson Nirnroot enters inventory normally, no conversion.
10. **Persistence**: gather, save, quit to desktop, reload — counts survive.
11. **New game / revert**: counters start at 0; loading an older save after a
    newer one doesn't leak counts across (revert callback).
12. **Co-save absence**: loading a pre-MAO save = empty pouch, no crash, log
    line notes fresh init.

## Explicitly NOT in P0

Flasks, charges, drinking, blueprints, potion stripping/interception, perk
overrides, refill timers, MCM, FOMOD, Requiem patch. The ESP (if one exists at
all in P0) contains nothing but identity.

## Open decisions for the user

1. **Vendor barter**: converting on purchase (test 6) is the consistent rule,
   but it means ingredient stock is effectively an essence shop. Confirm
   that's the intended economy, or exclude barter transfers.
2. **NPC/follower inventories**: P0 converts only player-received items.
   Followers looting ingredients keep them as items — acceptable long-term?
3. **Notification volume**: per-pickup notifications can get noisy in a
   flower field. Per-pickup, or batched ("+6 Base Essence" on a short timer)?
