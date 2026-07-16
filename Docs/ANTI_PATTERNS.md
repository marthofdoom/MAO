# Anti-patterns — never repeat these (MEO → MAO, and every sibling)

The portable digest of every class of mistake MEO made or nearly shipped,
adopted per its own instruction, MINUS the MEO-specifics with no MAO analog
(uid minting/rekeying, the stacking cap, sound-mute windows), PLUS the
mistakes MAO has already made itself. Each entry: the rule, then one line on
how it bit MEO or MAO. Full mechanics live in INVARIANTS.md /
ARCHITECTURE.md / ENGINE_NOTES.md; this file exists so the project (and the
next sibling) can audit itself against the list in one sitting.

## Engine state

- **Never hand-write state an engine flow produces — call the flow.**
  MEO's M2 burned ~6 release cycles rediscovering, field by field, what one
  engine call does. MAO drinks via `CastSpellImmediate`, coats via
  `ExtraPoison`, ejects via `NotifyAnimationGraph`.
- **Never let the vanilla consume path touch a permanent item — intercept
  and return handled even when the item is unconfigured.** An inert flask
  falling through to vanilla DrinkPotion is destroyed along with its
  Favorites/Wheeler binding (the P1d design point, plugin.cpp:1138-1144).
- **Never leave the player seated in furniture whose menu you hijacked.**
  The engine re-activates the CraftingMenu every few seconds over an
  occupied alchemy table — vanilla chrome flashed through MAO's overlay
  until the takeover ejected the player (b25aaf0). MEO's enchanting bench
  never did this; "it worked in the sibling" is not evidence.
- **Never give an item two live effect sources.** MEO: base `formEnchanting`
  + instance enchant applied both. MAO's flask carries a placeholder ENIT
  effect and the hook casts the variant — if the hook ever lets the vanilla
  path run too, the player gets placeholder + variant.
- **Never trust a convenience reimplementation for engine-visible state.**
  CommonLibSSE-NG's `SetName` and abstract `LookupForm<T>` both compiled
  clean and failed 100% at runtime in MEO (v0.31.0 dead conversion table).
- **No square brackets in display names.** UI suites strip a leading `[...]`
  — the rename looks failed (MEO). MAO renames flask items every
  reconfigure; keep the "Flask: X" shape.
- **Never stuff sentinel values into engine pricing/count fields.** MEO's
  `charge=0xFFFF` ballooned weapons to 20k gold. MAO's planned GetItemCount
  faking (DESIGN §6.1) must report only 0/1, never a magic number.

## Iteration & concurrency

- **Never mutate a container/list you are iterating — snapshot first, act
  second.** MEO's mastered-gem birth head-inserted into the entryList its
  own walker was on (build-S3). MAO's SyncFlaskItems snapshots slots first
  for the same reason.
- **Never call the engine under a non-recursive lock the engine can
  re-enter.** MAO: `AddObjectToContainer` under `g_flasksLock` fired
  `TESContainerChangedEvent` whose sink takes the same lock — self-deadlock
  on load (88f822f).
- **Never read task-thread-written state from the render thread without the
  lock.** MAO's menu read `g_chargesPerFlask` bare while RecomputeCapacity
  wrote it (6c21d34); now it's snapshotted under `g_flasksLock`.
- **Any global a config re-read can rewrite mid-session must be atomic.**
  MAO atomicized the whole tuning block once ReadConfig started re-running
  on MCM close while four other threads read it (5525ab5).
- **Check-then-act across a lock boundary is a TOCTOU.** MAO's drink hook
  re-verifies slot+variant under the lock at decrement time — a reconfigure
  raced the first check (88f822f).
- **A single bool is not a cross-instance token — use a generation counter
  or an exchange.** MEO's 15s reapply fallback stole a second load's token
  (S2); MAO's one-shot timer start uses `exchange` (plugin.cpp:1002).

## Persistence

- **Never store raw runtime FormIDs across sessions — resolve on load, drop
  what doesn't resolve.** One plugin add/remove and every record points into
  the wrong plugin's FormID space: "the mod ate my save" (MEO B1; MAO
  resolves BLPT + FLSK on every load).
- **Never persist a runtime-created (0xFF) FormID at all.** A bench-crafted
  potion's id dangles or is REUSED by a different created form next session
  — MAO converts it to essence but refuses to record it as a variant
  (plugin.cpp:1392-1399).
- **Never read serialized data unchecked — bound counts, bail on short read,
  clamp at ingestion.** MEO fabricated keys from a truncated record (N2);
  MAO's unclamped `g_flaskCount` would index past `g_flasks[6]` in two hot
  loops, and an unbounded BLPT count spins 2^32 iterations (98f45aa).
- **Never log a comforting falsehood about data safety.** SKSE destroys
  unread co-save records on the next save; MEO's "preserved as unread" log
  would have walked users into it (S1). MAO warns with an on-screen box.
- **Never let a debug override write through to state it can't cleanly
  revert.** MAO's MCM perk-debug wrote capacity into `g_flaskCount` on a
  perk-less load order; disabling debug then persisted the debug values into
  the co-save (5525ab5).
