# MAO changelog

Newest first. Every version listed here reached the game on the test Deck;
since v0.18.2 each ships as a complete mod zip (`tools/package_deck.sh` →
`~/Games/Projects/` on the Deck; cut releases will use `tools/release.sh`).
Point fixes fold into their feature's entry unless load-bearing on their own.
Version string lives in ONE place: `kPluginVersion` in `native/plugin.cpp`
(build-stamped into the MCM Debug page by `MAO_GenerateESP.py`).

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
