# Invariants — the load-bearing rules (v0.19.0)

Every rule below is an imperative with the failure mode that violating it
produces. Where the incident is MAO's own it is cited by commit/version;
where MAO hasn't hit it yet the rule is **inherited** from MEO's release
history and marked so. Line refs → `native/plugin.cpp` unless noted.
ARCHITECTURE.md maps the subsystems these rules live in; ANTI_PATTERNS.md is
the portable one-sitting audit digest.

## Engine interaction

1. **Call the engine's own flows, never hand-write the state a flow
   produces** (marth's standing doctrine, ENGINE_NOTES header; inherited —
   MEO's M2 burned ~6 release cycles rediscovering one engine call field by
   field). MAO's compliance: the drink payload is `CastSpellImmediate` of a
   real load-order potion (:1121-1123), coatings are the engine's own
   `ExtraPoison` (:1037-1038), conversion/grants are
   `RemoveItem`/`AddObjectToContainer`, the furniture eject is
   `NotifyAnimationGraph("IdleForceDefaultState")` (:2008).
2. **A flask form must NEVER fall through to the vanilla DrinkPotion path —
   the hook returns handled for EVERY flask, configured or not**
   (:1133, :1138-1144). The flask is a permanent item; vanilla consumption
   destroys it and its Favorites/Wheeler binding — the entire reason P1d
   moved to dedicated forms + the consume intercept (5db95e9).
3. **Station takeover must eject the player from the furniture** — on open
   (:2008) and again on close (:381-387). Failure (b25aaf0, the ~5s station
   glitch): seated at an occupied alchemy table with no crafting menu, the
   engine re-activates CraftingMenu every few seconds; each ~1-frame re-open
   flashed vanilla chrome through the overlay. MEO's enchanting bench never
   re-activates — the same `kHide` that was clean there was NOT clean here.
4. **Skip the DrinkPotion vfunc hook on VR** (:1154-1157). 0x10F is the
   SSE/AE index; VR shifts the Actor vtable — patching there hooks the wrong
   vfunc (caught in the P1d review, 88f822f).
5. **Resolve everything you need BEFORE spending state.** The drink hook
   resolves the variant before decrementing a charge (:1074-1082 — an
   unresolvable variant is inert, no charge burned); ConfigureFlask aborts
   when the slot's flask form is missing rather than spending essence into
   nothing (:841-847). Failure class: the P1d TOCTOU/spend-first review
   findings (88f822f).

## Iteration, threading & locks

6. **All engine mutation goes through `SKSE::GetTaskInterface()->AddTask`**
   (main thread). Sinks compute then queue (:1336, :1384); render-thread
   menu clicks queue `ConfigureFlask` (:1703); the refill timer and sleep
   sink queue (:995, :1009); MenuSink's takeover work queues (:1987).
   Inherited MEO doctrine — the render/input threads only read.
7. **Never call into the engine while holding `g_flasksLock` when the call
   can re-enter a sink — snapshot under the lock, act outside it**
   (`SyncFlaskItems` :912-927). Failure (88f822f): `AddObjectToContainer`
   fires `TESContainerChangedEvent` synchronously; the sink locks the same
   non-recursive mutex = self-deadlock on the load path.
8. **The render thread reads shared counts only as snapshots taken under the
   lock** (`DrawFieldKit` chargesCap :1629-1633). Failure (6c21d34): the
   VARIANTS header and per-variant costs read `g_chargesPerFlask` unlocked
   while `RecomputeCapacity` wrote it from the task thread.
9. **Every global that ReadConfig can rewrite mid-session is an atomic**
   (:307-314, :392-399). Failure (5525ab5): ReadConfig re-runs on MCM/menu
   close (task thread) while the render/refill/input/event threads read the
   tuning globals — aligned scalars don't tear on x86-64, but the race was
   UB; atomics make it well-defined. (`g_pouch`'s counters are the known
   remaining exemption — see ARCHITECTURE §6.)
10. **Re-verify state under the lock at the point of action.** The drink
    hook re-checks the slot is still configured with the SAME variant before
    decrementing (:1057-1064, :1093) and before refunding (:1104). Failure
    (88f822f TOCTOU): a reconfigure between hook entry and decrement charged
    the wrong flask.

## Persistence (co-save `'MAO1'`, POCH/BLPT/FLSK, schema v3)

11. **Every persisted FormID passes through `ResolveFormID` on load;
    unresolved → drop the record, never guess** (BLPT :2114/:2133-2135,
    FLSK :2173-2174). Inherited (MEO B1): the co-save stores raw runtime
    FormIDs, mod-index byte included — any load-order change remaps plugin
    indices and every key points into the wrong plugin's FormID space.
