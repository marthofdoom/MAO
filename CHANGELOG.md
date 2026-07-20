# MAO changelog

Newest first. Every version listed here reached the game on the test Deck;
since v0.18.2 each ships as a complete mod zip (`tools/package_deck.sh` →
`~/Games/Projects/` on the Deck; cut releases will use `tools/release.sh`).
Point fixes fold into their feature's entry unless load-bearing on their own.
Version string lives in ONE place: `kPluginVersion` in `native/plugin.cpp`
(build-stamped into the MCM Debug page by `MAO_GenerateESP.py`).

## v1.0.0 — first public release (2026-07-20)

First Nexus release. The core loop, both economies, discovery, coatings, the
perk tree, MCM, and the Synthesis install path are complete and soak-tested.

Release hardening (over v0.23.0):
- **Quest safety**: quest-FLAGGED items (vanilla or modded) are never converted
  — a generic inventory quest-object guard on both the ingredient and potion
  conversion paths, plus a name backstop for always-quest-only reagents
  (Vaermina's Torpor, Jarrin Root, Berit's Ashes, White Phial). Note: quests
  that need ORDINARY common ingredients (e.g. The Only Cure) require toggling
  conversion off in the MCM — see the Quest Compatibility note.
- **Fluid Motion deferred**: effect not implemented, so it is never granted and
  never placed in the tree (no more no-op perk).
- Accurate public README + Nexus description + in-zip readme; generated banner
  (3.5:1 + 16:9); release cutter gates the DLL build against HEAD and ships the
  readme.

Known experimental (stated on the mod page): automated-potion-mod compatibility
(not built) and the Fluid Motion perk (deferred).

## v0.23.0 — field-kit variants sorted by type (2026-07-17)

- The field-kit variant list is grouped by effect type with commonly-used
  categories on top — Restoratives > Fortify > Resist/Cure > Other > Coatings —
  with category headers, clustered by effect then name within each group
  (was g_discovered hash order). Pure render-side change, Fable-reviewed clean.

## v0.22.2 — flask item card shows real effect (2026-07-17)

- Soak fix (marth: flask effects read "0 seconds"): the flask item ships a baked
  placeholder effect (Restore Health 5/0) and MAO only renamed it — the drink
  hook casts the configured VARIANT, so the card always showed 0s. RenameFlask
  now mirrors the variant's primary effect (magnitude/area/duration) onto the
  flask form so the card matches.
- Coatings keep the benign placeholder (Fable review): a hostile mirrored effect
  would flip the engine's IsPoison() true, rerouting the flask off the
  DrinkPotion hook to the vanilla apply-poison flow — consuming the permanent
  flask and bypassing the coating system. Only beneficial variants mirror;
  unconfigured-slot reset restores the placeholder too.

## v0.22.1 — rank-chain prerequisites (2026-07-16)

- Chain ranks 2–5 now carry vanilla's second record condition,
  `HasPerk(previous rank)` (byte-dumped from Alchemist20/40) — the skills
  menu orders the rank walk by it, so the node no longer describes an
  already-held rank as the next upgrade. ESP-only; re-run Synthesis.
  (Second latent issue inherited from MEO's perk recipe — both flagged
  upstream.)

## v0.22.0 — coating gate + P2 perk effects (2026-07-16)

- **Coating gate is LIVE** (testing relaxation over): without Vanguard Coating,
  detrimental blueprints can be discovered but not prepared, selected, or
  applied; refills skip unusable coating flasks from pre-gate saves. Vanguard's
  description corrected (nothing implies poisons are drinkable pre-perk).
- **P2 perk effects implemented** (Fable-reviewed, findings fixed): Kit
  Calibration III (T1/T2 refill −15%), Kit Calibration V (T1/T2 sleep refill
  halved), Pouch Expansion (15% flora-harvest proc — bonus Catalyst in essence
  mode, +1 extra ingredient in ingredient mode; one proc per harvest),
  Extended Synthesis (+30s on drinkable BUFFS — never hostile riders).
- **Fluid Motion self-disables** without a drink-animation mod: no auto-grant,
  no effect flag, and the tree patcher omits its node entirely (marth's
  design). Penalty-halving lands when a target mod is identified.
- **Kit Calibration display fix**: chain records now declare numRanks=5
  (vanilla convention, byte-dumped from Alchemist00) — with ranks=1 the skills
  UI advertised an already-held rank ("3/3" at 3/3). Re-run Synthesis after
  installing (ESP changed).