- **Never mutate/grant items from persisted state before recomputing the
  authoritative capacity.** MAO granted and renamed flask items for slots
  the immediately-following recompute deactivated (6c21d34) — recompute
  first, sync second.
- **Versioned schema, readers forever, migrate honestly.** MAO is already at
  v3 with two live migrations; a migration that can't map a record DROPS it
  with instructions, never fabricates (plugin.cpp:2136-2142).

## Player-relative logic

- **Never apply a player-relative filter to non-player state.** MEO's
  player-only stacking-cap scan, reached from the NPC path, stripped every
  NPC enchant (build-B1 — its worst blocker). MAO keeps player-scoping at
  the SOURCE (the PlayerCharacter vtable hook, `newContainer == player`),
  not as filters buried in shared code.
- **Vouch for runtime state only from evidence that can produce it.** MEO
  counted unworn backpack spares and resurrected orphan effects (m24b). MAO
  analog: `PlayerHasItem` demands a real positive count because
  `GetInventoryCounts` retains stale 0-count entries (plugin.cpp:794-807).

## Config & release engineering

- **Never change an INI/MCM key's meaning under the same name — rename it.**
  MCM Helper persists old values per key name forever; MEO's stale absolute
  rate reread as a multiplier silently cut an XP stream ~100×.
- **Never let an absent key inherit its previous in-memory value —
  reset-then-parse.** MAO's perk-debug override and toggles survived being
  removed from the INI until every defaulted key was reset at the top of
  ReadConfig (a3479bc, cece4ec).
- **Never add an MCM control without backfilling its key into existing
  Settings files.** MCM Helper reads an absent key in an EXISTING settings
  ini as OFF/zero (no config.json fallback) — MAO's v0.18.0 conversion
  toggle looked dead on the Deck until deploy backfilled it (34d8bdf,
  `tools/merge_mcm_settings.py`).
- **Never let a failed parse silently become 0.0 — skip or clamp.** MEO's
  malformed MCM float zeroed boss-kill XP; MAO clamps every numeric at parse
  so garbage lands on a floor, not on zero.
- **Never cut release artifacts before the version's last commit.** MEO's
  v1.0.6 zip shipped tooltips off by ~2× against that evening's DLL.
- **Generated text and code math are one contract — derive or cross-check.**
  MAO derives where it can (the MCM version stamp is read from
  `kPluginVersion`); the perk DESCs vs the DLL ladder are hand-matched and
  five P2 perks currently describe effects the DLL doesn't implement yet —
  audit before shipping the tree.
- **Don't ship VMADs for scripts you don't ship.** Orphaned script refs
  spammed Papyrus errors in every MEO 1.0.x zip; MAO's one VMAD (MCM quest →
  `MAO_MCM.pex`) deploys with the mod — the 98f45aa audit keeps it that way.
- **Any linked-record walk needs a cycle guard.** Mutually-pointing NNAM
  chains hung MEO's installer; MAO's `RankChain` is seen-set guarded
  (Commands.Patch.cs:87-97).
- **Never index by another record's FormKey link without a guard.** One
  dangling link (missing master) crashed MEO's entire Synthesis run.
- **Setup/interactive code outside the top-level try = invisible death.**
  The Wine double-click console vanishes with no message; MAO's load-order
  read lives inside the handler (Program.cs:58-106).
- **Don't build on a host load order that silently omits plugins.**
  Synthesis's order drops Creation Club (~24% of MEO's conversions); MAO's
  Synthesis patcher builds its own tier load order from `Skyrim.ccc`.
- **The .sln entry and the .synth SelectedProject are one string — match
  them VERBATIM and assert it in CI.** The backslash mismatch shipped MEO's
  Synthesis path broken for three versions; MAO's `installer.yml` asserts.
- **Classify records by BEHAVIOR (entry points/conditions), never by
  name/EditorID.** LoreRim's winning alchemy tree is 20 nodes of Requiem +
  Ordinator remnants — a name list dies instantly; MAO's craft classifier is
  an entry-point census (Docs/PERK_TREE_RECON.md).

## Process

- **A stale binary voids every in-game test.** Check the log's version
  header before believing any result — MEO was bitten twice; MAO stamps
  `kPluginVersion` into the log, console, and MCM Debug page.
- **Fix stale landmark comments in save-critical code as if they were
  bugs.** MEO's "schema v3" (was v11) steered real decisions. MAO has one
  live today: plugin.cpp:2029 says "'POCH' record (schema v1)" while
  `kSerVersion` is 3 — fix it.
- **UI-visible bugs deserve a zero-code (or log-only) control before
  theorizing.** MEO solved its rename mystery with a renamed control item;
  MAO's field-open diagnostic dumps every slot's charges at the boundary
  (plugin.cpp:362-373) for exactly this — capture state where the symptom
  starts.
- **Testing relaxations must be one compile-time constant, commented with
  the flip-back condition.** `kCoatingsRequireVanguard = false`
  (plugin.cpp:199) is the pattern: every gate keys off it, one flip restores
  the shipping behavior — scattered `// TODO re-enable` checks get shipped.
- **One hook class per build, so a CTD bisects to one change** (MRO/MAO
  BUILD.md doctrine — sinks before code hooks, every milestone CI-green and
  tested in-game before the next is written).