12. **Bound every count, bail on short read, clamp fields at ingestion**
    (`readOk` :2077; BLPT count capped at 65536 :2099-2105; FLSK
    `g_flaskCount ≤ kMaxFlaskSlots`, charges [1,99] :2157-2158). Adopted in
    v0.18.2 (98f45aa) from MEO N2/xp-hooks-S4: a corrupt BLPT count would
    drive up to 2^32 iterations, and `g_flaskCount` bounds hot loops
    (`FindFlaskSlot` runs per container event and per drink) and indexes
    `g_flasks[]` — an unclamped value reads/writes out of bounds.
13. **Warn LOUDLY and ON-SCREEN about newer-version co-save records; never
    log a comforting falsehood** (:2079-2085 skip + flag; :2284-2289 the
    kPostLoadGame message box). SKSE does NOT round-trip unread records —
    saving with an older DLL destroys them. Adopted in 98f45aa; MEO S1's
    log-only "skipped" walked users into silent data loss.
14. **Never persist a runtime-created (0xFF) FormID** (:1392-1399): a potion
    crafted at a bench still converts to essence, but its FormID is never
    recorded as a discoverable variant — next session the id dangles or is
    REUSED by a different created form, silently changing what a "variant"
    is.
15. **Versioned schema; readers for every shipped version stay forever;
    migrate forward** (BLPT v1/v2 readers + migration :2117-2144; FLSK v1
    branch :2162). The v1→v3 migration also DROPS what it can't honestly
    migrate (an effect with no load-order potion :2136-2142) with a log line
    telling the player how to restore it — never fabricates.
16. **When the capacity perks don't resolve, HOLD the co-saved counts — EVEN
    in debug** (:249-257). Failure (5525ab5, co-save contamination): on a
    load order where the perk chain didn't resolve, the MCM debug override
    wrote its capacity into `g_flaskCount`; turning debug off then "held"
    those debug values and SaveCallback persisted them. Debug capacity may
    take effect only where real perks can cleanly reassert on revert.
17. **On load, recompute capacity BEFORE syncing flask items**
    (:2277-2281 — `RecomputeCapacity("load")` then `SyncFlaskItems`).
    Failure (6c21d34): with the old order a shrunk kit granted and renamed
    physical flask items for slots the recompute immediately deactivated.

## The flask identity contract

18. **The slot's dedicated ALCH form (0x810+slot) IS the flask's identity;
    reconfiguring renames CONTENTS only** (:88-94, :881, `RenameFlask`
    :1285). The item must survive every reconfigure or Favorites/Wheeler
    bindings break — and when flasks were ordinary renamed potions, a
    reconfigure's `RemoveItem(999)` stripped a sibling slot's identical
    flask (88f822f); dedicated per-slot forms (3643ae5) closed the class.
19. **On load, re-apply every configured slot's name AND reset every
    unconfigured one** (:941-947). Form state is GLOBAL for the session, not
    per-save: loading save B after save A left A's "Flask: X" names on B's
    unconfigured flasks.
20. **One blueprint per slot; the flask never leaves the inventory on
    reconfigure** — ConfigureFlask overwrites the slot record and only
    grants the item if absent (:869-885); `PlayerHasItem` requires a real
    positive count (:794-807) because `GetInventoryCounts` retains stale
    0-count entries for extra-data-bearing forms (would skip a needed
    re-grant).

## Gathering & the economy

21. **ContainerSink is the SOLE essence-credit point; HarvestSink tags and
    must NEVER credit** (:1294-1296, :1424-1444). A harvest fires BOTH
    events — crediting in both doubles every harvest (the P0 double-credit
    guard; designed-in, keep it that way when the harvest-tag perks land).
22. **The conversion toggle gates the ENTIRE sink at its head — one
    early-out, never scattered checks** (:1304-1306, cece4ec). When OFF,
    ingredients and potions stay ordinary items and NOTHING is consumed;
    flasks, the kit, perks and the drink hook are explicitly unaffected. A
    partial gate (credit without consume, or consume without credit) eats
    items for nothing.
23. **Credit only items ENTERING the player** (`newContainer == kPlayerID`,
    positive count :1308-1312). The credit task calls `RemoveItem`; without
    the direction filter the sink re-triggers on its own removal — an
    infinite credit loop.
24. **Quest ingredients never dissolve** (`kExclusions` :446, checked
    :1324). Converting Crimson Nirnroot bricks "A Return To Your Roots" —
    the P0 exclusion; broaden it (quest-item flag + generated set) rather
    than deleting it.
