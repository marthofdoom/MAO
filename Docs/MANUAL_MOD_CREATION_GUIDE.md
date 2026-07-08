# Creating Skyrim SE Mods Without the Creation Kit or xEdit

A field guide for building a complete Skyrim SE mod (ESP + Papyrus scripts +
FOMOD installer) entirely from Linux with Python and wine. Everything here was
validated while building Marth Requiem Overhaul (MRO) against the LoreRim
modlist. Where a byte layout is stated, it was verified against real records
parsed from Skyrim.esm or Requiem.esp — do the same for any record type not
covered here: **find a vanilla record that does what you want and copy its
bytes**, never trust format documentation alone.

## 1. The ESP file format (what you actually need)

An ESP is: one `TES4` header record, followed by top-level `GRUP` containers,
one per record type, each holding records.

### Records
```
type(4s) dataSize(u32) flags(u32) formID(u32) vc1(u32) formVersion(u16)=44 vc2(u16) | data
```
- **TES4 flags must be `0x00000200`** for a normal ESP. A survey of 3,288
  LoreRim plugins showed 94% use exactly this. `0x0` works sometimes but
  caused installer/tooling failures for us.
- Compressed records have flag `0x00040000`; their data is
  `decompressedSize(u32) + zlib`. You must handle this when *reading* vanilla
  masters; never bother compressing your own output.

### Subrecords
```
type(4s) size(u16) | payload
```
Order matters more than documentation admits: **EDID must come first**, and
EDID must precede VMAD in MGEF/QUST records (wrong order = silent breakage).

### Groups
```
'GRUP' totalSize(u32, includes 24-byte header) label(4s) groupType(i32)=0 stamp(u32)=0 unknown(u32)=0
```
Top-level groups only need type 0. Group order within the file doesn't matter
to the engine.

### TES4 header body
`HEDR` (version 1.70 f32, record count u32, next-object-id u32), `CNAM`
(author zstring), `SNAM` (description), then one `MAST` (master filename
zstring) + `DATA` (u64 zero) pair per master, in master-index order.

### FormIDs
- **Your own records MUST use your own file's master index as the upper
  byte**: with 5 masters that's `0x05000800+`. Using upper byte `0x00`
  *injects* your record into Skyrim.esm's FormID space — if that ID already
  exists there (as a different record type), your record silently never
  exists at runtime. This exact mistake (`0x00000810` colliding with a
  Skyrim.esm IMAD) cost days of debugging a "missing" MCM quest while every
  other record appeared to work.
- References to masters: `(masterIndex << 24) | (id & 0xFFFFFF)` where
  masterIndex is the position in *your* MAST list.
- Overrides of a master's record: emit a record with the master-indexed
  FormID; the engine replaces the original. This is how you override vanilla
  leveled lists, etc. Overrides are the ONLY legitimate reason for a
  non-own-index prefix.
- **GMSTs are special: matched by EDID, not FormID.** A brand-new FormID with
  EDID `fPlayerMaxResistance` overrides the setting; last plugin in the load
  order wins. First letter of the EDID declares type (f=float, i=int,
  s=string, b=bool); payload is a single `DATA` subrecord.

### SEQ file (mandatory for most quest mods)
SSE only auto-starts a Start-Game-Enabled quest that is NOT flagged Run Once
if the plugin ships `Data/SEQ/<PluginName>.seq` — a headerless flat array of
little-endian uint32 FormIDs (as stored in the plugin) of every such quest.
Run Once quests start without it, which makes this failure mode deceptive:
your Run Once startup quest works, your MCM quest (which must not be Run
Once) never starts, and nothing logs an error.

## 2. Record recipes that are known-good

### GLOB (global variable)
`EDID` + `FNAM` (1 byte: `'f'`) + `FLTV` (f32). Perfect for MCM-toggleable
feature flags read by scripts via `GlobalVariable.GetValueInt()`.

### QUST (quest that runs a script)
`EDID`, `FULL`, `VMAD`, `DNAM`, `NEXT` (empty), `ANAM` (u32 0).
DNAM (12 bytes): `priority(u8) + 01 00 FF + flags(u16) + type(u16) + u32 0`.
Flags: `0x0001` Start Game Enabled, `0x0004` Run Once.
**An MCM quest must NOT have Run Once** or SkyUI can't re-register it.