## v0.21.1 — refill trickle + Requiem pricing (2026-07-16)

- **Timer refills trickle**: 1 charge per flask per pass (was fill-to-cap —
  pre-existing in essence mode, made visible by ingredient consumption).
  Sleep keeps the full kit check.
- **Polarity-aware multi-effect pricing**: each secondary effect matching the
  primary's polarity costs its own recipe pair at its own concentration;
  opposite-polarity riders (Requiem's Damage-Regen side effects) are FREE.
  Replaces the flat +Apex-unit for any multi-effect potion, which priced
  ordinary Requiem poultices like endgame combos (42B+165A). Ingredient mode
  mirrors. New installer `potion` dump command (diagnosis tooling).

## v0.21.0 — ingredient mode (2026-07-16)

- **The conversion toggle now flips the WHOLE system** (marth). OFF =
  ingredient mode: a flask charge costs its variant's recipe ingredients (the
  cheapest source pair), consumed from your bags; pairs scale by concentration
  on the essence thresholds (+1 multi-effect) so the two economies mirror.
  Menu shows INGREDIENT MODE + per-variant item costs with affordability
  graying from a task-refreshed snapshot; refills batch per flask (Fable
  finding). The essence pouch persists untouched while OFF.
- **Discovery is mode-independent**: picking up a potion in ingredient mode
  studies its blueprint (+XP) but keeps the item and credits nothing — before
  this, ingredient mode could never learn a variant.
- **Flasks are unsellable** (VendorNoSale keyword — kit hardware, not
  merchandise) and the essence pouch counters are atomics (render-thread
  read race, doc-audit follow-up).
- Field mode grays no-recipe variants in ingredient mode (they could never
  refill); unresolved variants no longer refill for free.

## v0.20.1 — tier-matched analysis essence (2026-07-16)

- Analyzing a found/bought potion credits **exactly one refill charge of that
  potion's own variant** — `VariantCost` in the variant's own tier split,
  replacing the flat Base-tier yield. Same function refills spend, so credit
  and cost are 1:1 by construction and can never drift. DLL-only change.

## v0.20.0 — time-based coatings + MCM fix (2026-07-16)

