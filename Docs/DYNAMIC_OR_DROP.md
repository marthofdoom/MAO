# Dynamic-or-Drop Tracker for v1.0

Rule (set 2026-07-04): anything whose behavior depends on values baked in
from THIS machine's load order at ESP-generation time must either become
dynamic (computed at runtime in-game) or be dropped before a true 1.0
release. The shipped mod must behave as advertised on any load order.

Status values: DROP-CANDIDATE (no dynamic path found yet),
FIXABLE (dynamic path identified, not yet implemented), PORTABLE (fine as-is).

## DROP-CANDIDATE

(none — vendor gold resolved natively in v0.6.0)

### Vendor gold doubling — RESOLVED 2026-07-04 (v0.6.0, native M1)
MRO.dll doubles the 13 vanilla VendorGold* lists in memory at
kDataLoaded — fully dynamic on any load order. Baked LVLI records and
the generator's load-order scan retired. Original notes kept below.

### (historical) Vendor gold doubling
- Baked: 13 LVLI overrides with counts doubled from the winning override
  found by scanning this machine's plugins.txt/modlist.txt.
- Why not dynamic yet: leveled-list counts cannot be modified at runtime
  from Papyrus (PO3 only reads them; no SKSE scripting API writes them).
- Possible escape hatches to investigate before dropping:
  1. Runtime gold top-up: on "BarterMenu" open, resolve the merchant
     faction's chest via Faction.GetMerchantContainer(), count its gold,
     and AddItem an equal amount once per restock cycle. Needs careful
     bookkeeping (don't double-dip within one reset; track per-vendor).
  2. Our own tiny SKSE DLL that patches leveled lists at load. Breaks the
     "pure Papyrus + ESP" constraint; new toolchain (C++/CommonLibSSE).
- If neither pans out: DROP for 1.0.

## FIXABLE (make dynamic before 1.0)

### Physical DR curve kink — RESOLVED 2026-07-04 (v0.4.0)
- Now read live from fMaxArmorRating / fArmorScalingFactor each update;
  the 99% point is an MCM slider (MRO_T_DR99Armor, default 2000).
- Residual caveat: the 24 perk multipliers assume a ~75% engine cap;
  a load order with a very different cap shifts the top end slightly.
  Exact on any cap requires the native rewrite.

## GENERATION-DERIVED DEFAULTS (acceptable: runtime slider overrides)

### DR99Armor default (added v0.5.0)
- The 99%-DR armor target's DEFAULT is estimated at generation time from
  the load order's best obtainable heavy set (+ tempering headroom).
  LoreRim: best set 1760 + temper 600 -> target 3000.
- Acceptable under the 1.0 rule because it is only a default — the MCM
  slider is the runtime authority. Revisit if the native rewrite lands
  (could compute in-game).

## PORTABLE (verified fine as-is)

- fPlayerMaxResistance=10000 GMST override — generic.
- Absorb system — reads resistances/effects at runtime, fully generic.
- DR perk ladder records themselves — static multipliers, generic
  (only the rung-selection formula needs the FIXABLE change above).
- Mastery action costs — derived from VANILLA Skyrim.esm AVSK records,
  not the load order; portable by construction.
- Mastery bonuses (+300 armor etc.) — design constants, not baked scans.
- ~~Mod detections (World Eater's Influence, Immersive Miraak, Experience
  DLL) — runtime file/plugin checks.~~ **The Boss Readiness page (and all
  its boss/mod detection) was DROPPED in v0.8.0** — even with runtime
  plugin checks the readiness thresholds were a hardcoded heuristic per
  known boss, not a real per-load-order capability, so it misled more than
  it helped. Truly dynamic boss detection is infeasible; removed rather
  than shipped half-accurate.
- Optional Experience.ini — a user-chosen optional file, clearly labeled;
  static tuning is acceptable for an optional.
- SEQ file, FOMOD, ESL flagging — portable packaging.

## Considered direction: native hybrid (CommonLibSSE-NG)

User decision 2026-07-04: evaluate a partial rewrite as one small native
SKSE DLL (C++/CommonLibSSE-NG — the modern ecosystem standard) alongside
the existing ESP + Papyrus shell. It would resolve in one move:
- Vendor gold DROP-CANDIDATE (patch LVLI counts at data-load, dynamic)
- DR curve FIXABLE + the 24-perk quantization + 30s lag (damage hook,
  exact continuous formula, player/follower scoping)
- Absorb base-magnitude approximation (hook real damage application)
- Global resist-cap caveat (per-actor clamp hook)
Papyrus keeps: MCM, mastery bookkeeping, intro/upgrade logic.
Costs: C++ toolchain (Windows-centric; build via CI or msvc-wine from
Linux), CTD risk from native bugs, engine-update maintenance (mitigated
by Address Library). Tooling: ClibDT (2026), vcpkg/Conan templates.

## Release gate

Before tagging 1.0: every DROP-CANDIDATE resolved (implemented dynamic or
removed from ESP + texts), every FIXABLE implemented, and a clean-install
test on a non-LoreRim Requiem setup if available.
