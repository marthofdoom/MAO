The coating mechanics have been restructured into a time-based model, completely decoupling them from hit count metrics. They now offer robust, sustained durations of **45 seconds** and **120 seconds** respectively, utilizing standard, instant vanilla poison-application application rules rather than forcing the player into a clunky drinking state.

*Field Extraction* has been re-tuned from a volatile RNG proc into a highly reliable, flat **10% yield increase**. The automatic-use functions and status-clearing perks are entirely removed, replaced with high-engagement manual alternatives. Finally, a dedicated framework section has been added to outline exactly how the native C++ DLL exposes the flask architecture to third-party automatic potion mods.

---

# Marth's Alchemy Overhaul (MAO) — Design Document

Flask-style alchemy preparation for Skyrim SE. All single-use potions are removed from world loot and vendors. Potions become permanent, reusable **Flasks** that players charge with abstracted **Material Essences** gathered in the field. This completely replaces vanilla alchemy crafting.

**Target:** Vanilla SE + SKSE core, with a Requiem/LoreRim compatibility plugin as a FOMOD option. Built using the established sibling toolchain (Python ESP record generator, CommonLibSSE-NG MSVC pipeline via GitHub Actions, and SKSE co-save serialization).

### Why Native (The Core Architecture)

1. **Zero Inventory Bloat:** Instead of the Papyrus VM tracking thousands of individual item instances, dynamic potion IDs, and custom names, the entire field kit configuration lives in the C++ DLL and serializes directly to the **SKSE co-save** (`.skse`).


2. **Instant Field Evaluation:** When a player drinks from a flask, the hook intercepts the item use natively, applying the dynamic, data-driven magnitudes immediately without relying on slow Papyrus active script latency during high-stress combat.



---

## 1. The Material Spine & Harvesting Loop

To eliminate inventory paralysis and endless ingredient sorting, harvesting flora or looting creatures bypasses standard inventory items. The native layer intercepts the harvest event and increments a flat currency counter stored in the player's abstracted **Alchemical Pouch**.

### The Three Essence Tiers

* **Tier I: Base Essence**
* **Primary Sources:** Common field flora, local fungi, mundane creature drops.


* **Default Purpose:** Establishes the baseline effect profile (e.g., standard restoration, basic elemental padding).




* **Tier II: Catalyst Essence**
* **Primary Sources:** Rare plants (Nightshade, Deathbell), specialized creature drops (Falmer ear, Sabre Cat tooth).


* **Default Purpose:** Amplifies durations, introduces secondary sub-effects, or acts as an efficiency bridge.




* **Tier III: Apex Essence**
* **Primary Sources:** Elite monstrosities and boss-level encounters (Dragon Priests, Vampires, Dragons, Automaton cores).


* **Default Purpose:** Unlocks absolute ceiling magnitudes, allowing unperked characters to challenge Requiem caps.


* **The Scarcity Guardrail:** Without high-level Alchemy perks, using Tier III essences to charge a flask requires an astronomical amount of material. It is a highly deliberate, costly tactical choice for an unperked character, preventing early-game exploitation of boss drops.



---

## 2. The Magnitude Scaling Model & The Perk Handshake

To preserve the de-leveled integrity of a Requiem setup, MAO adheres to a strict balance boundary regarding standard potion ceilings and over-cap progression.

* **The Unperked Ceiling (The Apex Route):** A player who has spent 0 points in Alchemy can achieve standard highest-quality Requiem potion caps. However, doing so requires burning unmitigated, raw Tier III Apex Essences at an incredibly inefficient exchange rate.


* **The Standalone MAO Boundary:** Standalone MAO *never* allows a player to scale magnitudes past the standard highest-quality vanilla/Requiem potion limits.
* **The MRO Over-Cap Integration:** Breaking through standard caps to reach absolute late-game power fantasies is gated entirely behind **Marth Requiem Overhaul (MRO)** integration frameworks.



### The Structural Shift of Perks

Instead of perks scaling raw magnitude numbers infinitely, progression inside MAO targets the utility and footprint of your field kit:

* **Kit Capacity (Primary Focus):** Unlocks more permanent Flask slots in your inventory, giving you a wider toolkit of options for a dungeon run.
* **Charge Volume (Primary Focus):** Increases the number of drinks a specific Flask holds before going dry, expanding your combat endurance.
* **Resource Efficiency (Secondary Focus):** Mild-to-moderate cost-reduction nodes. Higher-level perks drastically reduce the raw essence cost of Tier III ingredients, turning Apex mixtures from a desperate, costly gamble into a sustainable endgame asset loop.

```
[No Perks]      --> 2 Flasks | 2 Charges | High Tier III Cost | Caps at Requiem Max
[Mid Perks]     --> 4 Flasks | 5 Charges | Moderate Tier III Cost | Caps at Requiem Max
[Max MAO]       --> 6 Flasks | 9 Charges | Low Tier III Cost | Caps at Requiem Max

```

---

## 3. Flask Mechanics & The Refill Engine

Flasks are permanent, static inventory items managed entirely via native memory blocks. Their operational payload and current resource states are tracked natively in the C++ DLL and serialized directly to the SKSE co-save (`.skse`).

### 3.1 Instant Reallocation & Payload Overwrite

Opening the Field Kit interface allows the player to instantly change a Flask's assigned effect profile at any time outside of active combat.

* **The Overwrite Penalty:** Reallocating a Flask that still contains active charges instantly purges those remaining charges. The previous mixture's ingredients are completely lost with no reclamation path.
* **Upfront Cost:** Configuring the new payload immediately deducts the required material essences from the player's Alchemical Pouch.



### 3.2 The Automated Refill Pipeline

Once a Flask hits 0 charges, it enters a dry state. Flasks automatically attempt to refill their internal charge volume via two game engine triggers managed by a lightweight native background timer:

1. **Rest State Trigger:** Sleeping immediately initiates a full kit check.
2. **Real-Time Clock Trigger:** Every 5 minutes of real-world time passing triggers a background kit maintenance pass.

### 3.3 The Field Kit Interface (UI)

