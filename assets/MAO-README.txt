marth's Alchemy Overhaul (MAO)
==============================
Flask-based alchemy: gather essence, configure reusable flasks, coat weapons.

REQUIREMENTS
------------
  * SKSE64
  * Address Library for SKSE Plugins
  * SkyUI
  * MCM Helper
  * Synthesis  (https://github.com/Mutagen-Modding/Synthesis)
      MAO adapts its perk tree and ingredient tiers to your load order through a
      Synthesis patcher (see SETUP). Synthesis brings its own .NET dependency, so
      there is nothing extra to install for MAO and no program to run by hand.
      The patcher is STRONGLY RECOMMENDED but not strictly required — see
      "DID IT WORK?" for what you lose without it.
  * No DLC required. (Dawnguard/Dragonborn only add extra ingredients and
    potions to the pool if present.)

The bundled MAO.dll is part of the mod and required.

WHAT IT DOES
------------
Single-use potions are removed from vendors and shop stock. Potions become
permanent, reusable FLASKS that you charge from abstracted "essence" gathered
in the field:
  * Harvesting flora and looting creatures increments a three-tier essence
    pouch (Base / Catalyst / Apex) instead of filling your inventory.
  * You learn flask BLUEPRINTS by analyzing potions you find in the world —
    picking one up destroys it and unlocks that effect plus matching essence.
  * Take over any alchemy station to open the Field Kit: configure a flask's
    effect, watch its charges, and drink it (the flask persists; charges refill
    from your pouch on sleep or a background timer).
  * Offensive blueprints become timed WEAPON COATINGS once you take the
    Vanguard Coating perk.
  * Two economy modes via the MCM conversion toggle: ESSENCE mode (default,
    above) or INGREDIENT mode, where a flask charge instead costs its recipe
    ingredients out of your bags. Blueprint learning works in both.

SETUP
-----
1. Install this mod with your mod manager, near the END of your load order,
   and enable MAO.esp.

2. Adapt MAO to YOUR load order with Synthesis (there is no exe):
     EASIEST: download the MAO.synth file, then in Synthesis select a group and
       double-click it — it adds the patcher for you (right project
       preselected).
     OR MANUALLY:
       a. Open Synthesis, add a new patcher -> Git Repository, point it at:
            https://github.com/marthofdoom/MAO
          Project:  installer/MAO.Synthesis/MAO.Synthesis.csproj
       b. Run your Synthesis pipeline. (Synthesis builds it for you.)

   Synthesis reads your load order and:
     * rebuilds the WINNING Alchemy perk tree into MAO's flask perks
       (writing "MAO - Patch.esp"), and
     * classifies every ingredient into Base / Catalyst / Apex tiers calibrated
       to the mods you actually run
       (writing SKSE/Plugins/MAO/mao_tiers.json).

3. Enable the Synthesis output in your load order (Synthesis normally does this
   for you) and play. Re-run Synthesis after ANY load-order change so the perk
   tree and ingredient tiers stay matched to your list.

DID IT WORK?
------------
In game, the SKSE log (Documents/My Games/Skyrim Special Edition/SKSE/MAO.log)
should show:
  * "[perks] ... TREE (MAO - Patch.esp)"     — the rebuilt Alchemy tree is live
  * "[tiers] loaded N ingredient tiers ..."   — your calibrated tier map loaded

WITHOUT the patcher MAO is still fully playable: the DLL AUTO-GRANTS the flask
perks at their skill thresholds (so you don't spend points on Alchemy), and
ingredient tiers fall back to a built-in name heuristic. In that state the log
reads "[perks] ... auto-grant" and "[tiers] no mao_tiers.json — using the
name-substring fallback". Running Synthesis upgrades both to your load order.

NOTES
-----
No FOMOD — all options are runtime MCM toggles (MCM -> marth's Alchemy
Overhaul), including the essence/ingredient conversion toggle and the tuning
sliders. Flask, essence, and blueprint state lives in the SKSE co-save and
migrates itself on load, so upgrades are save-safe within a major line. Flasks
have no gold value and cannot be sold — they're kit hardware, not merchandise.

QUEST COMPATIBILITY
-------------------
Quest items and quest-FLAGGED items are protected automatically and never
convert (Vaermina's Torpor, Jarrin Root, Berit's Ashes, the White Phial, and
any item a quest has flagged). A few quests instead need you to gather or hand
over ORDINARY ingredients (e.g. The Only Cure wants Deathbell + Vampire Dust;
some favor quests want bundles of common flora) — those still dissolve into
essence while conversion is on, because they can't be excluded without breaking
normal gathering. For those quests, toggle "Convert ingredients to essence" OFF
in the MCM, gather/deliver, then toggle it back on.

REQUIEM / LORERIM
-----------------
The Synthesis patcher targets whatever alchemy tree WINS in your load order, so
MAO installs on top of Requiem, LoreRim, or another overhaul rather than
fighting it. Standalone MAO never scales potion magnitudes past your list's own
standard caps — it changes where potions come from, not how strong the list
lets them be.