25. **Food is not a potion; a flask grant is not a discovery** (:1360,
    :1363-1367). Without the `IsFlaskForm` guard the sink analyzes and
    DESTROYS the flask item the mod just granted.
26. **`LoadTierMap` runs before `BuildRecipeTable`** (:2260-2261 — the
    recipe table tiers every ingredient as it's built). Reversed, every
    recipe prices off the name-substring fallback and the installer's map
    only affects future rebuilds.

## Config & cross-artifact contracts

27. **Reset-then-parse every ReadConfig pass** (:1236-1240): every key with
    a default is reset before the files are read, so an ABSENT key reverts
    instead of sticking at its last in-memory value. Failure (a3479bc,
    surfaced by the v0.18.0 review): the perk debug override and toggles
    would survive being removed from the MCM's INI.
28. **Every new MCM control's key must be BACKFILLED into existing Settings
    files on deploy** (`tools/merge_mcm_settings.py`, wired into
    deploy_deck.sh). Failure (34d8bdf): MCM Helper reads a key absent from
    an ALREADY-EXISTING Settings ini as OFF/zero — it does NOT fall back to
    config.json's defaultValue — and won't persist changes; the v0.18.0
    conversion toggle showed off and did nothing on the Deck because its
    settings file predated the key.
29. **An INI/MCM key that changes SEMANTICS must be renamed** (inherited —
    MEO's `fGemXpSkillXP` absolute-rate read as a multiplier silently cut an
    XP stream ~100×, found live in a deployed install). MCM Helper persists
    stored values per key name forever.
30. **Clamp INI values at parse; a failed or hostile parse must never become
    an out-of-range value** (:1206-1217 — e.g. garbage `fEssenceTax` strtod's
    to 0.0 and clamps to the 1.0 floor). Inherited from MEO's
    boss-kills-award-nothing strtof incident.
31. **The MAO.esp FormID band is a frozen generator↔DLL↔installer contract**
    (generator MAO_GenerateESP.py:75-82 ↔ DLL :84-86/:160-174 ↔
    Commands.Patch.cs:53-62): 0x800 MGEF + 0x801 SPEL, **0x810–0x815
    flasks**, **0x816 capstone**, 0x817 MCM QUST, **0x818–0x823 flag perks
    in MAO_PERKS order**; `NEXT_OBJECT_ID` 0x824, forms only ever ADDED.
    Inherited failure mode (MEO): reordering the perk list silently rebinds
    perks to wrong effects; moving a flask fid orphans every save's FLSK
    slots and breaks every Favorites binding.
32. **Generator text must equal DLL math** — perk DESCs ↔ the
    RecomputeCapacity ladder and effect multipliers; the CTDA skill gates ↔
    `kPerkDefs[].req`; the MCM version stamp is READ from `kPluginVersion`
    so it cannot drift (MAO_GenerateESP.py:29-69). Inherited failure (MEO
    v1.0.6): shipped tooltips claimed 8–40% where the DLL did 5–25%.
    Standing MAO exposure: the five P2 perk DESCs (Calibration III/V, Fluid
    Motion, Corrosive Retention, Pouch Expansion, Extended Synthesis)
    promise effects the DLL doesn't implement yet — close that gap before
    any release that ships the tree.
33. **`MAO.sln` contains ONLY the Synthesis project, and `assets/MAO.synth`
    SelectedProject matches the .sln entry VERBATIM (backslashes included);
    CI asserts it** (0d91479, `installer.yml`). Inherited failure: exactly
    this mismatch shipped MEO's Synthesis install broken for three versions.
34. **The patchers self-exclude their own output and build their own load
    order including `Skyrim.ccc`** (Program.cs:63, MAO.Synthesis:39-52/:84).
    Reading `MAO - Patch.esp` compounds the patch on re-runs; Synthesis's
    own order omits Creation Club (MEO measured ~24% conversion loss) and
    CC packs carry a large share of MAO's ingredients.
35. **Everything that reads the filesystem in the installer stays inside a
    top-level try/catch** (Program.cs:58-106). Inherited (MEO m32e/m35): a
    double-clicked Wine console dies with no message when setup code throws
    outside the handler.

## Process

36. **A stale binary voids every in-game test** — check the version header
    (`kPluginVersion` is stamped into MAO.log :2308, the console :2267, and
    the MCM Debug page). Inherited; MEO was bitten twice.
37. **`kCoatingsRequireVanguard = false` is a TESTING relaxation** (:199,
    604d3c4): every coating gate (:834, :1083, :1690) keys off the constant
    so one flip restores the perk gate. Shipping with it false ships
    coatings ungated — flip it back when the installer-written tree is the
    delivery vehicle.
