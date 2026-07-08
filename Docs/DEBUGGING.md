# Debugging Cookbook — symptom → cause → fix

Every entry below was hit for real in this project. Work the table before
theorizing. The universal method when nothing matches:
**find a vanilla record that does what yours should, dump both with
`tools/dump_record.py`, and diff every subrecord** — type, order, size,
bytes. The engine rejects records silently; the diff always finds it.

## Records / ESP

| Symptom | Cause | Fix |
|---|---|---|
| Record absent from `help <edid> 4` in-game, but present when parsing the ESP | Loader rejected it: wrong/extra/missing subrecord vs vanilla layout | Diff against vanilla twin. Known: PERK must have no trailing PRKF after last entry, playable=1 hidden=0 |
| Constant ability does nothing, not in Active Effects | SPIT spell type 3 (Lesser Power) — must be **4** (Ability). Or SPEL missing OBND/ETYP/DESC | See guide "SPEL (ability)" recipe |
| Fortify-AV effect applies (shows in Active Effects) but the actor value never moves | MGEF archetype 0 (Value Modifier) silently no-ops for fortify-from-ability; vanilla fortify MGEFs are archetype **34 (Peak Value Modifier)** + Recover flag | Copy the vanilla twin's DATA field-for-field (`AbFortifyCarryWeight`: flags 0x208802, 0.5 at DATA[48], archetype 34). **Direct MEO impact: every constant armor gem (Fortify Health/Magicka/Stamina/Carry Weight and all 18 fortify-skill effects) is this record class.** Record fixes don't reach active-effect instances already in saves — bump script version, Remove+AddSpell in migration |
| One record broken while its neighbors work | FormID collides with a real record in a master (own prefix wrong) | Own records need own-file master index prefix (0x05 with 5 masters) AND 0x800-0xFFF (ESL). `tools/audit_esp.py` checks both |
| Start-game-enabled quest never starts on existing save | No Run Once flag + no SEQ file | Generator writes SEQ/MRO.seq — ship it. Run Once quests don't need SEQ (which is why one quest works and the other doesn't) |
| MCM never appears | Quest not running (above), or quest has Run Once (SkyUI can't re-register), or SkyUI hasn't rescanned | Fix flags/SEQ; then `setstage ski_configmanagerinstance 1` |
| GMST override ignored | Another plugin loads later, or an SKSE DLL manages that setting | GMSTs match by EDID, last plugin wins; also re-apply from script heartbeat to beat runtime writers |
| Script property is None at runtime | VMAD property name doesn't match .psc Property, or not wired at all | `tools/audit_esp.py` |
| "invalid vector subscript" dialog from MO2 on install | FOMOD `<group>` missing `<plugins order="Explicit">` wrapper | See guide FOMOD section; empty-install options need `<files/>` |

## Papyrus / compilation

| Symptom | Cause | Fix |
|---|---|---|
| "unknown type X" compiling, X is a vanilla/SKSE class | Missing .psc in import path | One-line stub in Source/Scripts: `Scriptname X extends Form Hidden` |
| "cannot relatively compare variables to None" at a nonsense line | Stripped Actor.psc from compiler bundle shadowing SKSE's | SKSE64 sources must be in imports (tools/compile.sh has them) |
| Error line numbers don't match the file | Multibyte UTF-8 in source | ASCII only, everywhere |
| `â€¢` garbage in-game text | Non-ASCII in user-facing strings | ASCII only |
| Compiler can't run (mono errors) | System wine lacks Mono | Use Proton Hotfix wine — baked into tools/compile.sh |

## Runtime behavior

| Symptom | Cause | Fix |
|---|---|---|
| Popup/effect repeats every load or randomly | State flag set AFTER a queued UI call, or FormIDs changed between installs orphaning globals | Latch before showing; never change FormIDs post-release |
| Old script instances erroring in logs after update | Prior install had different FormIDs; orphaned instances in save | Inert noise on a test save; keep FormIDs stable so it never recurs |
| Mastery/percent progress from swinging at air | RegisterForActorAction(0) fires on swings, not hits | PO3 `RegisterForWeaponHit` → OnWeaponHit; gate on living hostile Actor target |
| Vendor gold unchanged after LVLI override | Merchant chest only re-rolls on cell reset | Wait 72+ in-game hours away from the cell |
| Feature works for player but not followers | Ability/perk granted to player only | Follower loop via `PO3_SKSEFunctions.GetPlayerFollowers()` in the heartbeat |
| GMST you scale keeps growing each cycle | Reading back your own written value | Capture base before first write, keep in a saved script var |
| A framework/SKSE-plugin call (CSF IncrementSkill, etc.) silently does nothing | Missing a required binding the framework reads — e.g. CSF's `Increment` no-ops unless the skill JSON binds a `"level"` GlobalVariable; a numeric config key it doesn't recognize is ignored | **When a framework call has no visible effect, READ ITS SOURCE** (same doctrine as diffing vanilla records). In MRO this masked a total mastery-XP failure for weeks. If the framework also caps behaviour (CSF hard-caps level at 100), own the state yourself: bind your own globals and `SetValue` them directly |
| DLL writes a GlobalVariable at data-load but Papyrus/menus read the old value | GlobalVariable values are SAVE-PERSISTED: loading a save restores its stored value over anything the DLL wrote at `kDataLoaded` | Re-assert DLL-owned globals on `kPostLoadGame` and `kNewGame`, not just `kDataLoaded` (MRO's native-DR handshake bug) |
| MCM checkbox/label doesn't repaint until the menu is closed and reopened | `OnOptionSelect` routed a read/write through a busy quest script; cross-script calls block on the target's instance lock, which a 30s heartbeat holds for its whole run | Keep the repaint path local to the MCM script (read the GlobalVariable directly, `SetValue`, `SetToggleOptionValue`); call into the quest only after |
| Form lookup on a hot path (per-hit, per-kill) is slow / churns | `GetFormFromFile` re-resolved every event | Look up once, cache the form in a saved script variable/array — FormIDs are frozen post-release |

## Crash analysis
Crash logs: `.../compatdata/3375297225/pfx/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SKSE/crash-*.log`.
Check POSSIBLE RELEVANT OBJECTS + CALL STACK for our forms/scripts. MRO is
pure Papyrus: it cannot cause native access violations — Papyrus bugs make
log errors, not CTDs. A deterministic same-action crash could implicate a
malformed record; random ones are other mods or the renderer.
