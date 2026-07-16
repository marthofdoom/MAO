# AVAlchemy perk-tree recon (Tier 2 Slice A)

Entry-point census of the winning `AVAlchemy` tree, gathered with the
installer's `tree-effects` command (2026-07-16), on two load orders:

- **Vanilla** (Skyrim.esm only, BottleRim Game Root): 10 perk chains, the
  classic tree.
- **LoreRim** (3400+ plugins; winning tree = Requiem + Special Feats +
  Ordinator remnants, 20 nodes): the adversarial case the classifier must
  survive.

This feeds the `MAO - Patch.esp` writer (the MEO `ApplyPatch` port). Prime
directive inherited from MEO: **classify perks BY BEHAVIOR (entry points),
never by name or EditorID.**

## Census

| Entry point | Vanilla | LoreRim | Verdict |
|---|---|---|---|
| `ModAlchemyEffectiveness` | Alchemist 1–5, Physician, Benefactor, Poisoner | REQ Alchemical Lore 1–2, Improved Elixirs/Poisons | **REMOVE** — brew-magnitude scaler; the flask system replaces brewing |
| `ModPotionsCreated` | — | REQ Catalysis 1–2 | **REMOVE** — potions-per-craft; not in vanilla, real in the wild |
| `ModInitialIngredientEffectsLearned` | Experimenter 50/70/90 | 1 node | **REMOVE** — eat-to-learn/experimentation; impossible in the flask design |
| `PurifyAlchemyIngredients` | Purity | REQ Purification Process | **REMOVE** — brew-purity toggle |
| `ModPoisonDoseCount` | Concentrated Poison | REQ Concentrated Poisons | **REMOVE** — poison doses; coating charges replace this |
| effect conditioned `GetIsObjectType == Ingredient` | — | Feat Herbalist 1–2 (`ModSpellMagnitude` ×2/×5 on eaten ingredients) | **REMOVE** — eat-ingredient scaler; dead once conversion consumes ingredients. The alchemy analogue of MEO's `GetIsObjectType == Enchantment` catch |
| `ModIngredientsHarvested` | Green Thumb | — | **KEEP** — harvest yield still works and synergizes with gathering (more ingredients → more essence). *Design call, revisit at the patch slice* |
| food-keyword `ModSpellMagnitude`/`ModSpellDuration` | — | Feat Gourmet (REQ_PVM_Food_* keywords) | **KEEP** — food perks; MAO never converts food (`IsFood()` early-out) |
| `ModAttackDamage` | — | Feat Drunken Combat 1–2 | **KEEP** — alcohol-fueled combat; not crafting |
| `ModTargetDamageResistance` | — | ORD Alkahest (poisons shred armor) | **KEEP** — poison *use*; works with coatings |
| pure abilities (no entry points) | Snakeblood | World Serpent, REQ passives (Night Vision, Regeneration, Fortified Muscles, Alchemical Intellect, Immunization) | **KEEP** — passives that don't touch the crafting flow |

## Classifier (for the ApplyPatch port)

A node (plus its whole NNAM rank chain, cycle-guarded) is a **craft perk MAO
obsoletes** if any rank has:

1. an entry point in { `ModAlchemyEffectiveness`, `ModPotionsCreated`,
   `ModInitialIngredientEffectsLearned`, `PurifyAlchemyIngredients`,
   `ModPoisonDoseCount` }, **or**
2. any effect whose conditions require `GetIsObjectType == Ingredient`
   (eat-ingredient scalers).

Everything else survives and goes through the keep/drop curation pass
(Synthesis = keep-all, same as MEO).

## Open design questions (decide at the patch slice)

- **Green Thumb keep/remove**: keeping it means a vanilla node survives in the
  MAO tree; removing it means MAO should ship its own harvest-yield flag perk
  (Field Extraction already exists in the §5.2 set as +10% gather).
- **1.0 ingredient-mode interaction**: when the (future) full ingredient-mode
  toggle turns conversion off, Herbalist-style eat-ingredient perks would work
  again — but the tree patch is static. Accepted: the patch targets the flask
  playstyle; documented limitation.
- **Snakeblood**: survives as a passive under this classifier. MAO's §5.2
  design re-enlists it as "Extended Synthesis" — if that re-enlistment stays,
  MAO's own flag perk replaces it and vanilla Snakeblood gets removed by an
  explicit exception, not by the behavior classifier. Revisit with the flag-perk
  set.

Raw dumps: regenerate anytime with
`dotnet run -- tree-effects <root> <plugins|profile>` (defaults to AVAlchemy).
