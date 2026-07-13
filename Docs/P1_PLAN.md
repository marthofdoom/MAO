# P1 Plan — Core (flasks, blueprints, the real power)

P0 proved the load-bearing native assumptions in-game (gathering on every path,
double-credit guard, value-weighted essence, co-save, and the ImGui render
hook). P1 builds the actual alchemy on top. Same doctrine as P0: **one hook /
form class per CI-green build, so a CTD bisects to one change**; each milestone
is deployed to the Deck and tested before the next.

P1 introduces the **first ESP**. `MAO_GenerateESP.py` generates it byte-by-byte,
ported from the sibling MEO generator (proven recipes — TES4, MGEF, SPEL, PERK,
FLST). The one principle holds: when a record misbehaves, dump it and diff
subrecords against a working vanilla twin. FormIDs are frozen from first commit
(co-saves and the ESP both reference them); forms are only ever added.

## Milestones

| # | Build | New surface | Gate |
|---|-------|-------------|------|
| **P1a** ✅ | **ESP foundation.** `MAO_GenerateESP.py` emits the minimal ESP: the *Open Field Kit* SPEL + one inert MGEF (FormIDs frozen 0x800/0x801, ESL). Proved the ESP toolchain in-game. **Outcome:** a granted power clutters the Powers select list, so the opener stays the **button** (DESIGN §3.3); the SPEL form is kept frozen but dormant, the cast sink wired. | first ESP; `TESSpellCastEvent` sink | ✅ ESP loads, spell resolves; power removed from select, button opens the kit |
| **P1b** | **Blueprint discovery.** The container sink gains an `AlchemyItem` branch: picking up a potion destroys it, grants a native (co-save) blueprint unlock for its effect profile + matching essence. The viewer lists known blueprints. | potion branch; `'BLPT'` co-save record | Pick up potions → blueprints appear + essence credited; potion leaves inventory |
| **P1c** | **Flasks + configuration UI.** Flask items (ESP `AlchemyItem`s, so auto-potion mods see them). The viewer becomes interactive (port MEO's full mouse/input routing): assign a blueprint to a flask slot, deduct essence, apply the §3.1 overwrite penalty. | flask forms; interactive menu | Configure a flask from known blueprints; essence deducted; state persists |
| **P1d** | **Real drinkable flasks + refill** (user chose real items, 2026-07-13). **Hook-free approach:** configuring a slot grants `charges` copies of the blueprint's **representative discovered potion**, renamed "Field Kit: <effect>" via `ExtraTextDisplayData`. Drinking is *vanilla* — the real potion applies the correct effect and the stack count decrements = a charge depleting (auto-potion mods see a real potion, so §6 count-faking is largely free). Charges = live `GetItemCount(repPotion)`, so no native charge var to desync ("the item is the database"). Refill tops the count back up from essence on `TESSleepStop` + a background timer. Constraint: a blueprint lives in one slot at a time (so count↔slot stays 1:1). Deviation from the "single permanent-when-dry item" design: a dry flask's item is absent until refill, but the **menu** is the management surface and always shows the slot — acceptable. True permanent-hardware + `EquipItem` intercept can come with the P3 §6 compat pass (where code hooks live). Needs: `'BLPT'` stores rep-potion FormID per blueprint (co-save bump); `TESSleepStop` sink + timer. | rep-potion storage; sleep sink + timer (no code hook) | Configure → drinkable renamed potions appear; drinking applies effect + depletes; sleep/timer refills from essence |
| **P1e** | **Perk override matrix.** The 13 vanilla/Requiem alchemy `PERK` records overridden in-place (DESIGN §5) — flask slots, charge volume, essence efficiency. | PERK records | Perks grant the capacity ladder (2/2 → 6/9) |
| **P1f** | **MCM.** The full option set via MCM Helper (the M1 INI surface grows here): tier rates, notification style, refill cadence, hotkey/power binding. | MCM config/quest | MCM page live; toggles drive the DLL |

## What stays native vs. ESP

- **Native (co-save):** essence pouch, blueprint unlocks, per-flask charge/
  payload state, gathering, discovery. No Papyrus.
- **ESP forms:** the power (SPEL+MGEF), flask items (`AlchemyItem`), the 13
  perk overrides. FormIDs frozen at `0x800+` (ESL-flagged plugin).
- **Deferred to P2+:** coatings, potion-loot stripping from vendors, the
  automated-potion-mod compat contract, FOMOD/load-order-aware install.

## Portability (DYNAMIC_OR_DROP)

Blueprint discovery keys off vanilla `AlchemyItem` effect profiles read at
runtime from the player's actual load order — not baked at generation time.
The P0 tier map (name-substring stand-in) is replaced here by a real map;
whatever generates it must resolve against the user's masters at install or
runtime, never hardcode this machine's setup.