The Field Kit is opened by **taking over the alchemy station**: activating any
alchemy station (bench type `kAlchemy` / `kAlchemyExperiment`) hides the vanilla
crafting menu and opens the Field Kit instead. This reuses the sibling MEO menu
architecture wholesale (MEO's proven enchanting-station takeover); only the menu
*content* differs.

* **Station → menu:** a `MenuOpenCloseEvent` sink catches the vanilla
  `CraftingMenu` opening, checks the occupied furniture's bench type, dismisses
  the vanilla menu (`UIMessageQueue` → `kHide`), and opens the Field Kit.
  Closing (B / Esc) forces the player up out of the furniture. This also blocks
  vanilla alchemy crafting — which the mod replaces anyway — closing the
  craft→convert essence exploit.

> **Opener history:** P1a shipped a lesser-power opener; it was **retired**
> (2026-07-13) in favour of station takeover (Marth: "instead of the power, not
> in addition"). The `SPEL` form stays frozen but dormant in the ESP and is
> removed from saves that got it. Optional keyboard/gamepad button fallbacks
> remain (`iOpenHotkey` / `iOpenButtonGamepad`); the **gamepad button is
> disabled by default** (on the Steam Deck the View button doubles as Select).
* **Rendering:** a self-contained **ImGui** menu drawn through a D3D11
  DXGI-present hook with its own input routing — the exact render/input
  framework MEO already ships. MAO re-skins the layout; the plumbing (device
  grab, present thunk, ImGui init, task-deferred open) is copied, not
  re-derived. NOTE: this is a code hook (the present-call thunk), not an event
  sink — it carries higher risk than the gathering sinks and must pass
  `verify_hook_site_live.py`, and per MRO doctrine lands as its own isolated
  milestone so a CTD bisects cleanly.
* **Layout (MAO-specific content):** the menu shows the **essence stores**
  (Base / Catalyst / Apex counts from the Pouch), the **flask slots** with each
  slot's assigned blueprint and remaining charges, and the **known blueprints**
  available to assign. Actions: configure/reallocate a flask (with the §3.1
  overwrite penalty and upfront essence cost applied natively), consistent with
  the Gem menu's socket/unsocket action model.
* **MCM parity:** configuration uses the same INI + MCM Helper settings surface
  as MEO (a dev/seed `SKSE/Plugins/MAO.ini` plus the MCM Helper file read last
  so it wins; re-read live on menu close), including a menu-skin/style option.
  The MCM option *set* is MAO's own (essence/flask toggles), but the mechanism
  is inherited.

---

## 4. Recipe Acquisition & Blueprint Learning

To maintain the environmental progression built into the world layout, player capabilities are gated entirely by exploration and discovery. You cannot configure a Flask for an effect until you have analyzed its physical counterpart in the world.

### 4.1 The Native Discovery Hook

Standard consumable potions are stripped from merchant stocks and vendor tables, but they remain intact inside enemy death drops, boss chests, and environmental cell placements.

* **The Intercept:** When the player picks up a physical potion or opens a container containing one, the C++ DLL intercepts the event.


* **The Conversion:** The physical single-use potion is instantly destroyed before it can enter the player's inventory, keeping the inventory clean.


* **The Payload Reward:** The player is immediately awarded a permanent **Blueprint Unlock** for that effect profile, alongside a direct yield of Material Essences matching the tier of the analyzed potion.



---

## 5. Perk Tree Rework & Bulk Capacity Milestones

Vanilla node positions are preserved, overriding the 13 vanilla/Requiem perk records in-place. The progression steps cleanly from an unperked baseline of **2 Flasks / 2 Charges** up to a master ceiling of **6 Flasks / 9 Charges**.

### 5.1 The Capacity Blueprint

* **Unperked (Base Setup):** 2 Flasks | 2 Charges
* **Perk 1 (The Initial Step):** 2 Flasks | 3 Charges (`+1 Charge` — The only single charge addition)
* **Perk 2 (The Second Step):** 3 Flasks | 3 Charges (`+1 Flask` — The only single flask addition)
* **Mid-Tree Milestone:** Bulk addition of `+1 Flask & +2 Charges` (Brings kit to 4 Flasks / 5 Charges)
* **End-Tree Milestone:** Bulk addition of `+2 Flasks & +4 Charges` (Brings kit to 6 Flasks / 9 Charges)

---

### 5.2 In-Place Perk Override Matrix

| Vanilla Perk | MAO Re-enlistment | Capacity Package | Tactical & Efficiency Effect |
| --- | --- | --- | --- |
| **Alchemist 1** | *Kit Calibration I* | **+1 Max Charge** | **The First Step:** Sets baseline hardware to 2 Flasks / 3 Charges. Unlocks fundamental Tier I blueprint extraction. |
| **Alchemist 2** | *Kit Calibration II* | **+1 Max Flask** | **The Second Step:** Sets baseline hardware to 3 Flasks / 3 Charges. Unlocks Tier II blueprint extraction. |
| **Alchemist 3** | *Kit Calibration III* | *None* | **Resource Efficiency:** Tier I and Tier II material essences are 15% more efficient during automated refill windows. |
| **Alchemist 4** | *Field Deployment* | **+1 Max Flask** <br>

<br>**+2 Max Charges** | **The Mid-Game Milestone:** Bulk capacity burst. Automatically scales your operational kit layout to **4 Flasks and 5 Charges**. |
| **Alchemist 5** | *Kit Calibration V* | *None* | **Refill Optimization:** Tier I and Tier II essence maintenance costs are cut cleanly in half during resting checkouts. |
| **Physician** | *Fluid Motion* | *None* | **Manual Mobility:** Reduces the severe movement speed reduction penalty during the manual flask-drinking animation by 50%. |
| **Poisoner** | *Vanguard Coating* | *None* | **The Weapon Treatment:** Converts offensive blueprints into applied weapon coatings instead of drinkable items. Non-drinkable form state. Lasts **45 seconds**. Uses vanilla poison application visual overlays instantly without custom drinking animations. |
| **Benefactor** | *Apex Stabilization* | *None* | **Endgame Efficiency:** Reduces the exorbitant baseline material cost of utilizing rare Tier III Apex Essences by 35%, making boss-tier mixtures sustainable. |
| **Experimenter** | *Field Extraction* | *None* | **Precision Yield:** Highly optimized collection routines. Extracting wild flora or looting slain creatures yields a guaranteed flat **10% more** material essence currency to your pouch. |
| **Concentrated Poison** | *Corrosive Retention* | *None* | **Coating Longevity:** Greatly extends weapon treatment durations. Active blade and arrow coatings now persist for **120 seconds**, allowing them to comfortably last through multi-stage engagements. |
| **Green Thumb** | *Pouch Expansion* | *None* | **Sifting Pouch:** Harvesting wild flora grants a flat 15% chance to automatically generate a matching Tier II Catalyst Essence alongside the base tier drop. |
| **Snakeblood** | *Extended Synthesis* | *None* | **Sustained Metabolization:** Deepens the efficacy of your field prep. All drinkable utility and restorative flask buff durations are extended by a flat **30 seconds**. |
| **Purity** | *Master Alchemist's Crucible* | **+2 Max Flasks** <br>

<br>**+4 Max Charges** | **The Master Milestone:** Bulk capacity burst. Hits the absolute mechanical ceiling of **6 Flasks and 9 Charges**. |

---

## 6. Compatibility & Automated Potion Mod Integration

Because MAO replaces standard single-use potion items with permanent, charge-depleting Flask hardware, the mod implements explicit code patterns to ensure native compatibility with popular automated consumption utilities (e.g., *Smart Potion*, *Auto PV*, *Automated Potion Consumption*).

### 6.1 Form Dummy Mirroring & Item Flags

To guarantee that third-party Papyrus scripts and independent SKSE plugins can detect, categorize, and execute flask drinks without manual patching, the C++ DLL mirrors flask data directly onto standard engine hooks:

* **The Potion Type Flag:** Configured flask items always maintain their base `AlchemyItem` (Potion) record types in memory. They are never converted into custom script types or script-heavy active magic effects.
* **Charge Inventory Faking:** When an automated potion mod queries the player inventory via `GetItemCount` or internal SKSE array iterations, the native layer intercepts the query. If a Flask has $\ge 1$ charge, the engine reports an inventory count of `1`. If the Flask is dry (0 charges), it reports `0`.
* **The Consumption Intercept:** When an external automation tool forces the player actor to consume a flask via `Actor::EquipItem`, the native hook catches the call immediately. It bypasses standard inventory destruction logic, applies the blueprint payload directly to the actor's active magic effect graph, and subtracts 1 charge natively from the SKSE co-save tracking register.

### 6.2 Load-Order-Aware Installation

MAO installs *on top of* whatever alchemy implementation the load order already
has — vanilla, Requiem, or another overhaul — and should **detect that
implementation and replace it smartly where applicable**, rather than blindly
stacking and hoping conflict resolution wins. The mechanism is a later-build
concern (P3 packaging), but the design commits to it now so earlier phases
don't make choices that preclude it:

* **Detection:** the FOMOD installer (and/or the DLL at `kDataLoaded`) probes
  for known alchemy overhauls by plugin name / signature records and by the
  presence of the perk records MAO intends to override, then selects the
  matching compatibility profile instead of assuming vanilla.
* **Smart replacement:** where MAO owns a system (the 13 alchemy perk records,
  potion loot, ingredient behavior) it overrides in place with the detected
  baseline's records as the diff source — so, e.g., a Requiem load order keeps
  Requiem's de-leveled integrity and MAO's over-cap gating defers to MRO,
  exactly as the magnitude model already specifies (§2). Where another mod owns
  a system MAO doesn't touch, it leaves it alone.
* **Portability caveat:** any detection baked from *this* machine's load order
  at generation time is subject to the `DYNAMIC_OR_DROP` rule — it must resolve
  at install/runtime against the user's actual load order, not ship a decision
  hardcoded from the developer's setup.

This is the same load-order-style adaptivity intended for the sibling MRO
(detect deleveled vs. encounter-zones vs. vanilla-leveled and tune from there);
MAO inherits the principle for its alchemy footprint.