### VMAD (script attachment) — SE object format 2
```
version(u16)=5 objFormat(u16)=2 scriptCount(u16)
per script: nameLen(u16) name status(u8)=0 propCount(u16)
per property: nameLen(u16) name type(u8) status(u8)=1 value
  type 1 (Object): unused(u16)=0 aliasID(i16)=-1 formID(u32)
  type 2 (String): len(u16) + chars   | 3: i32 | 4: f32 | 5: u8 bool
```
Property names must exactly match `Property` declarations in the .psc.
Extra VMAD properties for non-existent script properties log warnings —
remove them when a script drops a property.

### MGEF (magic effect) — DATA is 152 bytes
Key offsets (verify against a real MGEF before trusting):
`[12]`=MagicSkill(0xFFFFFFFF none), `[16]`=MinSkill, `[64]`=archetype
(0=ValueModifier, 1=Script, 34=PeakValueModifier), `[68]`=primary AV,
`[80]`=castType(0=Constant), `[84]`=delivery(0=Self), `[88]`=secondary AV
(0xFFFFFFFF), `[112]`=dualCastScale f32 1.0.
A scripted constant/self MGEF + VMAD = "run this ActiveMagicEffect script
while the ability is on the actor" (OnHit handlers etc.).
**Fortify-an-AV from a constant ability: archetype 0 (ValueModifier)
silently does NOTHING.** Vanilla fortify effects are archetype **34
(Peak Value Modifier)** with flags Recover(0x2)+NoArea(0x800) and 0.5 at
DATA[48] — copy Skyrim.esm `AbFortifyCarryWeight` field-for-field (this
cost MRO its carry-weight feature for weeks, invisible because the
ability itself showed as applied). Note: fixing an MGEF record does NOT
fix active-effect instances already in saves — bump the script's
version and Remove+AddSpell in the migration.

### SPEL (ability)
Required subrecords, in order (verified against Skyrim.esm
AbAlduinInvulnerability): `EDID`, `OBND` (12 zero bytes), `FULL`, `ETYP`
(equip type u32, vanilla abilities use 0x00013F44), `DESC` (empty zstring
in non-localized plugins), `SPIT` (36 bytes: cost f32, flags u32,
**type u32=4 Ability** — type 3 is Lesser Power and NEVER applies as a
constant effect; this one-value error made two "abilities" silently do
nothing for days —, chargeTime f32, castType u32=0 Constant, delivery
u32=0 Self, castDuration, range, perk u32=0), then per effect: `EFID`
(MGEF formID) + `EFIT` (magnitude f32, area u32, duration u32).
Add to actors with `Actor.AddSpell(spell, false)`. Verify in-game via the
Active Effects UI — a constant self ability with a FULL name shows there.
MGEF companions need `SNDD` (empty) and `DNAM` (empty zstring) after DATA,
and VMAD goes immediately after EDID.