- **Coatings are time-based** (DESIGN §5.2): 45 seconds baseline, 120 with
  Corrosive Retention. Hits are a 999 budget so time is the limit; expiry
  strips ONLY the poison MAO applied, sweeping every weapon in the inventory
  (unequipping can't bank the budget), and stale coatings are stripped at load
  (the clock is deliberately runtime-only). Fable-reviewed: an expiry-vs-recoat
  race and the unequip leak were found and fixed before cutting.
- **MCM Debug page rebuilt to MEO's shape** (formatting errors): the page opens
  with a header, the version stamp is numeric-only (the full string moved to
  the row's hover help), labels normalized.

## v0.19.0 — the MEO perk vehicle (2026-07-16)

- **MAO flag perks**: MAO.esp ships 13 PERK records — the Alchemy-100-gated
  capstone (0x816) and 12 flag perks (frozen band 0x818–0x823): the Kit
  Calibration 5-rank chain (NNAM), Fluid Motion, Vanguard Coating, Apex
  Stabilization, Field Extraction, Corrosive Retention, Pouch Expansion,
  Extended Synthesis. Effect-less markers with `GetBaseActorValue(Alchemy)`
  skill gates; the DLL reads `HasPerk` and applies all effects (MEO vehicle,
  byte recipe ported verbatim).
- **Vanilla perks untouched**: the interim rename-in-place vehicle is retired.
  Vanilla survivors (Snakeblood, Green Thumb) keep their names and effects —
  marth's rule: a survivor and its §5.2 analog both stay when not duplicative.
- **Two perk modes**: with `MAO - Patch.esp` present → TREE mode (perks bought
  in the rebuilt Alchemy constellation); without → skill-threshold auto-grant
  (0/20/30/40/50/60/70/80/100).
- **AVAlchemy tree patcher** (`write-patch` / Synthesis): overrides the WINNING
  AVAlchemy record; removes craft perks BY BEHAVIOR (entry points:
  ModAlchemyEffectiveness, ModPotionsCreated, ModInitialIngredientEffectsLearned,
  PurifyAlchemyIngredients, ModPoisonDoseCount, + `GetIsObjectType==Ingredient`
  effects — census in Docs/PERK_TREE_RECON.md); keeps and rewires everything
  else (orphans reparent to root, dangling HasPerk prerequisites dropped);
  inserts MAO's 9 nodes on a collision-scanned grid. ESL, pure override.
- **MAO.Synthesis**: the user-facing install path. Shares the installer's
  Commands.* code verbatim; also writes `mao_tiers.json` for the load order
  (self-built order including Skyrim.ccc — Synthesis omits CC plugins). Root
  `MAO.sln` + `assets/MAO.synth` with verbatim-matching SelectedProject, and a
  CI job that builds through the solution exactly as Synthesis does (MEO
  shipped this path broken for three versions; the guard prevents a repeat).

## v0.18.2 — co-save hardening + MCM version stamp (2026-07-16)

- **Persistence invariants adopted from MEO's release**: every co-save read is
  verified against its size (short read = loud bail, never fabricate state);
  the blueprint count is sanity-capped (a corrupt record could previously
  drive ~2^32 loop iterations); flask records bail to baseline on truncation.
- **Loud downgrade warning**: loading a save with records from a NEWER MAO now
  shows an on-screen message box that saving will permanently destroy them
  (SKSE does not round-trip unread co-save records). Was log-only.
- **Build-stamped MCM version**: the generator stamps `kPluginVersion` into the
  MCM Debug page as static text (the live-Papyrus route renders blank — MEO
  lesson). First thing to check after any install: the version line.
- First version delivered as a mod-manager zip to the Deck's Projects folder
  (the Deck now runs marth's own modlist; flat-install deploys retired).

## v0.18.1 — coatings unlocked for testing + field diagnostics (2026-07-13)

- Coatings (detrimental flasks → weapon poison via the drink hook) made
  craftable and applicable WITHOUT the Vanguard perk while P2 is tested —
  reversible `kCoatingsRequireVanguard` flag restores the design gate.
- Field-kit opens log a per-slot charge dump (diagnosed the reported
  "field change empties other flasks" — not reproducible in state; withdrawn).

## v0.18.0 — ingredient→essence conversion toggle (2026-07-13)

- MCM toggle `bConversionEnabled` (Field Kit → General, default on) gates the
  ContainerSink — the SOLE essence-credit point. OFF: harvested/picked-up
  ingredients and potions stay in the inventory as normal items; flasks, field
  kit, power, and perks are unaffected. Applies live on MCM close.
- Deploy now BACKFILLS new MCM keys into an existing Settings ini
  (`tools/merge_mcm_settings.py`): MCM Helper reads an absent key as OFF/zero
  and won't persist it — a control added after first install looked dead.
- `bNotify` reset to default each config pass (reset-then-parse consistency).

## v0.17.x — ingredient tier map + field power + XP + coatings (2026-07-13)

- **v0.17.1**: DLL loads `mao_tiers.json` (per-load-order ingredient rarity,
  written by the installer's `write-tiers`) with the name-heuristic fallback.
  Classifier tuned to 0.15 availability / 0.85 gold value with 0-source
  ingredients dropped to Base ("Apex must be obtainable").
- **v0.17.0-era**: "Open Field Kit" lesser power opens a LIMITED field kit
  (view essences; one flask change per trip, starting empty); Alchemy XP for
  essence spent (configure + refills) and new-variant discovery; P2a weapon
  coatings (detrimental flask → ExtraPoison on the equipped weapon).

## v0.15–v0.16 — P1e/P1f: perks + MCM (2026-07-12/13)

- Perk-driven kit capacity ladder 2/2→6/9 (vanilla Alchemist chain read via
  HasPerk — superseded by v0.19.0's own perks), MAO capstone perk, per-perk
  MCM debug override (cached flags only; never touches real perks).
- MCM Helper page (registered via QUST + MCM_ConfigBase pex — config.json
  alone is never read), tuning knobs, Settings seed.
- Alchemy-station takeover fix: eject the player from the furniture (the
  engine re-opened the vanilla CraftingMenu every ~5s, flashing menu chrome).

## earlier (P0–P1d, v0.1–v0.14, 2026-07-11/12)

Flasks (6 dedicated ALCH forms, stable per-slot items, runtime-renamed
contents), potion analysis/discovery, gathering → 3-tier essence pouch,
compound recipe-based economy (cost split per ingredient in its own tier,
2×mean×1.3 ingredient basis), refills (timer + sleep), SKSE co-save
(POCH/BLPT/FLSK), ImGui field-kit menu with station takeover, DrinkPotion
vfunc hook, gamepad/keyboard openers.
