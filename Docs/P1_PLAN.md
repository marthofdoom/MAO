# P1 Plan — Core (flasks, variants, the real system)

> **Status (2026-07-13):** **P1 COMPLETE** and in-game tested. P1a–P1d (tag
> `v0.8.2-p1d`), alchemy-station takeover opener (v0.9.0), compound recipe-based
> economy (v0.12.x), **P1e** perk-driven capacity + in-place perk rename
> (v0.13–v0.15.1, Fable-reviewed), and **P1f** the MCM (v0.15, MCM Helper page +
> per-perk debug override). The flask loop is playable end-to-end. **Next: P2**
> (coatings) and the MEO-style Mutagen installer (full load-order-aware perk
> override — descriptions, effect neutralization, tree placement of the MAO
> capstone). Two design shifts from the original plan below: the opener is
> the **alchemy station**, not a power (DESIGN §3.3); and a flask embodies a
> **specific discovered potion variant** (you must have found it), not one
> derived representative.


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
| **P1d** | **Real drinkable flasks + refill** (user chose real items, 2026-07-13). **Permanent-item approach** — the count-as-charges shortcut is REJECTED: a dry flask would hit 0 count and vanish, dropping it from **Favorites / Wheeler** and losing the binding on refill (user requirement: the flask stays bound when empty). So: distinct flask `AlchemyItem` forms per slot in the ESP (favorite/Wheeler each independently), granted on configure and **never consumed** (count stays 1). Charges tracked natively in `'FLSK'`. Drinking is intercepted (the §6 consume/`EquipItem` code hook): apply the payload — cast the blueprint's **representative discovered potion** via `ActorMagicCaster::CastSpellImmediate` — decrement the native charge, and block the vanilla consume so the item persists. A dry flask stays in inventory (bindings intact), does nothing on use, until refill tops the charge back up from essence on `TESSleepStop` + a timer. Needs: 6 flask forms (frozen FormIDs); `'BLPT'` stores rep-potion FormID per blueprint (co-save bump); the consume code hook (address-library, `verify_hook_site_live`); sleep sink + timer; runtime rename via `ExtraTextDisplayData`. Higher-risk (first gameplay code hook) — land the consume hook as its own isolated step. | 6 flask forms; consume **code hook**; sleep sink + timer | Flask stays favorited/on Wheeler when empty; drinking applies effect + depletes without consuming; sleep/timer refills |
| **P1e** ✅ | **Perk-driven capacity + efficiency (native).** DLL reads the vanilla Skyrim.esm alchemy perks and recomputes flask/charge capacity from them (Alchemist chain → 2/2→4/5, Purity → 6/9; Benefactor −35% Apex cost, Experimenter +10% gather). Recomputed on load + menu open; holds co-saved capacity when perks don't resolve (Requiem). **Deferred:** the ESP cosmetic perk-rename ("Kit Calibration" etc.) and the coating/drink/duration perks (need P2 systems). | PERK reads (HasPerk) | Perks grant the capacity ladder (2/2 → 6/9) |
| **P1f** ✅ | **MCM (MCM Helper).** Field Kit page (essence tax, cost rate, catalyst/apex thresholds, refill cadence, menu skin, notifications) + a Debug page (master perk-override toggle + per-perk toggles). Registers via a start-game QUST (`MAO_MCMQuest`) carrying an empty `MCM_ConfigBase` script (`MAO_MCM.pex`) — MCM Helper reads `Config/MAO/config.json` only for a mod with such a quest; a `Settings/MAO.ini` seed supplies defaults. DLL reads the settings INI + reloads on Journal(MCM) close. | MCM config + QUST/PEX | MCM page live; toggles drive the DLL |

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