### PERK with an entry point (e.g. damage multiplier)
Copied byte-for-byte from Skyrim.esm `DragonhideSpellPerk` (80% physical
damage reduction — the gold standard for "Mod Incoming Damage"):
```
EDID, DESC(b'\x00'), DATA(5)=[trait0, level0, ranks1, playable0, hidden1]
PRKE(3)=[2,0,0]            # type 2 = entry point
DATA(3)=[36,3,3]           # entryPoint=36 Mod Incoming Damage, function=3 Multiply Value
EPFT(1)=[1]                # param type float
EPFD(4)=f32 multiplier     # e.g. 0.2 = take 20% damage
PRKF(0)                    # end of effect
```
Entry point 36 affects physical/weapon hits only. Other verified entry
points (from Skyrim.esm Haggling00): **8 = Mod Buy Prices** (multiply <1 =
cheaper) and **60 = Mod Sell Prices** (multiply >1 = higher). Perks are
per-actor (AddPerk/RemovePerk) — the way to give player/follower-only
passive effects. For a dynamic value, generate a ladder of perks with
static values and swap them from script (AV-driven entry point functions
exist but we found no vanilla example to copy the layout from — don't guess).

PERK gotchas that made the loader silently reject records:
- NO trailing `PRKF` after the FINAL entry (multi-entry perks put PRKF
  between entries only — both Dragonhide and crFalmerPoison05 confirm).
- DATA(5) = [trait=0, level=0, numRanks=1, playable=1, hidden=0]; our
  hidden/unplayable variant did not load.
- Conditions (PRKC/CTDA) are optional; unconditioned entries work.

### FLST (form list)
`EDID` + one `LNAM` (u32 formID) per entry. One VMAD Object property hands a
whole perk ladder to a script (`FormList.GetAt(i) as Perk`).

### LVLI (leveled list) override
Copy the winning override's decompressed body wholesale, patch what you need,
emit with the original FormID. Entries are `LVLO` (12 bytes: level u32,
ref u32, count u32) — e.g. double `count` at offset 8 to double vendor gold.
To find the winning override: walk plugins.txt in order, resolve each file
through the MO2 mods folders (modlist.txt top = highest priority), parse each
plugin's TES4 MAST list to find Skyrim.esm's master index there, then scan
its LVLI GRUP for `(fid >> 24) == thatIndex` matches. Last match wins.

## 3. Compiling Papyrus on Linux

> In this repo, `tools/compile.sh` wraps everything below with all paths
> baked in — use it. The raw recipe here is for reproducing the setup on
> another machine.

System wine usually lacks Mono → `PapyrusCompiler.exe` (a .NET app) won't run.
**Proton Hotfix's wine bundles wine-mono** and works:

```bash
PROTON="/mnt/gaming/Steam/steamapps/common/Proton Hotfix/files/bin/wine"
MONO_DATA="/mnt/gaming/Steam/steamapps/common/Proton Hotfix/files/share/wine"
PAPYRUS="<Nemesis mod>/Nemesis_Engine/Papyrus Compiler/PapyrusCompiler.exe"
FLAGS="<same dir>/scripts/TESV_Papyrus_Flags.flg"

WINEDATADIR="$MONO_DATA" "$PROTON" "$PAPYRUS" MyScript.psc \
  -f="$FLAGS" \
  -i="<your Source/Scripts>;<SKSE64 Scripts/Source>;<CSF Source/Scripts>;<PO3 Source/scripts>;<Nemesis compiler scripts>" \
  -o="<output dir>"
```

Import path rules (ordered, first hit wins):
- Your source dir first so your stubs shadow anything broken.
- **SKSE64's full sources are mandatory** — the compiler-bundled `Actor.psc`
  is stripped (missing `GetBaseActorValue` etc.) and produces insane errors
  like "cannot relatively compare variables to None" at wrong line numbers.
- Add the source dir of every SKSE plugin API you call (CSF, PO3 …).

### The stub technique
Any type referenced by an imported script must resolve at compile time, even
if you never call it. Missing sources (SkyUI's `SKI_ConfigBase`, PO3's
`Furniture`/`Hazard`/`Message`/`VisualEffect` deps) are satisfied with
one-line stubs in your source dir:
```papyrus
Scriptname Hazard extends Form Hidden
```
For `SKI_ConfigBase`, stub the subset of the API you use (AddToggleOption,
SetSliderDialogRange, events, …) — declare functions `native`. Stubs are
compile-only; **never ship stub .pex files** — the real ones live in SkyUI's
BSA etc. at runtime, and Papyrus links by name at load.

### Encoding traps
- Keep .psc files pure ASCII. Multibyte UTF-8 (box-drawing chars in comments)
  makes the compiler miscount line numbers and report errors on wrong lines.
- In user-facing strings, `•` and `—` render as mojibake (`â€¢`) in-game.
  ASCII hyphens only.

## 4. Useful runtime APIs (beyond vanilla Papyrus)

- SKSE: `MagicEffect.GetResistance()` (name of the AV that resists an
  effect — the generic way to handle fire/frost/shock/magic/poison),
  `Weapon.GetEnchantment()`, `Enchantment.GetNthEffect*`, `Form.HasKeyword`,
  `Actor.GetWornForm(slotMask)` (0x4 = chest), `Armor.GetWeightClass()`
  (0=light, 1=heavy, 2=neither).
- PO3 Papyrus Extender: `PO3_SKSEFunctions.GetPlayerFollowers()` (no quest
  aliases needed), `GetEffectArchetypeAsString(mgef)`,
  `GetPrimaryActorValue(mgef)` — filter real damage effects from riders
  (frost Slow shares FrostResist with frost damage; archetype tells them apart).
- PO3 player events: `RegisterForWeaponHit` → `OnWeaponHit(target, source,
  projectile, flags)` fires on the player's actual landed weapon hits
  (melee AND arrows) — the correct gate for "hit an enemy" logic;
  swing-detection via `RegisterForActorAction(0)` fires on air swings too.
  Also `OnMagicHit`, `OnItemCrafted`, `OnActorKilled`, `OnSkillIncrease`.
  **CRITICAL: the receiving script must extend ObjectReference,
  ActiveMagicEffect, or ReferenceAlias (use the matching PO3_Events_*
  variant). Registering a Quest script "succeeds" but events are NEVER
  delivered** — this silently zeroed weapon XP for a whole test pass.
  Standard pattern: a hidden always-on ability whose ActiveMagicEffect
  registers in OnEffectStart and forwards to the quest via a property.
  Note `RegisterForHitEventEx` is for hits ON the registered reference —
  not for the player's outgoing hits.
- No max-actor-value getter in Papyrus: derive it via vanilla
  `GetActorValuePercentage(av)` → max = current / pct (guard pct <= 0).
  Needed for overheal/overflow logic.
- Reading a GMST you also write: capture the base value BEFORE the first
  write and keep it in a script variable (saved), or you ratchet your own
  output (e.g. scaling fSmithingArmorMax by mastery each cycle).
- Papyrus string comparison is case-insensitive. `\` continues a line.
- A quest script + `RegisterForSingleUpdate(30.0)` heartbeat re-applying
  GMSTs beats any mod that sets them at startup, since you re-win every cycle.

## 5. FOMOD installer (MO2)

Layout: archive root contains `fomod/info.xml`, `fomod/ModuleConfig.xml`,
and the payload files. Keep payload paths flat (no `Core/` indirection —
sources are archive-root-relative).

The single most important rule, learned via MO2's unhelpful
**"invalid vector subscript"** crash dialog:

```xml
<installStep name="...">
  <optionalFileGroups order="Explicit">
    <group name="..." type="SelectExactlyOne">
      <plugins order="Explicit">        <!-- REQUIRED WRAPPER -->
        <plugin name="...">
          <description>...</description>
          <files><file source="X" destination="X"/></files>
          <typeDescriptor><type name="Recommended"/></typeDescriptor>
        </plugin>
        <plugin name="Skip">
          <description>...</description>
          <files/>                       <!-- empty install still needs this -->
          <typeDescriptor><type name="Optional"/></typeDescriptor>
        </plugin>
      </plugins>
    </group>
  </optionalFileGroups>
</installStep>
```
Without `<plugins order="Explicit">`, MO2 parses zero plugins and indexes
into an empty vector. `order="Explicit"` belongs on `optionalFileGroups` and
`plugins`; `SelectExactlyOne` is a group *type*, not an order.

FOMOD choices can only decide **which files install**. Runtime behavior
toggles belong in an MCM reading GlobalVariables; use FOMOD pages for real
file alternatives and for documentation.

## 6. Verification workflow

1. `python3 your_generator.py out/` — regenerate ESP after every change.
2. Parse your own output back (walk GRUPs, dump records) to verify layouts.
3. In-game console:
   - `help MRO_ 0` — all your records resolve (0 = all types).
   - `set <global> to 0` — poke feature flags (`setglobalvalue` doesn't exist).
   - `getgamesetting fPlayerMaxResistance` — confirm GMST wins.
   - `player.hasperk <formid>` / `player.getav DamageResist` — perk ladders.
   - `setstage ski_configmanagerinstance 1` — force SkyUI MCM rescan.
4. MCM appears only after registration; save + reload if the setstage trick
   doesn't take.

## 7. Architecture that worked

- One **startup quest** (Start Game Enabled + Run Once) owning: GMST
  application, ability distribution, a 30s `OnUpdate` heartbeat for
  everything dynamic (perk ladders, follower sync, mastery bonuses).
- One **MCM quest** (Start Game Enabled, NO Run Once) with the SkyUI script.
- Feature flags as GLOBs wired into both quests' VMADs; scripts treat a null
  property as "feature on" so a missing GLOB fails open.
- Player/follower-only passives = abilities and perks added by script;
  global tuning = GMST records. Choose per effect: GMSTs hit everyone.
- Keep the generator idempotent and fast to re-run; rebuild zips every time;
  never hand-edit the ESP.
