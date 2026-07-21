// MAO native plugin — MAO.dll (CommonLibSSE-NG).
//
// Marth's Alchemy Overhaul: flask-based alchemy. Single-use potions are
// replaced by permanent Flasks charged from abstracted Material Essences the
// player gathers in the field. State lives here and serializes to the SKSE
// co-save — no Papyrus save bloat. See Docs/DESIGN.md and Docs/P0_PLAN.md.
//
// Built in staged, individually CI-green milestones (Docs/BUILD.md), one hook
// class per step so a CTD bisects to one change (MRO doctrine):
//   M0: skeleton — DLL loads, logs version. Zero hooks.
//   M1: THE GATHERING LOOP. TESContainerChanged sink is the sole essence-
//       credit point; TESHarvested tags harvests; pouch persists in 'POCH'.
//   M2: THE FIELD KIT VIEWER. An ImGui overlay (D3D11 present
//       thunk + input dispatch thunk — the Wheeler pattern, ported verbatim
//       from MEO's proven hook sites) shows the essence stores, read-only.
//       Opened by a hotkey for now (Data/SKSE/Plugins/MAO.ini iOpenHotkey);
//       DESIGN §3.3 makes this a lesser POWER in P1, when the ESP that grants
//       the power exists for flasks/blueprints anyway. The render hook is a
//       CODE hook — its address-library IDs are proven live on 1.6.1170 in
//       MEO; re-verify with tools/verify_hook_site_live.py before trusting a
//       new runtime.
//
// ── SAVE-SAFETY RULES (inherited from MEO; apply from the first co-save) ──
//   1. The co-save 'POCH' schema is VERSIONED. Readers for every shipped
//      version stay forever; writers write only the newest. Never reorder or
//      remove fields — extend via a version bump + migration in LoadCallback.
//   2. MAO.esp FormIDs (once the ESP exists in P1) are frozen generator
//      constants. Forms are only ever ADDED, never renumbered or deleted.

#include <spdlog/sinks/basic_file_sink.h>

// Render hook pulls in d3d11.h -> windows.h; NG never includes it, so guard
// the min/max macros and drop wingdi's GetObject (it hijacks
// RE::BGSDefaultObjectManager::GetObject<T>).
#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi.h>
#ifdef GetObject
#    undef GetObject
#endif

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>
#include <vector>

namespace {

constexpr auto kPluginVersion = "1.0.1";

constexpr std::uint32_t kSerID         = 'MAO1';
constexpr std::uint32_t kRecPouch      = 'POCH';
constexpr std::uint32_t kRecBlueprints = 'BLPT';
constexpr std::uint32_t kRecFlasks     = 'FLSK';
constexpr std::uint32_t kSerVersion    = 3;  // v3: BLPT stores discovered potion FORMS (variants)
constexpr RE::FormID    kPlayerID      = 0x14;

// MAO.esp forms (P1a). FROZEN — must match MAO_GenerateESP.py. ESL-flagged
// plugin, so LookupForm takes the low local ID + the plugin name.
constexpr const char* kPluginName      = "MAO.esp";
constexpr RE::FormID  kFieldKitSpellID = 0x801;  // Open Field Kit lesser power (dormant)
constexpr RE::FormID  kFlaskBaseID     = 0x810;  // MAO.esp flasks 0x810..0x815
RE::SpellItem*        g_fieldKitSpell  = nullptr;
// The 6 dedicated flask AlchemyItem forms (slot i ↔ g_flaskForms[i]). Their
// NAME is renamed at runtime to the configured variant (the drink hook casts
// the variant for the actual effect); the item IDENTITY is stable across
// reconfigure so Favorites/Wheeler bindings survive. (The item card still shows
// the ESP placeholder effect — correct-tooltip/auto-mod effect mutation is a
// later, clone-based refinement.)
std::array<RE::AlchemyItem*, 6> g_flaskForms{};
// The flask's BAKED placeholder effect (Restore Health 5/0 from the ESP),
// snapshotted at kDataLoaded before any variant mirror. Restored when a slot is
// reset OR configured with a COATING — a hostile mirrored effect would flip the
// engine's IsPoison() true and reroute the flask off the DrinkPotion hook,
// destroying the permanent item (Fable v0.22.2 review).
// Primitive fields (not RE::EffectItem by value — it's an incomplete type this
// early in the TU). magnitude/area/duration are what the item card reads.
struct FlaskPlaceholder {
    RE::EffectSetting* eff = nullptr;
    float              mag = 0.0f;
    std::uint32_t      area = 0, dur = 0;
    bool               ok = false;
};
std::array<FlaskPlaceholder, 6> g_flaskPlaceholder{};

// ── Essence pouch: the abstracted store. Three tier counters, nothing more.
// This is the entire P0 persisted state; it lives here and serializes to the
// co-save, never as an inventory item.
enum class Tier : std::uint8_t { Base = 0, Catalyst = 1, Apex = 2 };

// Atomic counters: written on the task thread (credits/spends/load), read
// every frame by the render thread (the field-kit essence line) — the last
// non-atomic shared globals from the 5525ab5 pass (doc-audit follow-up).
struct Pouch {
    std::atomic<std::uint32_t> base{ 0 };
    std::atomic<std::uint32_t> catalyst{ 0 };
    std::atomic<std::uint32_t> apex{ 0 };

    void Reset() {
        base.store(0);
        catalyst.store(0);
        apex.store(0);
    }
};
Pouch g_pouch;

// ── Discovered potions (P1d): the SPECIFIC potion forms the player has
// analyzed (found or bought). Each is a usable variant — a Superior potion is
// only available once you've actually found/bought one, not derived. A flask
// is configured with one discovered variant, which becomes its permanent item
// and what it casts on drink. Beneficial variants are drinkable flasks;
// detrimental ones are coatings (P2). Persisted in 'BLPT'. Mutex-guarded.
std::unordered_set<RE::FormID> g_discovered;  // AlchemyItem FormIDs
std::mutex                     g_discoveredLock;

// ── Flasks (P1c). Permanent kit slots, each holding a blueprint payload +
// charges. Native slots for now (physical drinkable item forms + drinking
// arrive in P1d). Slot/charge counts are the unperked baseline here; P1e
// scales them via the perk ladder. Guarded — render reads, task writes.
constexpr std::size_t kMaxFlaskSlots = 6;  // design ceiling (6 flasks / 9 charges)
static_assert(kMaxFlaskSlots == 6, "g_flaskForms[6] and MAO_GenerateESP NUM_FLASKS assume 6");

struct Flask {
    RE::FormID    blueprint = 0;  // primary-effect MGEF (for cost/coating/refill); 0 = empty
    RE::FormID    repPotion = 0;  // the physical flask item (rep potion form); 0 = none
    std::uint32_t charges   = 0;
};
std::array<Flask, kMaxFlaskSlots> g_flasks{};
std::uint32_t                     g_flaskCount     = 2;  // unlocked slots (P1e: perk-scaled)
std::uint32_t                     g_chargesPerFlask = 2;  // max charges (P1e: perk-scaled)
std::mutex                        g_flasksLock;

// ── P1e: perk-driven capacity + efficiency (DESIGN §5). The vanilla Skyrim.esm
// alchemy perks are resolved at kDataLoaded; capacity is RECOMPUTED from them
// (perks are authoritative over the co-saved counts). Vanilla "Alchemist" is 5
// chained perk records, so the ladder reads purely off HasPerk — the highest
// chained rank owned is the effective rank; no rank-field reading needed. Under
// a perk overhaul (Requiem) these FormIDs may not resolve; capacity then holds
// at the unperked baseline until the FOMOD retargets them (P2).
// The alchemy perks MAO reads, table-driven so the debug menu can enumerate and
// toggle each individually. Indices are fixed (capacity logic references them);
// PK_ALCH1..5 are the Kit Calibration chain (MAO's own NNAM-chained ranks 1..5,
// the vanilla-Alchemist analog — enum names kept for the stable MCM debug keys).
enum PerkIdx : int {
    PK_ALCH1, PK_ALCH2, PK_ALCH3, PK_ALCH4, PK_ALCH5,
    PK_CAPSTONE, PK_BENEFACTOR, PK_EXPERIMENTER,
    PK_PHYSICIAN, PK_POISONER, PK_GREENTHUMB, PK_SNAKEBLOOD, PK_CONCPOISON,
    PK_COUNT
};
// MEO perk vehicle (verbatim port): ALL perks are MAO's OWN flag records in
// MAO.esp (frozen band: capstone 0x816, flags 0x818..0x823 — MAO_GenerateESP
// MAO_PERKS order). Vanilla alchemy perks are no longer touched (no renames):
// the installer-written "MAO - Patch.esp" removes the vanilla craft nodes from
// the tree and inserts these; without the patch the DLL AUTO-GRANTS them at
// their skill thresholds (req = the record's CTDA GetBaseActorValue gate,
// mirrored here). Vanilla survivors (Snakeblood, Green Thumb) keep their own
// vanilla effects and are none of our business.
struct PerkDef { const char* label; RE::FormID id; const char* iniKey; float req; };
constexpr PerkDef kPerkDefs[PK_COUNT] = {
    { "Kit Calibration I  (+1 charge)",        0x818, "bPerkAlch1",        0.0f },
    { "Kit Calibration II  (+1 flask)",        0x819, "bPerkAlch2",        20.0f },
    { "Kit Calibration III  (refill -15%)", 0x81A, "bPerkAlch3",        40.0f },
    { "Field Deployment  (+1 flask +2 chg)",   0x81B, "bPerkAlch4",        60.0f },
    { "Kit Calibration V  (rest refill -50%)",  0x81C, "bPerkAlch5",        80.0f },
    { "Master's Crucible  (+2 flask +4 chg)",  0x816, "bPerkCapstone",     100.0f },
    { "Apex Stabilization  (Apex -35%)",       0x81F, "bPerkBenefactor",   30.0f },
    { "Field Extraction  (+10% gather)",       0x820, "bPerkExperimenter", 50.0f },
    { "Fluid Motion  (P2)",                    0x81D, "bPerkPhysician",    20.0f },
    { "Vanguard Coating  (coatings)",          0x81E, "bPerkPoisoner",     30.0f },
    { "Pouch Expansion  (15% Catalyst proc)",                 0x822, "bPerkGreenThumb",   70.0f },
    { "Extended Synthesis  (+30s buffs)",              0x823, "bPerkSnakeblood",   80.0f },
    { "Corrosive Retention  (120s coatings)",             0x821, "bPerkConcPoison",   60.0f },
};
RE::BGSPerk*               g_perkForm[PK_COUNT] = {};
// TREE mode = "MAO - Patch.esp" present (perks live in the AVAlchemy
// constellation, bought with perk points). Otherwise the DLL auto-grants them
// at their skill thresholds (MEO's interim vehicle for patch-less installs).
std::atomic<bool>          g_treeMode{ false };
std::atomic<std::uint32_t> g_perkMask{ 0 };  // bit i = kPerkDefs[i] is effectively active
// MCM debug (MEO-style override): when g_perkDebug, capacity/efficiency are
// driven by g_perkWantMask (the MCM toggles) INSTEAD of the real perks — no
// AddPerk/RemovePerk, so real progression is never disturbed and it reverts
// cleanly when the toggle is turned off.
std::atomic<bool>          g_perkDebug{ false };
std::atomic<std::uint32_t> g_perkWantMask{ 0 };
bool              g_capacityPerksResolved = false;  // MAO.esp Kit Calibration chain resolved?
std::atomic<int>  g_alchemistRank{ 0 };
std::atomic<bool> g_hasCapstone{ false };
std::atomic<bool> g_hasBenefactor{ false };   // read in VariantCost (render + task)
std::atomic<bool> g_hasExperimenter{ false };  // read in the gather credit (task)
std::atomic<bool> g_hasVanguard{ false };   // Vanguard Coating -> coatings unlocked
std::atomic<bool> g_hasCorrosive{ false };  // Corrosive Retention -> 120s coating window
std::atomic<bool> g_hasCal3{ false };       // Kit Calibration III -> T1/T2 refill cost -15%
std::atomic<bool> g_hasCal5{ false };       // Kit Calibration V -> T1/T2 sleep-refill cost halved
std::atomic<bool> g_hasPouchExp{ false };   // Pouch Expansion -> 15% bonus Catalyst per harvest
std::atomic<bool> g_hasExtSynth{ false };   // Extended Synthesis -> +30s drinkable buff durations

// Fluid Motion only means something when a drink-animation mod imposes a
// movement penalty — MAO's own drinks are instant (§Why Native). The perk
// CHECKS FOR ANIMATION MODS AND DISABLES ITSELF IF NONE IS FOUND (marth
// 2026-07-16): no auto-grant, no cached flag, and the tree patcher skips its
// node. Known mods, by plugin and by SKSE dll (extend as they surface):
constexpr std::array<const char*, 4> kDrinkAnimPlugins{
    "Animated Potions.esp", "Animated Poisons.esp",
    "zxlice ultimate potion animation.esp", "ZUPA.esp"
};
constexpr std::array<const char*, 2> kDrinkAnimDlls{
    "Data/SKSE/Plugins/AnimatedPotions.dll", "Data/SKSE/Plugins/AnimatedPoisons.dll"
};
std::atomic<bool> g_drinkAnimMod{ false };  // set at kDataLoaded
std::atomic<bool> g_hasFluidMotion{ false };  // perk held AND an anim mod present

// DESIGN: coatings unlock with the Vanguard Coating perk. The v0.18.1 testing
// relaxation is over (marth 2026-07-16: "coatings are active without the perk
// — wrong"): the gate is LIVE. Without the perk, detrimental blueprints can be
// discovered but not prepared or applied.
constexpr bool kCoatingsRequireVanguard = true;

int RankFromMask(std::uint32_t a_mask) {
    int r = 0;
    for (int i = PK_ALCH1; i <= PK_ALCH5; ++i) {
        if (a_mask & (1u << i)) {
            r = i - PK_ALCH1 + 1;  // highest chained rank present
        }
    }
    return r;
}
// Recompute flask/charge capacity + efficiency flags from the player's perks —
// or, when the MCM debug override is on, from its toggle bits instead. Safe to
// call whenever the perk set may have changed (load, menu open, MCM close).
void RecomputeCapacity(const char* a_why) {
    auto* pc = RE::PlayerCharacter::GetSingleton();
    if (!pc) {
        return;
    }
    // AUTO-GRANT (MEO's patch-less vehicle): without the installer-written
    // tree patch the player has no node to buy these from, so grant each flag
    // perk once its skill threshold is met. Runs on the task thread (all
    // callers AddTask this). Real perks only — the MCM debug override below
    // never touches them.
    auto* avo = pc->AsActorValueOwner();
    if (!g_treeMode.load() && g_capacityPerksResolved && avo) {
        const float skill = avo->GetBaseActorValue(RE::ActorValue::kAlchemy);
        for (int i = 0; i < PK_COUNT; ++i) {
            if (i == PK_PHYSICIAN) {
                continue;  // Fluid Motion: effect not implemented yet — never
                           // auto-grant a no-op perk (Fable v0.23.0 SF1). The
                           // drink-anim detection stays for when the effect lands.
            }
            if (g_perkForm[i] && skill >= kPerkDefs[i].req && !pc->HasPerk(g_perkForm[i])) {
                pc->AddPerk(g_perkForm[i], 1);
                spdlog::info("[perks] auto-grant '{}' (Alchemy {} >= {})", kPerkDefs[i].label,
                             skill, kPerkDefs[i].req);
            }
        }
    }
    std::uint32_t realMask = 0;
    for (int i = 0; i < PK_COUNT; ++i) {
        if (g_perkForm[i] && pc->HasPerk(g_perkForm[i])) {
            realMask |= (1u << i);
        }
    }
    const bool          debug = g_perkDebug.load();
    const std::uint32_t eff   = debug ? g_perkWantMask.load() : realMask;
    g_perkMask.store(eff);
    g_hasBenefactor.store((eff & (1u << PK_BENEFACTOR)) != 0);
    g_hasExperimenter.store((eff & (1u << PK_EXPERIMENTER)) != 0);
    g_hasVanguard.store((eff & (1u << PK_POISONER)) != 0);
    g_hasCorrosive.store((eff & (1u << PK_CONCPOISON)) != 0);
    g_hasCal3.store((eff & (1u << PK_ALCH3)) != 0);
    g_hasCal5.store((eff & (1u << PK_ALCH5)) != 0);
    g_hasPouchExp.store((eff & (1u << PK_GREENTHUMB)) != 0);
    g_hasExtSynth.store((eff & (1u << PK_SNAKEBLOOD)) != 0);
    // Fluid Motion self-disables without a drink-animation mod to modify.
    g_hasFluidMotion.store(((eff & (1u << PK_PHYSICIAN)) != 0) && g_drinkAnimMod.load());
    // If MAO.esp's Kit Calibration chain didn't resolve (stale/missing ESP),
    // hold the co-saved counts rather than force the baseline and clamp away
    // charges the player legitimately earned. This applies EVEN in debug: the
    // override would otherwise write debug capacity into g_flaskCount here and,
    // once debug is turned back off, this same hold-path would "hold" those
    // debug values (and SaveCallback would persist them). Debug capacity only
    // takes effect where we can cleanly revert to real perks.
    if (!g_capacityPerksResolved) {
        spdlog::info("[perks] {}: no capacity perks resolved (MAO.esp stale/missing?) — "
                     "holding co-saved {} flasks / {} charges",
                     a_why, g_flaskCount, g_chargesPerFlask);
        return;
    }
    const int  rank     = RankFromMask(eff);
    const bool capstone = (eff & (1u << PK_CAPSTONE)) != 0;
    std::uint32_t flasks = 2, charges = 2;  // unperked baseline
    if (rank >= 1) charges += 1;                     // Kit Calibration I   -> 2/3
    if (rank >= 2) flasks += 1;                      // Kit Calibration II  -> 3/3
    if (rank >= 4) { flasks += 1; charges += 2; }    // Field Deployment    -> 4/5
    if (capstone)  { flasks += 2; charges += 4; }    // Master's Crucible   -> 6/9
    flasks  = std::min<std::uint32_t>(flasks, kMaxFlaskSlots);
    charges = std::min<std::uint32_t>(charges, 9u);
    g_alchemistRank.store(rank);
    g_hasCapstone.store(capstone);
    {
        std::scoped_lock lk(g_flasksLock);
        g_flaskCount      = flasks;
        g_chargesPerFlask = charges;
        for (auto& f : g_flasks) {  // never leave a flask above its new cap
            f.charges = std::min(f.charges, charges);
        }
    }
    spdlog::info("[perks] {}{}: Calibration rank {}, Crucible {}, ApexStab {}, FieldExtract {} "
                 "-> {} flasks / {} charges",
                 a_why, debug ? " [DEBUG override]" : "", rank, capstone, g_hasBenefactor.load(),
                 g_hasExperimenter.load(), flasks, charges);
}

// ── Essence economy: per-effect flask cost, computed from the load order's
// own ingredients (native, DYNAMIC_OR_DROP-correct — no data file). For each
// effect, cost-per-charge = round(2 * mean(source ingredient gold value) *
// TAX): a potion is ~2 ingredients, so 2*mean is the vanilla recipe cost, and
// TAX is the convenience tax over it. Denominating cost in ingredient value
// (the same currency the player earns) keeps the tax an honest %, unlike
// potion record values which run ~5-7x their ingredients (see
// essence-economy-tax memory). Built at kDataLoaded; guarded for the render
// read.
// Per-effect cheapest 2-ingredient recipe, kept as its TWO ingredients so the
// base cost splits per ingredient IN ITS OWN TIER (a Base flower + a Catalyst
// plant → part Base + part Catalyst). Base cost = each ingredient's value ×
// quality x tax, added to that ingredient's tier pool.
struct RecipeIng { std::uint32_t value = 5; Tier tier = Tier::Base; RE::FormID form = 0; };
struct Recipe { RecipeIng a, b; };
std::unordered_map<RE::FormID, Recipe> g_effectRecipe;
std::mutex    g_effectCostLock;
// Tuning globals are atomic: ReadConfig() now re-runs mid-session (on MCM/menu
// close) on the task thread while these are read from the render / refill /
// input / event-sink threads. Aligned scalars don't tear on x86-64, but atomic
// makes the concurrent read/write well-defined (matches g_perkDebug's pattern).
std::atomic<float>                            g_essenceTax = 1.3f;   // fEssenceTax
// Flask cost scales linearly with the variant's QUALITY (VariantQuality:
// cross-effect, median potion = 1.0), and the flask's ESSENCE TIER
// REQUIREMENT steps up at the two quality thresholds below — above
// fCatalystQuality it also needs Catalyst, at/above fApexQuality it also needs
// Apex, independent of what the recipe's ingredients are (DESIGN §1/§2).
// RENAMED from fCatalystLevel/fApexLevel because the SEMANTICS changed
// (concentration-x -> quality-x, a different scale): MCM Helper persists values
// per key name forever, so a stale 3.0/6.0 would silently mis-apply on the new
// scale (INVARIANTS: rename any key whose meaning changes).
std::atomic<float> g_costRate = 1.0f;  // fCostRate
std::atomic<float> g_catalystQuality = 1.0f;  // fCatalystQuality — ABOVE this, Catalyst is required
std::atomic<float> g_apexQuality     = 1.2f;  // fApexQuality — at/above this, Apex is required

// Effect -> the strongest load-order potion/poison carrying it as its primary
// effect. This is what physically embodies a blueprint (the flask item + what
// the drink hook casts). Deriving it from the load order — instead of only the
// exact potion you found — means every discovered blueprint is usable, incl.
// blueprints from pre-P1d saves that never stored a representative.
std::unordered_map<RE::FormID, std::pair<RE::FormID, std::uint32_t>> g_effectPotion;
std::unordered_map<RE::FormID, float> g_effectMinMag;  // MGEF -> weakest (standard) primary-effect magnitude
std::mutex                            g_effectPotionLock;
// Median potion gold value in the load order — the QUALITY reference. Set at
// kDataLoaded by BuildEffectPotionTable; 1.0 until then.
std::atomic<float>                    g_medianPotionValue{ 1.0f };

// Flask helpers — defined below near the container sink; forward-declared for
// ConfigureFlask / the drink hook above them.
int  FindFlaskSlot(RE::FormID a_form);
bool IsFlaskForm(RE::FormID a_form);
void RenameFlask(std::size_t a_slot, RE::AlchemyItem* a_variant);
void RestoreFlaskPlaceholder(std::size_t a_slot);

// ── Field Kit viewer state (M2). open/close is driven by the input hook and
// read by the present hook — both run off the game's render/input threads, so
// these are atomics.
struct MenuState {
    std::atomic<bool> open{ false };
    std::atomic<bool> cursorInit{ false };  // push cursor to center on next open
    std::atomic<bool> station{ false };     // opened from an alchemy station (takeover)
};
MenuState g_menu;

// Field mode (power opener) is a LIMITED kit: view essences + change up to
// kFieldChangeLimit flask(s) per trip, and a field change starts the flask EMPTY
// (0 charges, no upfront essence) — it fills from the refill loop over time. A
// station is the full kit (full charges, full cost, unlimited changes).
constexpr int    kFieldChangeLimit = 1;
std::atomic<int> g_fieldChanges{ 0 };  // reconfigurations used this field session

// Ingredient→essence conversion toggle (bConversionEnabled). When OFF, picked-up
// and harvested ingredients stay in the inventory as normal items instead of
// being consumed into essence — the rest of the mod (flasks, field kit, power,
// perks) is unaffected. Read live from the MCM.
std::atomic<bool> g_conversionEnabled{ true };

// Defined with the ingredient-mode machinery below; the kit opener refreshes
// the menu's ingredient-count snapshot.
void RefreshHeldIngredients();

// Open/close the Field Kit. When opened from a station, closing forces the
// player up out of the furniture (the vanilla crafting menu was hidden).
void OpenFieldKit(bool a_station) {
    if (!a_station) {
        g_fieldChanges.store(0);  // fresh field trip
    }
    g_menu.station.store(a_station);
    g_menu.cursorInit.store(true);
    g_menu.open.store(true);
    // Diagnostic (Bug B — "field change empties other flasks"): dump every slot's
    // charge state at open so a repro shows exactly which slots held what.
    {
        std::scoped_lock lk(g_flasksLock);
        std::string dump;
        for (std::uint32_t i = 0; i < g_flaskCount; ++i) {
            dump += std::format("[{}]bp={:X} {}/{}  ", i, g_flasks[i].blueprint, g_flasks[i].charges,
                                g_chargesPerFlask);
        }
        spdlog::info("[field] open ({}): {}", a_station ? "station" : "field", dump);
    }
    // Refresh perk-scaled capacity in case a perk was taken since the last load,
    // and the ingredient-count snapshot the menu's affordability reads.
    SKSE::GetTaskInterface()->AddTask([]() {
        RecomputeCapacity("open");
        RefreshHeldIngredients();
    });
}
void CloseFieldKit() {
    const bool wasStation = g_menu.station.exchange(false);
    g_menu.open.store(false);
    if (wasStation) {
        SKSE::GetTaskInterface()->AddTask([]() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && player->GetOccupiedFurniture()) {
                player->NotifyAnimationGraph("IdleForceDefaultState");
            }
        });
    }
}

// ── Config (Data/SKSE/Plugins/MAO.ini). The full MCM option set arrives with
// P1; P0 seeds the surface with the keys it needs.
std::atomic<bool> g_notify   = true;  // bNotify — per-pickup essence notifications
std::atomic<std::uint32_t> g_openHotkey = 0x25;  // iOpenHotkey — keyboard DirectInput scancode; 0x25 = K
// iOpenButtonGamepad — RE::BSWin32GamepadDevice::Key bitflag, or 0 = disabled.
// DEFAULT 0: the power is the opener. On the Steam Deck the View button (0x20)
// doubles as Select, so binding an opener there collides — leave it off.
std::atomic<std::uint32_t> g_openButtonGamepad = 0x0;
std::atomic<int> g_menuStyle = 0;  // iMenuStyle — Field Kit skin 0..3
std::atomic<std::uint32_t> g_refillSeconds = 300;  // iRefillSeconds — real-time refill cadence

const char* TierName(Tier a_t) {
    switch (a_t) {
    case Tier::Catalyst: return "Catalyst";
    case Tier::Apex:     return "Apex";
    default:             return "Base";
    }
}

// ── P0 tier map: a hardcoded name-substring stand-in that exercises all three
// buckets. Everything is Base unless it matches an override. P1 replaces this
// with an identity (FormID) map generated from the game masters
// (DYNAMIC_OR_DROP applies). Name-based is a deliberate prototype shortcut —
// transparent and easy to extend — not the shipping mechanism.
struct TierOverride { std::string_view needle; Tier tier; };
// Ingredient TIER by rarity/role (DESIGN §1), NOT gold value — value only sets
// the amount of essence. Catalyst = rare plants + specialized creature drops;
// Apex = boss/Daedra/vampire drops. Name-substring match (interim; a generated
// FormID map is the eventual replacement). Everything unlisted is Base.
constexpr std::array<TierOverride, 18> kTierOverrides{ {
    // Apex
    { "Daedra Heart",     Tier::Apex },
    { "Vampire Dust",     Tier::Apex },
    { "Void Salts",       Tier::Apex },
    { "Briar Heart",      Tier::Apex },
    // Catalyst — rare plants
    { "Nightshade",       Tier::Catalyst },
    { "Deathbell",        Tier::Catalyst },
    { "Nirnroot",         Tier::Catalyst },
    { "Gleamblossom",     Tier::Catalyst },
    { "Spriggan Sap",     Tier::Catalyst },
    // Catalyst — specialized creature drops
    { "Falmer Ear",       Tier::Catalyst },
    { "Sabre Cat Tooth",  Tier::Catalyst },
    { "Hagraven Claw",    Tier::Catalyst },
    { "Hagraven Feathers", Tier::Catalyst },
    { "Chaurus Eggs",     Tier::Catalyst },
    { "Ectoplasm",        Tier::Catalyst },
    { "Human Heart",      Tier::Catalyst },
    { "Human Flesh",      Tier::Catalyst },
    { "Netch Jelly",      Tier::Catalyst },
} };

// Quest-reagent exclusion — these must NEVER dissolve into essence or be
// analyzed. The PRIMARY guard is the runtime quest-object flag (IsQuestItem
// below), which catches ANY quest-flagged item (vanilla or modded) without
// touching normal gathering of the same ingredient. This NAME list is a
// deterministic backstop for always-quest-only items that have no ordinary
// alchemy use, in case the flag isn't set at the pickup event. Do NOT add
// common ingredients here (Deathbell/Nightshade/Nirnroot etc. are legitimate
// gather targets — the quest-flag guard protects their quest instances).
// (Release blocker fix, Fable v0.23.0 review: default mode destroyed
// quest-required items and soft-locked vanilla quests incl. two Daedric.)
constexpr std::array<std::string_view, 5> kExclusions{ {
    "Crimson Nirnroot",   // A Return To Your Roots
    "Vaermina's Torpor",  // Waking Nightmare (Daedric) — must be DRUNK
    "Jarrin Root",        // Death Incarnate (Dark Brotherhood)
    "Berit's Ashes",      // Wisdom of the Ages
    "White Phial",        // Repairing the Phial
} };

// Is the player's inventory entry for this object a QUEST item? Generic guard
// against converting any quest-flagged item — checks the live inventory entry's
// quest-object flag (set from a quest alias). Runs on the task thread.
bool IsQuestItem(RE::PlayerCharacter* a_player, RE::TESBoundObject* a_obj) {
    if (!a_player || !a_obj) {
        return false;
    }
    auto* changes = a_player->GetInventoryChanges();
    if (!changes || !changes->entryList) {
        return false;
    }
    for (auto* entry : *changes->entryList) {
        if (entry && entry->object == a_obj) {
            return entry->IsQuestObject();
        }
    }
    return false;
}

// ── TIER-RELATIVE VALUATION (v1.0.1 pricing redo, marth).
//
// Essence used to be the ingredient's raw gold value at rate 1.0 for EVERY
// tier. Because rarer ingredients are individually worth far more (on marth's
// list the medians are Base 2g / Catalyst 20g / Apex 62g), a single Apex
// pickup paid 31x a Base pickup — which exactly cancelled its rarity, so all
// three pools filled at the same pace ("they stack up at an equal pace").
//
// Now each tier is valued RELATIVE TO ITS OWN CATEGORY: an ingredient is worth
// its tier's unit scaled by how it compares to that tier's MEDIAN value, so a
// typical Apex drop is worth kApexUnit no matter how many gold pieces it is.
// BASE IS UNCHANGED (absolute gold value) — marth: "leave base costs as is".
// The medians are per-load-order and come from mao_tiers.json (the installer
// emits them); the defaults below are only a fallback.
constexpr float kCatalystUnit = 3.0f;  // a MEDIAN Catalyst ingredient yields this
constexpr float kApexUnit     = 1.0f;  // a MEDIAN Apex ingredient yields this
// Surcharge cost in those same units — this sets "drops per charge":
// Catalyst 6/3 = ~2 drops, Apex 3/1 = ~3 drops (marth: "several Apex drops
// per charge").
constexpr std::uint32_t kCatalystSurcharge = 6;
constexpr std::uint32_t kApexSurcharge     = 3;
// Per-tier median ingredient gold value (index by Tier). Overwritten from
// mao_tiers.json at kDataLoaded when the installer supplied stats.
std::atomic<float> g_tierMedian[3]{ { 5.0f }, { 20.0f }, { 60.0f } };

// An ingredient's worth in ITS OWN tier's essence units.
float IngredientUnits(std::uint32_t a_value, Tier a_tier) {
    const int i = static_cast<int>(a_tier);
    if (a_tier == Tier::Base) {
        return static_cast<float>(a_value);  // unchanged: absolute gold value
    }
    const float unit   = (a_tier == Tier::Apex) ? kApexUnit : kCatalystUnit;
    const float median = std::max(1.0f, g_tierMedian[i].load());
    return unit * (static_cast<float>(a_value) / median);
}

bool ContainsCI(std::string_view a_hay, std::string_view a_needle) {
    if (a_needle.empty() || a_needle.size() > a_hay.size()) {
        return false;
    }
    auto it = std::search(a_hay.begin(), a_hay.end(), a_needle.begin(), a_needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != a_hay.end();
}

Tier TierOf(std::string_view a_name) {
    for (const auto& ov : kTierOverrides) {
        if (ContainsCI(a_name, ov.needle)) {
            return ov.tier;
        }
    }
    return Tier::Base;
}

// Availability-based tier map produced by the installer (mao_tiers.json). It
// reads the real load order — Base=common, Catalyst=uncommon, Apex=scarce — so
// it supersedes the name-substring heuristic (which stays as the fallback when
// the file is absent). Populated read-only at kDataLoaded, before any gather.
std::unordered_map<RE::FormID, Tier> g_ingredientTier;

Tier TierFromString(std::string_view a_s) {
    if (a_s == "Apex") return Tier::Apex;
    if (a_s == "Catalyst") return Tier::Catalyst;
    return Tier::Base;
}

void LoadTierMap() {
    g_ingredientTier.clear();
    std::ifstream f("Data/SKSE/Plugins/MAO/mao_tiers.json");
    auto*         dh = RE::TESDataHandler::GetSingleton();
    if (!f || !dh) {
        spdlog::info("[tiers] no mao_tiers.json — using the name-substring fallback");
        return;
    }
    try {
        nlohmann::json j;
        f >> j;
        int n = 0, miss = 0;
        for (const auto& e : j.value("tiers", nlohmann::json::array())) {
            const std::string plugin = e.value("plugin", std::string{});
            const std::string fidStr = e.value("fid", std::string{});
            if (plugin.empty() || fidStr.empty()) {
                continue;
            }
            const RE::FormID raw =
                static_cast<RE::FormID>(std::strtoul(fidStr.c_str(), nullptr, 0)) & 0xFFFFFF;
            if (auto* ingr = dh->LookupForm<RE::IngredientItem>(raw, plugin)) {
                g_ingredientTier[ingr->GetFormID()] = TierFromString(e.value("tier", "Base"));
                ++n;
            } else {
                ++miss;
            }
        }
        // Per-tier median ingredient value — the reference the tier-relative
        // valuation divides by (IngredientUnits). Load-order specific, so the
        // installer emits it; older tier files without it keep the defaults.
        if (const auto st = j.find("tierStats"); st != j.end() && st->is_object()) {
            const char* names[3] = { "base", "catalyst", "apex" };
            for (int i = 0; i < 3; ++i) {
                if (const auto t = st->find(names[i]); t != st->end() && t->is_object()) {
                    const float m = t->value("median", 0.0f);
                    if (m > 0.0f) {
                        g_tierMedian[i].store(m);
                    }
                }
            }
        }
        spdlog::info("[tiers] loaded {} ingredient tiers ({} unresolved); medians B={} C={} A={}",
                     n, miss, g_tierMedian[0].load(), g_tierMedian[1].load(),
                     g_tierMedian[2].load());
    } catch (const std::exception& ex) {
        spdlog::error("[tiers] parse failed ({}) — using the name fallback", ex.what());
        g_ingredientTier.clear();
    }
}

// Tier for a specific ingredient FORM: the installer map wins; else name-heuristic.
Tier TierOfForm(RE::IngredientItem* a_ingr) {
    if (!a_ingr) {
        return Tier::Base;
    }
    if (auto it = g_ingredientTier.find(a_ingr->GetFormID()); it != g_ingredientTier.end()) {
        return it->second;
    }
    const char* nm = a_ingr->GetName();
    return TierOf(nm ? nm : "");
}

bool IsExcluded(std::string_view a_name) {
    for (const auto& ex : kExclusions) {
        if (ContainsCI(a_name, ex)) {
            return true;
        }
    }
    return false;
}

std::uint32_t YieldFor(int a_value, Tier a_tier) {
    const float raw = IngredientUnits(static_cast<std::uint32_t>(std::max(0, a_value)), a_tier);
    return static_cast<std::uint32_t>(std::max(1.0f, std::ceil(raw)));
}

void CreditPouch(Tier a_tier, std::uint32_t a_amount) {
    switch (a_tier) {
    case Tier::Catalyst: g_pouch.catalyst += a_amount; break;
    case Tier::Apex:     g_pouch.apex += a_amount;     break;
    default:             g_pouch.base += a_amount;     break;
    }
}

// Build the per-effect cost table from the actual load order's ingredients.
// Called once at kDataLoaded. Logs the real distribution so the balance can be
// eyeballed against MAO.log.
void BuildRecipeTable() {
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!dh) {
        return;
    }
    // Per effect, the (value, tier, form) of every ingredient carrying it; plus
    // the pool of Catalyst / Apex ingredient values for the surcharge units.
    struct Src { std::uint32_t value; Tier tier; RE::FormID form; };
    std::unordered_map<RE::FormID, std::vector<Src>> byEffect;
    std::vector<std::uint32_t> catVals, apexVals;
    std::uint32_t              ingredients = 0;
    for (auto* ingr : dh->GetFormArray<RE::IngredientItem>()) {
        if (!ingr || ingr->value <= 0) {
            continue;
        }
        ++ingredients;
        const Tier          it = TierOfForm(ingr);
        const std::uint32_t v  = static_cast<std::uint32_t>(ingr->value);
        if (it == Tier::Catalyst) {
            catVals.push_back(v);
        } else if (it == Tier::Apex) {
            apexVals.push_back(v);
        }
        for (auto* eff : ingr->effects) {
            if (eff && eff->baseEffect) {
                byEffect[eff->baseEffect->GetFormID()].push_back({ v, it, ingr->GetFormID() });
            }
        }
    }
    auto median = [](std::vector<std::uint32_t>& v, std::uint32_t dflt) -> std::uint32_t {
        if (v.empty()) return dflt;
        std::sort(v.begin(), v.end());
        return std::max(1u, v[v.size() / 2]);
    };
    std::scoped_lock lk(g_effectCostLock);
    g_effectRecipe.clear();
    for (auto& [id, vec] : byEffect) {
        std::sort(vec.begin(), vec.end(),
                  [](const auto& a, const auto& b) { return a.value < b.value; });
        // Cheapest 2 ingredients sharing the effect = the vanilla recipe. Keep
        // both so the base cost can split across their (possibly different)
        // tiers — and their FORMS, so ingredient mode can consume the real items.
        const auto& i0 = vec[0];
        const auto& i1 = vec.size() > 1 ? vec[1] : vec[0];
        g_effectRecipe[id] = { { std::max(1u, i0.value), i0.tier, i0.form },
                               { std::max(1u, i1.value), i1.tier, i1.form } };
    }
    spdlog::info("[econ] recipe table: {} effects from {} ingredients; tier medians C={} A={}",
                 g_effectRecipe.size(), ingredients, g_tierMedian[1].load(), g_tierMedian[2].load());
}
struct FlaskCost { std::uint32_t base = 0, catalyst = 0, apex = 0; };

void AddTier(FlaskCost& c, Tier t, std::uint32_t amt) {
    if (t == Tier::Apex) {
        c.apex += amt;
    } else if (t == Tier::Catalyst) {
        c.catalyst += amt;
    } else {
        c.base += amt;
    }
}

// ── QUALITY: how good this specific potion is, on a scale where a MEDIAN
// potion in the load order is 1.0 (v1.0.1 pricing redo).
//
// Replaces the old per-effect magnitude "concentration" (magnitude divided by
// that effect's own weakest instance), which was only coherent WITHIN one
// effect family and produced real inversions across families: an effect used by
// exactly ONE potion normalized against itself and always read 1.0 — the
// cheapest possible — so Requiem's Restore Health (Surpassing) (its own
// full-heal MGEF, 500 gold) priced BELOW (Good) (80 gold). Magnitude is also
// unusable across effects: it is 0 for duration-only effects (Invisibility),
// 9999 for full-heals, 2-5 for Fortify Smithing and 40-120 for Damage Health,
// and it cannot see duration at all (lingering poisons are entirely duration).
//
// Gold value is the load order's OWN cross-effect quality metric: it is
// monotonic along every quality ladder (Diluted->Surpassing), authored by the
// same people who balanced the potions, and prices duration in implicitly.
// The cost BASIS stays ingredient-denominated (DESIGN §2) — only this
// multiplier changed.
//
// Fallback: potions with no gold value (some mod-added flasks) keep the old
// magnitude concentration. Floored so nothing is ever free, capped so a single
// absurdly-priced modded potion can't demand the whole pouch.
constexpr float kQualityFloor = 0.25f;
constexpr float kQualityCap   = 8.0f;

float VariantQuality(RE::AlchemyItem* a_alch) {
    if (!a_alch) {
        return 1.0f;
    }
    if (const auto gv = a_alch->GetGoldValue(); gv > 0) {
        return std::clamp(static_cast<float>(gv) / g_medianPotionValue.load(), kQualityFloor,
                          kQualityCap);
    }
    // No gold value — fall back to the legacy per-effect magnitude ratio.
    if (a_alch->effects.empty() || !a_alch->effects[0] || !a_alch->effects[0]->baseEffect) {
        return 1.0f;
    }
    const float mag =
        a_alch->effects[0]->effectItem.magnitude > 0.0f ? a_alch->effects[0]->effectItem.magnitude : 1.0f;
    float minMag = 0.0f;
    {
        std::scoped_lock lk(g_effectPotionLock);
        auto             it = g_effectMinMag.find(a_alch->effects[0]->baseEffect->GetFormID());
        if (it != g_effectMinMag.end()) {
            minMag = it->second;
        }
    }
    // NOTE: this ratio is a CONCENTRATION, a different scale from the
    // value-derived quality the thresholds are now calibrated against
    // (Fable v1.0.1 S4). Compress it toward 1.0 so a value-0 potion isn't
    // handed Catalyst+Apex surcharges it never used to pay: a 2x-concentration
    // legacy potion reads ~1.0, not 2.0.
    const float conc = (minMag > 0.01f) ? std::max(1.0f, mag / minMag) : 1.0f;
    return std::clamp(std::sqrt(conc), kQualityFloor, kQualityCap);
}

// Compound per-charge cost for a specific variant (Marth's model):
//   BASE = each recipe ingredient's value x quality x tax, added to THAT
//          ingredient's own tier pool (so a Base+Catalyst recipe splits into
//          part Base + part Catalyst by design).
//   +CAT = high quality (>= fCatalystQuality) needs a tier-up surcharge;
//          doubles at >= fApexQuality.
//   +APEX = any multi-effect potion needs an Apex ingredient's worth.
// Concentration = magnitude / the effect's weakest magnitude (magnitude-ranked).
FlaskCost VariantCost(RE::AlchemyItem* a_alch) {
    FlaskCost fc{};
    if (!a_alch || a_alch->effects.empty() || !a_alch->effects[0] ||
        !a_alch->effects[0]->baseEffect) {
        return fc;
    }
    auto*  eff = a_alch->effects[0]->baseEffect;
    Recipe rec{};
    {
        std::scoped_lock lk(g_effectCostLock);
        auto             it = g_effectRecipe.find(eff->GetFormID());
        if (it != g_effectRecipe.end()) {
            rec = it->second;
        }
    }
    // Cross-effect quality (see VariantQuality) replaces the old per-effect
    // magnitude concentration.
    const float quality = VariantQuality(a_alch);
    const float basis   = quality * g_costRate * g_essenceTax;
    // BASE splits per ingredient, each priced in ITS OWN tier's units.
    for (const RecipeIng& ing : { rec.a, rec.b }) {
        AddTier(fc, ing.tier,
                std::max(1u, static_cast<std::uint32_t>(
                                 std::lround(IngredientUnits(ing.value, ing.tier) * basis))));
    }
    // GUARANTEED TIER REQUIREMENTS BY QUALITY (DESIGN §1/§2, marth). These do
    // NOT depend on the recipe's ingredient tiers: a high-quality potion
    // requires the rarer essences even when its ingredients are common
    // ("The Unperked Ceiling / Apex Route" — reaching top-quality caps means
    // burning Tier III at a deliberately inefficient rate). They are
    // INDEPENDENT and ADDITIVE — a top-tier potion needs Catalyst *and* Apex.
    // Amounts scale with quality like the base portion does.
    if (quality > g_catalystQuality) {
        fc.catalyst += std::max(kCatalystSurcharge,
                                static_cast<std::uint32_t>(std::lround(kCatalystSurcharge * quality)));
    }
    if (quality > g_apexQuality) {
        fc.apex += std::max(kApexSurcharge,
                            static_cast<std::uint32_t>(std::lround(kApexSurcharge * quality)));
    }
    // Secondary effects that match the primary's polarity price like the
    // primary: their own recipe pair at the SAME potion quality, in their own
    // tiers. A drawback (opposite polarity — Requiem's Damage-Regen side
    // effects on every standard potion) adds NOTHING: it is a cost the drinker
    // pays, not the brewer. (Replaces the flat +apexUnit for ANY multi-effect
    // potion, which priced ordinary Requiem poultices like endgame combos —
    // marth 2026-07-16: "42B and 165A. Thats not correct".)
    const bool primaryHostile = eff->IsHostile() || eff->IsDetrimental();
    for (std::size_t i = 1; i < a_alch->effects.size(); ++i) {
        auto* e2 = a_alch->effects[i];
        if (!e2 || !e2->baseEffect) {
            continue;
        }
        auto*      m2       = e2->baseEffect;
        const bool hostile2 = m2->IsHostile() || m2->IsDetrimental();
        if (hostile2 != primaryHostile) {
            continue;  // drawback / off-polarity rider: free
        }
        Recipe r2{};
        bool   have2 = false;
        {
            std::scoped_lock lk(g_effectCostLock);
            auto             it = g_effectRecipe.find(m2->GetFormID());
            if (it != g_effectRecipe.end()) {
                r2    = it->second;
                have2 = true;
            }
        }
        if (!have2) {
            AddTier(fc, Tier::Catalyst, kCatalystSurcharge);  // no recipe known — mild fallback
            continue;
        }
        // Riders price off the SAME potion quality as the primary (one potion,
        // one quality) — their own recipe pair in their own tier units.
        for (const RecipeIng& ing : { r2.a, r2.b }) {
            AddTier(fc, ing.tier,
                    std::max(1u, static_cast<std::uint32_t>(
                                     std::lround(IngredientUnits(ing.value, ing.tier) * basis))));
        }
    }
    if (g_hasBenefactor.load() && fc.apex > 0) {  // Apex Stabilization: Tier III cost -35%
        fc.apex = std::max(1u, static_cast<std::uint32_t>(std::lround(fc.apex * 0.65)));
    }
    return fc;
}

FlaskCost operator*(const FlaskCost& c, std::uint32_t m) {
    return { c.base * m, c.catalyst * m, c.apex * m };
}
bool CanAfford(const FlaskCost& c) {
    return g_pouch.base >= c.base && g_pouch.catalyst >= c.catalyst && g_pouch.apex >= c.apex;
}
// Train the vanilla Alchemy skill for MAO's alchemy work. MAO replaces potion-
// crafting, so preparing/refilling flasks should advance Alchemy just as brewing
// did — including the automatic (timer/sleep) refills, each of which is one
// charge's worth of potion-making. Must run on the game thread (SpendCost and
// the discovery sink both do).
constexpr float kAlchemyXpPerEssence = 0.1f;  // XP per essence spent (tune in-game)
constexpr float kDiscoverXp          = 3.0f;  // XP for analyzing a new variant
void AwardAlchemyXP(float a_xp) {
    if (a_xp <= 0.0f) {
        return;
    }
    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
        player->AddSkillExperience(RE::ActorValue::kAlchemy, a_xp);
    }
}

bool SpendCost(const FlaskCost& c) {
    if (!CanAfford(c)) {
        return false;
    }
    g_pouch.base -= c.base;
    g_pouch.catalyst -= c.catalyst;
    g_pouch.apex -= c.apex;
    // Spending essence = making potion(s): train Alchemy (configure + refills).
    AwardAlchemyXP(static_cast<float>(c.base + c.catalyst + c.apex) * kAlchemyXpPerEssence);
    return true;
}
std::string CostString(const FlaskCost& c) {
    std::string s;
    if (c.base) s += std::format("{}B ", c.base);
    if (c.catalyst) s += std::format("{}C ", c.catalyst);
    if (c.apex) s += std::format("{}A ", c.apex);
    return s.empty() ? std::string("free") : s;
}

// ── Ingredient mode (marth 2026-07-16: "the toggle should change the WHOLE
// system"). Conversion OFF flips the kit's currency: a flask charge costs its
// variant's RECIPE INGREDIENTS — the same cheapest-pair the essence economy
// prices from — consumed from the player's bags. Pairs per charge scale by
// concentration on the SAME thresholds the essence surcharges use, +1 for
// multi-effect, so the two economies stay mirror images. The essence pouch
// persists untouched while OFF and resumes when toggled back on.
bool IngredientMode() { return !g_conversionEnabled.load(); }

struct IngrCharge { RE::FormID a = 0, b = 0; std::uint32_t pairs = 0; };

IngrCharge IngrCostPerCharge(RE::AlchemyItem* a_alch) {
    IngrCharge ic{};
    if (!a_alch || a_alch->effects.empty() || !a_alch->effects[0] ||
        !a_alch->effects[0]->baseEffect) {
        return ic;
    }
    auto*  eff = a_alch->effects[0]->baseEffect;
    Recipe rec{};
    {
        std::scoped_lock lk(g_effectCostLock);
        auto             it = g_effectRecipe.find(eff->GetFormID());
        if (it == g_effectRecipe.end()) {
            return ic;  // no known recipe — unpriceable in ingredient mode
        }
        rec = it->second;
    }
    if (!rec.a.form || !rec.b.form) {
        return ic;
    }
    // SAME curve as the essence economy (marth): ingredient mode scales off the
    // same cross-effect quality, not the retired per-effect concentration — the
    // two economies must stay mirror images.
    const float quality = VariantQuality(a_alch);
    // Mirror of VariantCost's multi-effect rule: only secondaries matching the
    // primary's polarity add a pair — drawbacks (Requiem side effects) are free.
    const bool    primaryHostile = eff->IsHostile() || eff->IsDetrimental();
    std::uint32_t extra          = 0;
    for (std::size_t i = 1; i < a_alch->effects.size(); ++i) {
        auto* e2 = a_alch->effects[i];
        if (e2 && e2->baseEffect &&
            ((e2->baseEffect->IsHostile() || e2->baseEffect->IsDetrimental()) ==
             primaryHostile)) {
            ++extra;
        }
    }
    ic.a     = rec.a.form;
    ic.b     = rec.b.form;
    ic.pairs =
        1u + (quality > g_catalystQuality ? 1u : 0u) + (quality > g_apexQuality ? 1u : 0u) + extra;
    return ic;
}

// Per-form needs for a_charges charges. A single-source recipe (a == b) needs
// DOUBLE of the one form — a pair is still two ingredients.
void IngrNeeds(const IngrCharge& ic, std::uint32_t a_charges, RE::FormID& a_formA,
               std::uint32_t& a_needA, RE::FormID& a_formB, std::uint32_t& a_needB) {
    a_formA               = ic.a;
    a_formB               = ic.b;
    const std::uint32_t n = ic.pairs * a_charges;
    if (ic.a == ic.b) {
        a_needA = 2 * n;
        a_formB = 0;
        a_needB = 0;
    } else {
        a_needA = n;
        a_needB = n;
    }
}

std::uint32_t HeldCount(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_obj) {
    if (!a_ref || !a_obj) {
        return 0;
    }
    std::uint32_t total = 0;
    for (const auto& [obj, cnt] : a_ref->GetInventoryCounts(
             [&](RE::TESBoundObject& o) { return &o == a_obj; })) {
        if (cnt > 0) {
            total += static_cast<std::uint32_t>(cnt);
        }
    }
    return total;
}

// Render-readable snapshot of the player's ingredient counts — the menu must
// not walk the inventory from the render thread. Refreshed on the task thread:
// field-kit open, after every ingredient spend, and on ingredient pickups
// while conversion is OFF.
std::mutex                                    g_heldLock;
std::unordered_map<RE::FormID, std::uint32_t> g_heldIngr;

void RefreshHeldIngredients() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }
    std::unordered_map<RE::FormID, std::uint32_t> fresh;
    for (const auto& [obj, cnt] : player->GetInventoryCounts(
             [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Ingredient); })) {
        if (obj && cnt > 0) {
            fresh[obj->GetFormID()] = static_cast<std::uint32_t>(cnt);
        }
    }
    std::scoped_lock lk(g_heldLock);
    g_heldIngr.swap(fresh);
}

std::uint32_t HeldSnapshot(RE::FormID a_form) {
    std::scoped_lock lk(g_heldLock);
    auto             it = g_heldIngr.find(a_form);
    return it == g_heldIngr.end() ? 0u : it->second;
}

// Render-side affordability from the snapshot (never touches the inventory).
bool CanAffordIngrSnapshot(const IngrCharge& ic, std::uint32_t a_charges) {
    if (!ic.pairs) {
        return false;
    }
    RE::FormID    fa, fb;
    std::uint32_t na, nb;
    IngrNeeds(ic, a_charges, fa, na, fb, nb);
    if (HeldSnapshot(fa) < na) {
        return false;
    }
    return !fb || HeldSnapshot(fb) >= nb;
}

// Menu cost text: "3x Wheat, 3x Blue Mountain Flower" for a_charges charges.
std::string IngrCostString(const IngrCharge& ic, std::uint32_t a_charges) {
    if (!ic.pairs) {
        return "(no recipe known)";
    }
    RE::FormID    fa, fb;
    std::uint32_t na, nb;
    IngrNeeds(ic, a_charges, fa, na, fb, nb);
    auto name = [](RE::FormID f) -> std::string {
        auto*       i = RE::TESForm::LookupByID<RE::IngredientItem>(f);
        const char* n = i ? i->GetName() : nullptr;
        return (n && *n) ? n : "?";
    };
    std::string s = std::format("{}x {}", na, name(fa));
    if (fb && nb) {
        s += std::format(", {}x {}", nb, name(fb));
    }
    return s;
}

// Task-thread spend against the REAL inventory. Consumes the recipe items and
// awards the essence-equivalent XP (parity with SpendCost's brewing XP).
bool SpendIngredients(RE::AlchemyItem* a_variant, std::uint32_t a_charges) {
    auto*            player = RE::PlayerCharacter::GetSingleton();
    const IngrCharge ic     = IngrCostPerCharge(a_variant);
    if (!player || !ic.pairs) {
        return false;
    }
    RE::FormID    fa, fb;
    std::uint32_t na, nb;
    IngrNeeds(ic, a_charges, fa, na, fb, nb);
    auto* ia = RE::TESForm::LookupByID<RE::IngredientItem>(fa);
    auto* ib = fb ? RE::TESForm::LookupByID<RE::IngredientItem>(fb) : nullptr;
    if (!ia || (fb && !ib)) {
        return false;
    }
    if (HeldCount(player, ia) < na || (ib && HeldCount(player, ib) < nb)) {
        return false;
    }
    player->RemoveItem(ia, static_cast<std::int32_t>(na), RE::ITEM_REMOVE_REASON::kRemove,
                       nullptr, nullptr);
    if (ib && nb) {
        player->RemoveItem(ib, static_cast<std::int32_t>(nb), RE::ITEM_REMOVE_REASON::kRemove,
                           nullptr, nullptr);
    }
    const FlaskCost eq = VariantCost(a_variant) * a_charges;
    AwardAlchemyXP(static_cast<float>(eq.base + eq.catalyst + eq.apex) * kAlchemyXpPerEssence);
    RefreshHeldIngredients();
    return true;
}

RE::FormID RepPotionFor(RE::FormID a_effect) {
    std::scoped_lock lk(g_effectPotionLock);
    auto it = g_effectPotion.find(a_effect);
    return it != g_effectPotion.end() ? it->second.first : 0;
}

// Build the effect -> strongest-potion table from the load order (potions AND
// poisons; poisons back the coating blueprints). Called at kDataLoaded.
void BuildEffectPotionTable() {
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!dh) {
        return;
    }
    // Prefer the CLEANEST embodiment: fewest total effects (so a Fortify Carry
    // Weight flask is a plain carry-weight potion, not a multi-effect modded
    // elixir that happens to lead with that effect), then strongest by value.
    struct Best { RE::FormID form = 0; std::size_t effects = 0; std::uint32_t value = 0; };
    std::unordered_map<RE::FormID, Best>  best;
    std::unordered_map<RE::FormID, float> minMag;  // weakest primary-effect magnitude = "standard"
    std::vector<std::uint32_t>            potionValues;  // for the global quality reference
    for (auto* alch : dh->GetFormArray<RE::AlchemyItem>()) {
        if (!alch || alch->IsFood() || alch->effects.empty() || !alch->effects[0] ||
            !alch->effects[0]->baseEffect) {
            continue;
        }
        if (const auto gv = alch->GetGoldValue(); gv > 0) {
            potionValues.push_back(static_cast<std::uint32_t>(gv));
        }
        const RE::FormID    e = alch->effects[0]->baseEffect->GetFormID();
        const std::size_t   n = alch->effects.size();
        const std::uint32_t v = static_cast<std::uint32_t>(std::max(0, alch->GetGoldValue()));
        auto&               b = best[e];
        if (b.form == 0 || n < b.effects || (n == b.effects && v > b.value)) {
            b = { alch->GetFormID(), n, v };
        }
        const float mg = alch->effects[0]->effectItem.magnitude;
        if (mg > 0.01f) {
            auto it = minMag.find(e);
            if (it == minMag.end() || mg < it->second) {
                minMag[e] = mg;
            }
        }
    }
    // Global quality reference = the MEDIAN potion gold value in this load
    // order. Quality is value/median, so a typical potion is ~1.0 and every
    // potion is comparable ACROSS effects (see VariantQuality).
    if (!potionValues.empty()) {
        std::sort(potionValues.begin(), potionValues.end());
        g_medianPotionValue =
            std::max(1.0f, static_cast<float>(potionValues[potionValues.size() / 2]));
    }
    std::scoped_lock lk(g_effectPotionLock);
    g_effectPotion.clear();
    g_effectMinMag = std::move(minMag);
    for (const auto& [e, b] : best) {
        g_effectPotion[e] = { b.form, b.value };
    }
    spdlog::info("[potions] rep table: {} effect(s); median potion value {} (quality reference)",
                 g_effectPotion.size(), g_medianPotionValue.load());
}

// Play a potion's drink sound at the player (the vanilla DrinkPotion we bypass
// would have done this).
void PlayDrinkSound(RE::Actor* a_player, RE::AlchemyItem* a_potion) {
    auto* audio = RE::BSAudioManager::GetSingleton();
    if (!a_player || !a_potion || !audio) {
        return;
    }
    RE::BSSoundHandle handle;
    bool              built = false;
    if (a_potion->data.consumptionSound) {
        built = audio->BuildSoundDataFromDescriptor(handle, a_potion->data.consumptionSound);
    }
    if (!built) {  // many vanilla potions leave it null — use the default drink sound
        audio->BuildSoundDataFromEditorID(handle, "ITMPotionUse", 0x1A);
    }
    if (auto* root = a_player->Get3D()) {
        handle.SetObjectToFollow(root);
    }
    handle.Play();
}

// Configure a flask slot with a blueprint (P1c). Runs on the task thread so it
// serializes with gathering/discovery. Deducts the essence cost up front and
// fills the flask to max charges; reassigning a flask that still holds charges
// purges them with no refund (the §3.1 overwrite penalty).
bool PlayerHasItem(RE::TESObjectREFR* a_ref, RE::TESBoundObject* a_obj) {
    if (!a_ref || !a_obj) {
        return false;
    }
    // Require a real positive count — GetInventoryCounts can retain a stale
    // 0-count entry for extra-data-bearing forms.
    for (const auto& [obj, cnt] : a_ref->GetInventoryCounts(
             [&](RE::TESBoundObject& o) { return &o == a_obj; })) {
        if (cnt > 0) {
            return true;
        }
    }
    return false;
}

// Configure a flask slot with a specific DISCOVERED potion variant (a_potion).
// a_field = opened from the power (limited kit): capped changes per trip, and
// the flask starts EMPTY with no upfront essence (refills over time).
void ConfigureFlask(std::size_t a_slot, RE::FormID a_potion, bool a_field = false) {
    if (a_slot >= g_flaskCount || !a_potion) {
        return;
    }
    {
        std::scoped_lock lk(g_discoveredLock);
        if (!g_discovered.contains(a_potion)) {
            return;  // must be a variant the player has actually found/bought
        }
    }
    // Resolve the variant's primary effect (drives cost, coating, refill).
    auto* alch = RE::TESForm::LookupByID<RE::AlchemyItem>(a_potion);
    if (!alch || alch->effects.empty() || !alch->effects[0] || !alch->effects[0]->baseEffect) {
        return;
    }
    const RE::FormID a_blueprint = alch->effects[0]->baseEffect->GetFormID();
    const RE::FormID rep         = a_potion;  // the flask IS this exact variant

    // Coatings (detrimental variants) are weapon treatments (P2), unlocked by
    // the Vanguard Coating perk. Without it, they can't be prepared.
    const bool isCoating = alch->effects[0]->baseEffect->IsHostile() ||
                           alch->effects[0]->baseEffect->IsDetrimental();
    if (kCoatingsRequireVanguard && isCoating && !g_hasVanguard.load()) {
        spdlog::info("[flask] slot {} DENIED — coating needs the Vanguard Coating perk", a_slot);
        if (g_notify.load()) {
            RE::DebugNotification("Coatings need the Vanguard Coating perk.");
        }
        return;
    }
    if (!g_flaskForms[a_slot]) {  // stale/missing MAO.esp — don't spend into nothing
        spdlog::error("[flask] slot {} has no flask form (MAO.esp stale or missing?) — aborted",
                      a_slot);
        if (g_notify) {
            RE::DebugNotification("Flask item missing — MAO.esp may be out of date.");
        }
        return;
    }
    const FlaskCost cost = VariantCost(alch) * g_chargesPerFlask;
    if (a_field) {  // limited kit: cap changes per trip; no upfront cost either mode
        if (g_fieldChanges.load() >= kFieldChangeLimit) {
            spdlog::info("[flask] slot {} field-configure DENIED — {} change(s)/trip used", a_slot,
                         kFieldChangeLimit);
            if (g_notify.load()) {
                RE::DebugNotification("Field kit: only one flask change per trip.");
            }
            return;
        }
    } else if (IngredientMode()) {  // conversion OFF: the fill costs recipe ingredients
        if (!SpendIngredients(alch, g_chargesPerFlask)) {
            const std::string need = IngrCostString(IngrCostPerCharge(alch), g_chargesPerFlask);
            spdlog::info("[flask] slot {} configure DENIED — need {}", a_slot, need);
            if (g_notify.load()) {
                RE::DebugNotification(
                    std::format("Not enough ingredients — need {}.", need).c_str());
            }
            return;
        }
    } else if (!SpendCost(cost)) {
        spdlog::info("[flask] slot {} configure DENIED — need {} (have B={} C={} A={})", a_slot,
                     CostString(cost), g_pouch.base.load(), g_pouch.catalyst.load(), g_pouch.apex.load());
        if (g_notify.load()) {
            RE::DebugNotification(std::format("Not enough essence — need {}.", CostString(cost)).c_str());
        }
        return;
    }
    const std::uint32_t startCharges = a_field ? 0u : g_chargesPerFlask;
    std::uint32_t       hadCharges   = 0;
    {
        std::scoped_lock lk(g_flasksLock);
        hadCharges       = g_flasks[a_slot].charges;
        g_flasks[a_slot] = { a_blueprint, rep, startCharges };  // rep = variant source
    }
    if (a_field) {
        g_fieldChanges.fetch_add(1);
    }

    // The physical item is the SLOT'S dedicated flask form — stable across
    // reconfigure, so its Favorites/Wheeler binding never breaks. We only
    // rename it to the new variant and make sure the player has it.
    RenameFlask(a_slot, alch);
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (player && g_flaskForms[a_slot] && !PlayerHasItem(player, g_flaskForms[a_slot])) {
        player->AddObjectToContainer(g_flaskForms[a_slot], nullptr, 1, nullptr);
    }

    const char* vName = alch->GetName();
    spdlog::info("[flask] slot {} <- '{}' (variant {:08X}, {} charges); {}; (was {} charges); "
                 "pouch B={} C={} A={}",
                 a_slot, vName ? vName : "?", rep, startCharges,
                 a_field ? std::string("field (empty, no cost)")
                         : ("spent " + (IngredientMode()
                                            ? IngrCostString(IngrCostPerCharge(alch),
                                                             g_chargesPerFlask)
                                            : CostString(cost))),
                 hadCharges, g_pouch.base.load(), g_pouch.catalyst.load(), g_pouch.apex.load());
    if (g_notify.load()) {
        RE::DebugNotification(
            std::format("Flask {} set: {}{}", a_slot + 1, vName ? vName : "potion",
                        a_field ? " (filling)" : "")
                .c_str());
    }
}

// On load, make sure each configured slot's flask item is actually in the
// player's inventory (the item persists in the save, but re-grant defensively —
// e.g. a mod removed it, or a pre-P1d save).
void SyncFlaskItems() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }
    // Form state is GLOBAL for the session (not reset per load), so re-apply
    // each configured slot's name AND reset unconfigured ones, and make sure
    // each configured slot's dedicated flask item is in the player's bags.
    // Snapshot (slot, variant) UNDER the lock, then act OUTSIDE it — grants fire
    // TESContainerChangedEvent whose sink re-locks g_flasksLock (non-recursive).
    struct Sync { std::size_t slot; RE::FormID variant; };
    std::vector<Sync> todo;
    {
        std::scoped_lock lk(g_flasksLock);
        for (std::uint32_t i = 0; i < g_flaskCount; ++i) {
            // Heal pre-P1d slots (blueprint set, no variant) from the load order.
            if (g_flasks[i].blueprint && g_flasks[i].repPotion == 0) {
                g_flasks[i].repPotion = RepPotionFor(g_flasks[i].blueprint);
            }
            if (g_flasks[i].repPotion) {
                todo.push_back({ i, g_flasks[i].repPotion });
            }
        }
    }
    std::array<bool, 6> synced{};
    for (const auto& t : todo) {
        synced[t.slot] = true;
        auto* vAlch    = RE::TESForm::LookupByID<RE::AlchemyItem>(t.variant);
        if (vAlch) {
            RenameFlask(t.slot, vAlch);
        }
        auto* item = g_flaskForms[t.slot];
        if (item && !PlayerHasItem(player, item)) {
            player->AddObjectToContainer(item, nullptr, 1, nullptr);
        }
    }
    // Reset the name AND displayed effect of any flask form NOT configured this
    // save (global form state could carry a stale "Flask: X" name + a mirrored
    // variant effect from another save this session — Fable v0.22.2: a stale
    // COATING effect would even leave an unconfigured flask reading as a poison).
    for (std::size_t i = 0; i < g_flaskForms.size(); ++i) {
        if (!synced[i] && g_flaskForms[i]) {
            g_flaskForms[i]->fullName = RE::BSFixedString("Field Flask");
            RestoreFlaskPlaceholder(i);
        }
    }
}

// ── Refill (P1d-3): dry/partial flasks top back up from essence, on sleep and
// on a real-time timer. Refilling costs essence per charge (a maintenance cost;
// perks cut it in P1e). Charges are native, so refill never touches the
// inventory item. Runs on the task thread.
// a_maxPerFlask: cap on charges restored per flask this pass (0 = fill to the
// brim). The TIMER trickles 1 charge/flask per tick (marth: "field restores
// are only supposed to be 1 every 300 seconds" — the old fill-to-cap predated
// ingredient mode making it visible); SLEEP stays the §3.2 full kit check.
void RefillFlasks(const char* a_trigger, std::uint32_t a_maxPerFlask = 0) {
    if (!RE::PlayerCharacter::GetSingleton()) {
        return;  // no game loaded (timer fires at the main menu too)
    }
    std::uint32_t refilled = 0, slots = 0, starved = 0;
    {
        std::scoped_lock lk(g_flasksLock);
        for (std::uint32_t i = 0; i < g_flaskCount; ++i) {
            auto& f = g_flasks[i];
            if (!f.blueprint || f.repPotion == 0 || f.charges >= g_chargesPerFlask) {
                continue;
            }
            auto*         valch = RE::TESForm::LookupByID<RE::AlchemyItem>(f.repPotion);
            FlaskCost     fc    = valch ? VariantCost(valch) : FlaskCost{};
            std::uint32_t added = 0;
            if (!valch) {
                continue;  // unresolved variant — inert flask, nothing to refill
            }
            // A coating flask the player can't use (gate armed, no Vanguard —
            // e.g. configured during the v0.18.1-0.21.1 relaxation) must not
            // drain essence/ingredients into charges the drink hook refuses
            // (Fable v0.22.0 finding 2).
            if (kCoatingsRequireVanguard && !g_hasVanguard.load()) {
                if (auto* bpEff = RE::TESForm::LookupByID<RE::EffectSetting>(f.blueprint);
                    bpEff && (bpEff->IsHostile() || bpEff->IsDetrimental())) {
                    continue;
                }
            }
            // Kit Calibration III: T1/T2 essences 15% more efficient in automated
            // refills. Kit Calibration V: T1/T2 maintenance halved during resting
            // checkouts (sleep). Multiplicative when both; Apex untouched (the
            // perks are explicitly Tier I/II). Essence mode only — the perk text
            // prices essences, and ingredient pairs are integral.
            if (!IngredientMode()) {
                float mult = 1.0f;
                if (g_hasCal3.load()) {
                    mult *= 0.85f;
                }
                if (g_hasCal5.load() && std::strcmp(a_trigger, "sleep") == 0) {
                    mult *= 0.5f;
                }
                if (mult < 1.0f) {
                    if (fc.base) {
                        fc.base = std::max(
                            1u, static_cast<std::uint32_t>(std::lround(fc.base * mult)));
                    }
                    if (fc.catalyst) {
                        fc.catalyst = std::max(
                            1u, static_cast<std::uint32_t>(std::lround(fc.catalyst * mult)));
                    }
                }
            }
            if (IngredientMode()) {
                // BATCH the whole flask (Fable 3f73d4b finding 2): compute how
                // many charges the held ingredients afford, spend them in ONE
                // SpendIngredients call — one RemoveItem per form and one
                // snapshot refresh per flask instead of ~4 inventory walks and
                // 2 event dispatches per charge, all under g_flasksLock.
                const IngrCharge ic = IngrCostPerCharge(valch);
                if (!ic.pairs) {
                    spdlog::info("[refill] {}: slot unpriceable in ingredient mode "
                                 "(no recipe for its effect) — skipped",
                                 a_trigger);
                    continue;
                }
                RE::FormID    fa, fb;
                std::uint32_t na, nb;
                IngrNeeds(ic, 1, fa, na, fb, nb);  // per-charge needs
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* ia     = RE::TESForm::LookupByID<RE::IngredientItem>(fa);
                auto* ib     = fb ? RE::TESForm::LookupByID<RE::IngredientItem>(fb) : nullptr;
                if (!player || !ia || (fb && !ib)) {
                    continue;
                }
                std::uint32_t can = na ? HeldCount(player, ia) / na : 0u;
                if (ib && nb) {
                    can = std::min(can, HeldCount(player, ib) / nb);
                }
                can = std::min(can, g_chargesPerFlask - f.charges);
                if (a_maxPerFlask) {
                    can = std::min(can, a_maxPerFlask);
                }
                if (can && SpendIngredients(valch, can)) {
                    f.charges += can;
                    added = can;
                }
            } else {
                // one charge's worth of essence per iteration
                while (f.charges < g_chargesPerFlask &&
                       (!a_maxPerFlask || added < a_maxPerFlask) && SpendCost(fc)) {
                    ++f.charges;
                    ++added;
                }
            }
            if (added) {
                refilled += added;
                ++slots;
            } else if (f.charges < g_chargesPerFlask) {
                // STARVED: the flask is below cap and the pouch can't afford a
                // single charge. Say so — silently never refilling looks like a
                // bug, and at the top of the quality curve it is the intended
                // Scarcity Guardrail rather than a fault (Fable v1.0.1 S6).
                // Ingredient mode needs no such line: the kit already lists the
                // exact items and greys what you can't afford.
                ++starved;
                if (!IngredientMode()) {
                    const char* vn = valch->GetName();
                    spdlog::info("[refill] {}: '{}' short of {} — {}/{} charges, "
                                 "pouch B={} C={} A={}",
                                 a_trigger, vn ? vn : "?", CostString(fc), f.charges,
                                 g_chargesPerFlask, g_pouch.base.load(),
                                 g_pouch.catalyst.load(), g_pouch.apex.load());
                }
            }
        }
    }
    if (refilled) {
        spdlog::info("[refill] {}: +{} charge(s) across {} flask(s){}; pouch B={}", a_trigger,
                     refilled, slots,
                     starved ? std::format(", {} flask(s) short", starved) : std::string{},
                     g_pouch.base.load());
        if (g_notify) {
            RE::DebugNotification(std::format("Field Kit replenished (+{} charges).", refilled).c_str());
        }
    } else if (starved) {
        spdlog::info("[refill] {}: nothing refilled — {} flask(s) short of essence", a_trigger,
                     starved);
    }
}

class SleepSink : public RE::BSTEventSink<RE::TESSleepStopEvent> {
public:
    static SleepSink* GetSingleton() {
        static SleepSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStopEvent*,
                                          RE::BSTEventSource<RE::TESSleepStopEvent>*) override {
        SKSE::GetTaskInterface()->AddTask([]() { RefillFlasks("sleep"); });
        return RE::BSEventNotifyControl::kContinue;
    }
};

// ── Coating duration (P2, DESIGN §5.2). A coating is TIME-based: 45s baseline
// (Vanguard Coating), 120s with Corrosive Retention. The hit count on the
// ExtraPoison is a big budget so time, not hits, is the limit; the 1s timer
// tick below strips OUR poison when the window closes. State is runtime-only:
// stale coatings surviving a save/load are stripped at kPostLoadGame instead.
constexpr std::uint32_t kCoatingSeconds          = 45;   // Vanguard baseline
constexpr std::uint32_t kCoatingSecondsCorrosive = 120;  // Corrosive Retention
constexpr std::int32_t  kCoatingHitBudget        = 999;  // never the limiting factor
// Coating clock. GUARDED BY g_coatingLock, not lock-free: the tick thread's
// check-then-clear vs a same-instant re-coat is a genuine check-then-act race
// (Fable review of 8a548d7, finding 1 — the tick could wipe a fresh coating's
// clock or schedule a strip against the NEW variant). These paths are cold
// (1 Hz tick + explicit player actions); a mutex is airtight where the atomics
// were not.
std::mutex    g_coatingLock;
std::uint64_t g_coatingExpiryMs = 0;  // steady-clock ms; 0 = no live coating
RE::FormID    g_coatingPoison   = 0;  // the variant WE applied (never strip others)

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Harvest source-tag: TESHarvestedEvent fires around the container add for the
// same base form; the gather credit checks the tag to know the ingredient came
// from FLORA, not loot — Pouch Expansion procs on harvests only (DESIGN §5.2
// "harvesting wild flora"). This is exactly what the tag-only HarvestSink was
// built to prove out.
std::atomic<RE::FormID>    g_lastHarvestForm{ 0 };
std::atomic<std::uint64_t> g_lastHarvestMs{ 0 };

bool RollPercent(std::uint32_t a_pct) {
    static thread_local std::mt19937 rng{ std::random_device{}() };
    return std::uniform_int_distribution<std::uint32_t>(0, 99)(rng) < a_pct;
}

// Strip an ExtraPoison from every WEAPON in the player's inventory whose
// poison satisfies a_shouldStrip. The whole inventory, not just equipped
// hands: unequipping a coated weapon must not bank the hit budget past the
// window (Fable review of 8a548d7, finding 2 — a stowed 999-hit coating was a
// permanent free poison). Equipped weapons are inventory entries too, so one
// sweep covers both. Task thread.
template <class F>
int StripCoatings(RE::PlayerCharacter* a_player, F&& a_shouldStrip) {
    int stripped = 0;
    for (auto& [obj, data] : a_player->GetInventory()) {
        if (!obj || !obj->Is(RE::FormType::Weapon)) {
            continue;
        }
        auto& entry = data.second;
        if (!entry || !entry->extraLists) {
            continue;
        }
        for (auto* xList : *entry->extraLists) {
            if (!xList) {
                continue;
            }
            auto* xp = xList->GetByType<RE::ExtraPoison>();
            if (xp && xp->poison && a_shouldStrip(xp->poison)) {
                xList->RemoveByType(RE::ExtraDataType::kPoison);
                ++stripped;
            }
        }
    }
    return stripped;
}

// Strip MAO's coating once the window closes. Task thread. a_form is the
// variant captured UNDER THE LOCK when the expiry was consumed — only that
// exact poison form is removed; a manually-applied poison is not ours to touch.
void ExpireCoating(RE::FormID a_form) {
    if (!a_form) {
        return;
    }
    {
        // A re-coat between the tick and this task re-arms the clock; if it
        // re-armed for the SAME variant, this strip would eat the fresh
        // coating — the live clock owns it now, stand down.
        std::scoped_lock lk(g_coatingLock);
        if (g_coatingExpiryMs != 0 && g_coatingPoison == a_form) {
            return;
        }
    }
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }
    const int stripped =
        StripCoatings(player, [&](RE::AlchemyItem* p) { return p->GetFormID() == a_form; });
    spdlog::info("[coat] coating window closed — stripped {} weapon(s)", stripped);
    if (stripped && g_notify.load()) {
        RE::DebugNotification("Weapon coating has worn off.");
    }
}

// A save can carry a coating whose window died with the process (the expiry is
// deliberately runtime-only, not co-saved). At load, strip any equipped
// ExtraPoison whose poison is a DISCOVERED detrimental variant — only MAO's
// coating path puts those on a weapon (loose poisons convert on pickup), so a
// reload can never hand back a time-based coating with the clock erased.
void StripStaleCoatings() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        return;
    }
    // Whole-inventory sweep (not just hands): a coated weapon stowed before
    // its window closed still carries the hit budget. Known soft spot: a
    // pre-install stock poison the player applies manually shares its base
    // FormID with a later-discovered variant and gets eaten here — accepted
    // (conversion makes loose poisons rare; see Fable 8a548d7 finding 3).
    const int stripped = StripCoatings(player, [](RE::AlchemyItem* p) {
        {
            std::scoped_lock lk(g_discoveredLock);
            if (!g_discovered.contains(p->GetFormID())) {
                return false;  // a manual vanilla poison — not ours to touch
            }
        }
        const auto* eff = !p->effects.empty() && p->effects[0] ? p->effects[0]->baseEffect
                                                               : nullptr;
        return eff && (eff->IsHostile() || eff->IsDetrimental());
    });
    if (stripped) {
        spdlog::info("[coat] load: stripped {} stale coating(s) — expiry is runtime-only",
                     stripped);
    }
}

void StartRefillTimer() {
    static std::atomic<bool> started{ false };
    if (started.exchange(true)) {
        return;  // once
    }
    // 1-second tick: coating expiry needs fine granularity; refills keep their
    // g_refillSeconds cadence by accumulating ticks (behavior unchanged).
    std::thread([]() {
        std::uint32_t acc = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            // Consume the expiry and capture WHICH variant it belonged to in
            // one critical section — a same-instant re-coat either finishes
            // before (new clock survives untouched) or after (it re-arms the
            // clock; this strip targets only the old form).
            RE::FormID expired = 0;
            {
                std::scoped_lock lk(g_coatingLock);
                if (g_coatingExpiryMs && NowMs() >= g_coatingExpiryMs) {
                    expired           = g_coatingPoison;
                    g_coatingExpiryMs = 0;
                    g_coatingPoison   = 0;
                }
            }
            if (expired) {
                SKSE::GetTaskInterface()->AddTask([expired]() { ExpireCoating(expired); });
            }
            const std::uint32_t secs = g_refillSeconds.load() ? g_refillSeconds.load() : 300u;
            if (++acc >= secs) {
                acc = 0;
                SKSE::GetTaskInterface()->AddTask([]() { RefillFlasks("timer", 1); });
            }
        }
    }).detach();
}

// ── Coating application (P2). A coating flask (detrimental variant) isn't drunk
// — it's applied to the equipped weapon(s) as a poison, exactly like vanilla
// poison. Sets an ExtraPoison on each equipped weapon's worn extra list,
// replacing any existing poison. Returns how many weapons were coated. Count is
// a per-application hit budget for now; the time-based 45s/120s durations (per
// the Vanguard/Corrosive perks) land in the next slice.
int ApplyCoating(RE::Actor* a_actor, RE::AlchemyItem* a_poison, std::int32_t a_count) {
    if (!a_actor || !a_poison) {
        return 0;
    }
    int coated = 0;
    for (bool left : { false, true }) {
        auto* entry = a_actor->GetEquippedEntryData(left);
        if (!entry || !entry->object || !entry->object->Is(RE::FormType::Weapon)) {
            continue;
        }
        if (!entry->extraLists || entry->extraLists->empty()) {
            continue;  // no worn instance to hold the poison (rare for equipped)
        }
        auto* xList = entry->extraLists->front();
        if (!xList) {
            continue;
        }
        xList->RemoveByType(RE::ExtraDataType::kPoison);  // one coating at a time
        xList->Add(new RE::ExtraPoison(a_poison, a_count));
        ++coated;
    }
    return coated;
}

// ── Consume intercept (P1d): the flask is a PERMANENT item — it must never be
// removed (so it stays favorited / on Wheeler). Hooking Actor::DrinkPotion
// (the funnel all drink paths reach, incl. potion mods) lets us apply the
// payload and decrement a native charge WITHOUT the vanilla removal. Player-
// only: we hook the PlayerCharacter vtable.
struct DrinkPotionHook {
    static bool thunk(RE::Actor* a_this, RE::AlchemyItem* a_potion, RE::ExtraDataList* a_extra) {
        if (a_this && a_this->IsPlayerRef() && a_potion) {
            const RE::FormID form = a_potion->GetFormID();
            const int        slot = FindFlaskSlot(form);
            if (slot >= 0) {
                RE::FormID bpMgef  = 0;
                RE::FormID variant = 0;
                {
                    std::scoped_lock lk(g_flasksLock);
                    // Re-verify configured under lock (could have been cleared).
                    if (g_flasks[slot].repPotion != 0) {
                        variant = g_flasks[slot].repPotion;
                        bpMgef  = g_flasks[slot].blueprint;
                    }
                }
                if (!variant) {
                    return true;  // reconfigured mid-call; leave the item intact
                }
                // A coating (detrimental effect) isn't drunk — it applies to the
                // equipped weapon(s) as a poison (P2), gated behind Vanguard.
                bool coating = false;
                if (auto* eff = RE::TESForm::LookupByID<RE::EffectSetting>(bpMgef)) {
                    coating = eff->IsHostile() || eff->IsDetrimental();
                }
                // Resolve the VARIANT (the flask form carries only a placeholder
                // effect) BEFORE spending a charge — if it can't resolve, the
                // flask is inert; don't burn a charge and don't consume it.
                auto* vAlch = RE::TESForm::LookupByID<RE::AlchemyItem>(variant);
                if (!vAlch) {
                    spdlog::warn("[drink] flask slot {} variant {:08X} unresolved — inert", slot,
                                 variant);
                    return true;
                }
                if (kCoatingsRequireVanguard && coating && !g_hasVanguard.load()) {
                    spdlog::info("[drink] flask slot {} is a coating — Vanguard perk not held", slot);
                    if (g_notify.load()) {
                        RE::DebugNotification("Coatings need the Vanguard Coating perk.");
                    }
                    return true;
                }
                int remaining = -1;
                {
                    std::scoped_lock lk(g_flasksLock);
                    if (g_flasks[slot].repPotion == variant && g_flasks[slot].charges > 0) {
                        g_flasks[slot].charges--;
                        remaining = static_cast<int>(g_flasks[slot].charges);
                    }
                }
                if (remaining >= 0 && coating) {
                    // Coating: apply to the equipped weapon(s) instead of drinking.
                    // TIME-based (DESIGN §5.2): 45s baseline, 120s with Corrosive
                    // Retention; the hit count is a budget so time is the limit.
                    const int coated = ApplyCoating(a_this, vAlch, kCoatingHitBudget);
                    if (coated == 0) {  // nothing to coat — refund the charge
                        std::scoped_lock lk(g_flasksLock);
                        if (g_flasks[slot].repPotion == variant) {
                            g_flasks[slot].charges++;
                        }
                        spdlog::info("[coat] flask slot {} — no weapon equipped; charge refunded",
                                     slot);
                        if (g_notify.load()) {
                            RE::DebugNotification("Equip a weapon to coat it.");
                        }
                        return true;
                    }
                    PlayDrinkSound(a_this, vAlch);
                    const std::uint32_t dur =
                        g_hasCorrosive.load() ? kCoatingSecondsCorrosive : kCoatingSeconds;
                    {
                        std::scoped_lock lk(g_coatingLock);
                        g_coatingPoison   = variant;
                        g_coatingExpiryMs = NowMs() + dur * 1000ull;
                    }
                    spdlog::info("[coat] flask slot {} -> {} weapon(s) for {}s{}; {} charge(s) left",
                                 slot, coated, dur, g_hasCorrosive.load() ? " (Corrosive)" : "",
                                 remaining);
                    if (g_notify.load()) {
                        RE::DebugNotification(
                            std::format("Weapon coated ({} seconds).", dur).c_str());
                    }
                } else if (remaining >= 0) {
                    // Extended Synthesis: +30s on drinkable buff durations. The
                    // variant form is SHARED (looted copies drink the same
                    // record), so bump-cast-restore within this hook: the
                    // ActiveEffect captures the extended duration at apply time
                    // and the form never stays mutated.
                    std::vector<std::pair<RE::Effect*, std::uint32_t>> bumped;
                    if (g_hasExtSynth.load()) {
                        for (auto* e : vAlch->effects) {
                            // BUFFS only: a hostile rider (Requiem's Damage-Regen
                            // drawbacks) must not run longer too (Fable v0.22.0
                            // finding 1 — mirrors VariantCost's polarity rule).
                            if (e && e->effectItem.duration > 0 && e->baseEffect &&
                                !e->baseEffect->IsHostile() && !e->baseEffect->IsDetrimental()) {
                                bumped.emplace_back(
                                    e, static_cast<std::uint32_t>(e->effectItem.duration));
                                e->effectItem.duration += 30;
                            }
                        }
                    }
                    if (auto* caster =
                            a_this->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
                        caster->CastSpellImmediate(vAlch, false, a_this, 1.0f, false, 0.0f, a_this);
                    }
                    for (auto& [e, d] : bumped) {
                        e->effectItem.duration = static_cast<std::int32_t>(d);
                    }
                    PlayDrinkSound(a_this, vAlch);
                    spdlog::info("[drink] flask slot {} drunk{}; {} charge(s) left", slot,
                                 bumped.empty() ? "" : " (+30s Extended Synthesis)", remaining);
                } else {
                    spdlog::info("[drink] flask slot {} is dry", slot);
                    if (g_notify) {
                        RE::DebugNotification("That flask is dry.");
                    }
                }
                return true;  // handled — item is never consumed (permanent flask)
            }
            // A flask item whose slot isn't live (unconfigured, or its variant's
            // plugin was removed) must STILL never be vanilla-consumed, or the
            // permanent item (+ its Favorites/Wheeler binding) is destroyed.
            if (IsFlaskForm(form)) {
                spdlog::info("[drink] inert flask ({:08X}) — not prepared; not consumed", form);
                if (g_notify) {
                    RE::DebugNotification("This flask isn't prepared.");
                }
                return true;
            }
        }
        return func(a_this, a_potion, a_extra);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

void InstallDrinkHook() {
    // 0x10F is the SSE/AE Actor::DrinkPotion vtable index; VR shifts the Actor
    // vtable, so bail there rather than patch the wrong vfunc.
    if (REL::Module::IsVR()) {
        spdlog::warn("[drink] VR runtime — DrinkPotion vtable index differs; hook skipped");
        return;
    }
    REL::Relocation<std::uintptr_t> vtbl{ RE::PlayerCharacter::VTABLE[0] };
    DrinkPotionHook::func = vtbl.write_vfunc(0x10F, DrinkPotionHook::thunk);
    spdlog::info("[drink] DrinkPotion vfunc hook installed (PlayerCharacter idx 0x10F)");
}

void SetupLog() {
    auto logDir = SKSE::log::log_directory();
    if (!logDir) {
        SKSE::stl::report_and_fail("MAO: unable to resolve the SKSE log directory");
    }
    auto logPath = *logDir / "MAO.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
    auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
}

void ApplyIniLine(std::string a_line) {
    // MCM-Helper INI format: strip a UTF-8 BOM, ignore [Section] headers and
    // comments, parse "key = value". The surface is here so P1's MCM has
    // somewhere to write.
    if (a_line.size() >= 3 && static_cast<unsigned char>(a_line[0]) == 0xEF) {
        a_line.erase(0, 3);
    }
    auto eq = a_line.find('=');
    if (eq == std::string::npos) {
        return;
    }
    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        auto e = s.find_last_not_of(" \t\r\n");
        if (e != std::string::npos) {
            s.erase(e + 1);
        }
        return s;
    };
    const std::string key = trim(a_line.substr(0, eq));
    const std::string val = trim(a_line.substr(eq + 1));
    if (key == "bConversionEnabled") {
        g_conversionEnabled = !(val == "0" || val == "false");
    } else if (key == "bNotify") {
        g_notify = !(val == "0" || val == "false");
    } else if (key == "iOpenHotkey") {
        g_openHotkey = static_cast<std::uint32_t>(std::strtoul(val.c_str(), nullptr, 0));
    } else if (key == "iOpenButtonGamepad") {
        g_openButtonGamepad = static_cast<std::uint32_t>(std::strtoul(val.c_str(), nullptr, 0));
    } else if (key == "iMenuStyle") {
        g_menuStyle = std::clamp(static_cast<int>(std::strtol(val.c_str(), nullptr, 0)), 0, 3);
    } else if (key == "fEssenceTax") {
        g_essenceTax = std::clamp(static_cast<float>(std::strtod(val.c_str(), nullptr)), 1.0f, 5.0f);
    } else if (key == "iRefillSeconds") {
        g_refillSeconds = static_cast<std::uint32_t>(
            std::clamp<long>(std::strtol(val.c_str(), nullptr, 0), 15, 86400));
    } else if (key == "fCostRate") {
        g_costRate = std::clamp(static_cast<float>(std::strtod(val.c_str(), nullptr)), 0.05f, 20.0f);
    } else if (key == "fCatalystLevel" || key == "fApexLevel") {
        // Retired in v1.1 (concentration -> quality rescale); the value is on a
        // different scale so it is deliberately ignored rather than applied.
        static bool warned = false;
        if (!warned) {
            warned = true;
            spdlog::warn("[config] '{}' is retired — v1.1 replaced the concentration thresholds "
                         "with fCatalystQuality/fApexQuality (different scale); ignoring it. "
                         "Re-set the new sliders in the MCM if you had tuned this.",
                         key);
        }
    } else if (key == "fCatalystQuality") {
        g_catalystQuality = std::clamp(static_cast<float>(std::strtod(val.c_str(), nullptr)), 0.5f, 8.0f);
    } else if (key == "fApexQuality") {
        g_apexQuality = std::clamp(static_cast<float>(std::strtod(val.c_str(), nullptr)), 0.5f, 8.0f);
    } else if (key == "bDebugPerks") {
        g_perkDebug.store(!(val == "0" || val == "false"));
    } else {
        // Per-perk debug override toggles (bPerk*). Set/clear the want-bit.
        const bool on = !(val == "0" || val == "false");
        for (int i = 0; i < PK_COUNT; ++i) {
            if (key == kPerkDefs[i].iniKey) {
                std::uint32_t m = g_perkWantMask.load();
                m = on ? (m | (1u << i)) : (m & ~(1u << i));
                g_perkWantMask.store(m);
                break;
            }
        }
    }
}

void ReadConfig() {
    // Reset the debug override each read so an MCM key that's absent (default)
    // reverts to off rather than sticking from a previous pass.
    g_perkDebug.store(false);
    g_perkWantMask.store(0);
    g_conversionEnabled.store(true);  // absent key = conversion on (default)
    g_notify.store(true);             // absent key = notifications on (default)
    for (const char* path : { "Data/SKSE/Plugins/MAO.ini", "Data/MCM/Settings/MAO.ini" }) {
        std::ifstream f(path);
        std::string   line;
        while (std::getline(f, line)) {
            ApplyIniLine(line);
        }
    }
    spdlog::info("[config] conversion={} bNotify={} iOpenHotkey=0x{:X} debugPerks={} wantMask=0x{:X}",
                 g_conversionEnabled.load(), g_notify.load(), g_openHotkey.load(), g_perkDebug.load(),
                 g_perkWantMask.load());
}

// Which flask slot (if any) uses this form as its physical item. -1 = none.
// Is this form one of our dedicated flask items (configured or not)? Used to
// keep the discovery sink from analysing a flask that's being (re)granted.
bool IsFlaskForm(RE::FormID a_form) {
    if (!a_form) {
        return false;
    }
    for (auto* f : g_flaskForms) {
        if (f && f->GetFormID() == a_form) {
            return true;
        }
    }
    return false;
}

// Which CONFIGURED slot's flask item is this form? (drink hook.)
int FindFlaskSlot(RE::FormID a_form) {
    if (!a_form) {
        return -1;
    }
    std::scoped_lock lk(g_flasksLock);
    for (std::uint32_t i = 0; i < g_flaskCount; ++i) {
        if (g_flasks[i].repPotion != 0 && g_flaskForms[i] &&
            g_flaskForms[i]->GetFormID() == a_form) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Rename a slot's flask item to its configured variant so the player can tell
// flasks apart in the inventory / on Wheeler. Runs on the main (task) thread.
// Restore a flask form's displayed effect to its baked benign placeholder
// (Restore Health 5/0). Keeps the flask NON-hostile so the engine never
// classifies it as a poison (which would reroute it off the DrinkPotion hook).
void RestoreFlaskPlaceholder(std::size_t a_slot) {
    if (a_slot >= g_flaskForms.size() || !g_flaskForms[a_slot] || !g_flaskPlaceholder[a_slot].ok) {
        return;
    }
    auto* flask = g_flaskForms[a_slot];
    if (!flask->effects.empty() && flask->effects[0]) {
        auto& ei         = flask->effects[0]->effectItem;
        flask->effects[0]->baseEffect = g_flaskPlaceholder[a_slot].eff;
        ei.magnitude     = g_flaskPlaceholder[a_slot].mag;
        ei.area          = g_flaskPlaceholder[a_slot].area;
        ei.duration      = g_flaskPlaceholder[a_slot].dur;
    }
}

void RenameFlask(std::size_t a_slot, RE::AlchemyItem* a_variant) {
    if (a_slot >= g_flaskForms.size() || !g_flaskForms[a_slot] || !a_variant) {
        return;
    }
    auto*       flask = g_flaskForms[a_slot];
    const char* vn    = a_variant->GetName();
    flask->fullName =
        RE::BSFixedString(std::format("Flask: {}", (vn && *vn) ? vn : "Alchemy").c_str());

    // Mirror the variant's PRIMARY effect onto the flask's displayed effect so
    // the item card reads the real magnitude/duration instead of the baked
    // placeholder (marth soak: "effects read as zero seconds"). The drink hook
    // casts the VARIANT, so the flask's own effect is never applied on the drink
    // path — BUT a HOSTILE mirrored effect makes the engine classify the flask
    // as a poison (IsPoison keys off the primary MGEF's Hostile+Detrimental
    // flags), which reroutes activation to the vanilla apply-poison flow,
    // bypassing the hook and consuming the permanent item (Fable v0.22.2). So
    // coatings KEEP the benign placeholder; only beneficial variants mirror.
    const bool hostile = !a_variant->effects.empty() && a_variant->effects[0] &&
                         a_variant->effects[0]->baseEffect &&
                         (a_variant->effects[0]->baseEffect->IsHostile() ||
                          a_variant->effects[0]->baseEffect->IsDetrimental());
    if (hostile) {
        RestoreFlaskPlaceholder(a_slot);  // coating: never let the flask read as a poison
    } else if (!flask->effects.empty() && flask->effects[0] && !a_variant->effects.empty() &&
               a_variant->effects[0] && a_variant->effects[0]->baseEffect) {
        flask->effects[0]->baseEffect = a_variant->effects[0]->baseEffect;
        flask->effects[0]->effectItem = a_variant->effects[0]->effectItem;
    }
}

// ── The gathering sink: the SOLE essence-credit point. Anything an ingredient
// enters the player's inventory through funnels here.
class ContainerSink : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
public:
    static ContainerSink* GetSingleton() {
        static ContainerSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
                                          RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
        if (!g_conversionEnabled.load()) {
            // OFF (ingredient mode): ingredients/potions stay as items. Two
            // side effects remain: an ingredient pickup freshens the menu's
            // affordability snapshot, and a potion pickup still LEARNS its
            // blueprint ("finding" — you studied it) without consuming it or
            // crediting essence. Discovery must not depend on the economy mode
            // or ingredient mode could never learn a new variant.
            if (a_event && a_event->baseObj && a_event->newContainer == kPlayerID &&
                a_event->itemCount > 0) {
                if (auto* f = RE::TESForm::LookupByID(a_event->baseObj)) {
                    if (f->Is(RE::FormType::Ingredient)) {
                        // Pouch Expansion, ingredient-mode analog (Fable v0.22.0
                        // finding 3): items are this mode's currency, so a flora
                        // harvest sifts out +1 EXTRA of the harvested ingredient
                        // (15%). Safe from echo-procs: the tag is consumed on
                        // match, so the granted copy's own event finds no tag.
                        RE::FormID expectTag = a_event->baseObj;
                        const bool fromFlora =
                            NowMs() - g_lastHarvestMs.load() < 2000 &&
                            g_lastHarvestForm.compare_exchange_strong(expectTag, 0);
                        if (fromFlora && g_hasPouchExp.load() && RollPercent(15)) {
                            auto* ingr2 = f->As<RE::IngredientItem>();
                            SKSE::GetTaskInterface()->AddTask([ingr2]() {
                                auto* player = RE::PlayerCharacter::GetSingleton();
                                if (!player || !ingr2) {
                                    return;
                                }
                                player->AddObjectToContainer(ingr2, nullptr, 1, nullptr);
                                const char* n = ingr2->GetName();
                                spdlog::info("[gather] Pouch Expansion (ingredient mode): +1 '{}'",
                                             n ? n : "?");
                                if (g_notify.load()) {
                                    RE::DebugNotification(
                                        std::format("+1 {} (Pouch Expansion)", n ? n : "ingredient")
                                            .c_str());
                                }
                                RefreshHeldIngredients();
                            });
                        } else {
                            SKSE::GetTaskInterface()->AddTask([]() { RefreshHeldIngredients(); });
                        }
                    } else if (auto* alch = f->As<RE::AlchemyItem>();
                               alch && !alch->IsFood() && !IsFlaskForm(a_event->baseObj) &&
                               !alch->effects.empty() && alch->effects[0] &&
                               alch->effects[0]->baseEffect) {
                        const RE::FormID potForm   = alch->GetFormID();
                        const bool       ephemeral = (potForm & 0xFF000000) == 0xFF000000;
                        if (!ephemeral) {
                            const char*       nm = alch->GetName();
                            const std::string potName(nm ? nm : "potion");
                            SKSE::GetTaskInterface()->AddTask([potForm, potName]() {
                                bool learned = false;
                                {
                                    std::scoped_lock lk(g_discoveredLock);
                                    learned = g_discovered.insert(potForm).second;
                                }
                                if (learned) {
                                    AwardAlchemyXP(kDiscoverXp);
                                    spdlog::info("[discover] '{}' studied (ingredient mode — "
                                                 "item kept, no essence)",
                                                 potName);
                                    if (g_notify.load()) {
                                        RE::DebugNotification(
                                            std::format("Discovered: {}", potName).c_str());
                                    }
                                }
                            });
                        }
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
        // Only credit items ENTERING the player. Removals (our own included)
        // have newContainer != player and are ignored — so RemoveItem below
        // can't re-trigger us (no loop).
        if (!a_event || !a_event->baseObj || a_event->newContainer != kPlayerID ||
            a_event->itemCount <= 0) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* form = RE::TESForm::LookupByID(a_event->baseObj);
        if (!form) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const int count = a_event->itemCount;

        // ── Ingredient → essence (M1 gathering) ──
        if (auto* ingr = form->As<RE::IngredientItem>()) {
            const char*      cname = ingr->GetName();
            std::string_view name  = cname ? cname : "";
            if (IsExcluded(name)) {
                spdlog::info("[gather] EXCLUDED '{}' ({:08X}) — kept as item, no conversion",
                             name, a_event->baseObj);
                return RE::BSEventNotifyControl::kContinue;
            }
            const Tier tier  = TierOfForm(ingr);
            const int  value = ingr->value;
            std::uint32_t total = YieldFor(value, tier) * static_cast<std::uint32_t>(count);
            if (g_hasExperimenter.load()) {  // Field Extraction: +10% gather yield
                total = std::max(1u, static_cast<std::uint32_t>(std::lround(total * 1.10)));
            }
            // Pouch Expansion procs on FLORA harvests only — the harvest tag
            // must match this form, be fresh (same-frame in practice; 2s is
            // generous), and is CONSUMED on match so one harvest pays out once
            // (Fable v0.22.0 finding 4: an unconsumed tag let a same-form loot
            // or vendor stack within 2s roll too).
            RE::FormID expectTag  = a_event->baseObj;
            const bool fromFlora = NowMs() - g_lastHarvestMs.load() < 2000 &&
                                   g_lastHarvestForm.compare_exchange_strong(expectTag, 0);
            const std::string nameStr(name);
            SKSE::GetTaskInterface()->AddTask([ingr, count, tier, total, value, nameStr,
                                               fromFlora]() {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
                    return;
                }
                // Never dissolve a quest-flagged item (checked live now that the
                // pickup is complete and the alias/flag is attached).
                if (IsQuestItem(player, ingr)) {
                    spdlog::info("[gather] QUEST ITEM '{}' — kept as item, no conversion", nameStr);
                    return;
                }
                player->RemoveItem(ingr, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                CreditPouch(tier, total);
                spdlog::info("[gather] +{} {} essence <- {}x '{}' (value {}); pouch B={} C={} A={}",
                             total, TierName(tier), count, nameStr, value,
                             g_pouch.base.load(), g_pouch.catalyst.load(), g_pouch.apex.load());
                if (g_notify) {
                    RE::DebugNotification(
                        std::format("+{} {} Essence ({})", total, TierName(tier), nameStr).c_str());
                }
                // Pouch Expansion (DESIGN §5.2): 15% chance a flora harvest also
                // yields a matching Tier II Catalyst essence.
                if (fromFlora && g_hasPouchExp.load() && RollPercent(15)) {
                    const std::uint32_t bonus = YieldFor(value, Tier::Catalyst);
                    CreditPouch(Tier::Catalyst, bonus);
                    spdlog::info("[gather] Pouch Expansion proc: +{} Catalyst ({})", bonus,
                                 nameStr);
                    if (g_notify.load()) {
                        RE::DebugNotification(
                            std::format("+{} Catalyst Essence (Pouch Expansion)", bonus).c_str());
                    }
                }
            });
            return RE::BSEventNotifyControl::kContinue;
        }

        // ── Potion → blueprint discovery (P1b) ──
        // Single-use potions/poisons are stripped and analyzed: the physical
        // item is destroyed, its primary effect becomes a permanent blueprint
        // unlock, and it yields essence. Food is NOT a potion — leave it. (P1c
        // adds a flask-item exclusion here once flasks exist.)
        if (auto* alch = form->As<RE::AlchemyItem>()) {
            if (alch->IsFood()) {
                return RE::BSEventNotifyControl::kContinue;
            }
            // A flask item landing in the player's bags (grant) must NOT be
            // analysed/destroyed.
            if (IsFlaskForm(a_event->baseObj)) {
                return RE::BSEventNotifyControl::kContinue;
            }
            // Quest potions (e.g. Vaermina's Torpor, which the player must DRINK)
            // must never be analysed. Same guard set as ingredients — name
            // backstop here, live quest-flag check in the task below.
            if (const char* pnm = alch->GetName(); pnm && IsExcluded(pnm)) {
                spdlog::info("[discover] EXCLUDED '{}' ({:08X}) — kept as item, no analysis",
                             pnm, a_event->baseObj);
                return RE::BSEventNotifyControl::kContinue;
            }
            // Primary effect → blueprint key. Beneficial effects become
            // drinkable flasks; hostile/detrimental ones become COATINGS (the
            // Poisoner→Vanguard Coating path, DESIGN §5.2) — both are learned
            // as blueprints here; the drink hook and P2 tell them apart.
            RE::FormID  bpKey   = 0;
            const char* bpCName = nullptr;
            if (!alch->effects.empty() && alch->effects[0] && alch->effects[0]->baseEffect) {
                bpKey   = alch->effects[0]->baseEffect->GetFormID();
                bpCName = alch->effects[0]->baseEffect->GetName();
            }
            // Tier-matched 1:1 (marth 2026-07-16, DESIGN §4.1): analyzing a
            // potion credits exactly ONE refill charge's worth of THAT variant
            // — VariantCost is the per-charge cost, in the variant's own tier
            // split. Finding or buying a potion always funds one refill of it.
            const RE::FormID    potForm = alch->GetFormID();
            const FlaskCost     fc      = VariantCost(alch) * static_cast<std::uint32_t>(count);
            const char*         pcName  = alch->GetName();
            const std::string   potName(pcName ? pcName : "potion");
            const std::string   bpName(bpCName ? bpCName : "");
            SKSE::GetTaskInterface()->AddTask([alch, count, bpKey, potForm, fc, potName,
                                               bpName]() {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
                    return;
                }
                // Never analyse a quest-flagged potion (delivery targets, or
                // potions the quest wants you to drink).
                if (IsQuestItem(player, alch)) {
                    spdlog::info("[discover] QUEST ITEM '{}' — kept as item, no analysis", potName);
                    return;
                }
                player->RemoveItem(alch, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                bool learned = false;
                // Skip runtime-created (0xFF) forms — a potion crafted at a
                // bench is ephemeral; persisting its FormID would dangle or be
                // reused by a different created form next session. Still convert
                // it to essence, just don't record it as a discoverable variant.
                const bool ephemeral = (potForm & 0xFF000000) == 0xFF000000;
                if (bpKey && !ephemeral) {
                    std::scoped_lock lk(g_discoveredLock);
                    learned = g_discovered.insert(potForm).second;  // this SPECIFIC variant
                }
                if (fc.base)     CreditPouch(Tier::Base, fc.base);
                if (fc.catalyst) CreditPouch(Tier::Catalyst, fc.catalyst);
                if (fc.apex)     CreditPouch(Tier::Apex, fc.apex);
                if (learned) {  // learning a new variant trains Alchemy (like eating an ingredient)
                    AwardAlchemyXP(kDiscoverXp);
                }
                spdlog::info("[discover] '{}' analyzed ({}); effect '{}'; +{} (1 refill charge); "
                             "pouch B={} C={} A={}",
                             potName, bpKey ? (learned ? "NEW variant" : "known") : "no-effect",
                             bpName.empty() ? "?" : bpName, CostString(fc), g_pouch.base.load(),
                             g_pouch.catalyst.load(), g_pouch.apex.load());
                if (g_notify) {
                    if (learned) {
                        RE::DebugNotification(std::format("Discovered: {}", potName).c_str());
                    }
                    RE::DebugNotification(
                        std::format("+{} Essence ({})", CostString(fc), potName).c_str());
                }
            });
            return RE::BSEventNotifyControl::kContinue;
        }

        return RE::BSEventNotifyControl::kContinue;
    }
};

// ── Harvest tag: fires alongside the container event on a flora/creature
// harvest. P0 only logs it (the credit already happened in ContainerSink);
// it exists to prove we can source-tag harvests for the perks that will care
// (Field Extraction +10%, Pouch Expansion Tier-II proc). NEVER credits.
class HarvestSink : public RE::BSTEventSink<RE::TESHarvestedEvent::ItemHarvested> {
public:
    static HarvestSink* GetSingleton() {
        static HarvestSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESHarvestedEvent::ItemHarvested* a_event,
                                          RE::BSTEventSource<RE::TESHarvestedEvent::ItemHarvested>*)
        override {
        if (a_event && a_event->harvester && a_event->harvester->IsPlayerRef() &&
            a_event->produceItem) {
            g_lastHarvestForm.store(a_event->produceItem->GetFormID());
            g_lastHarvestMs.store(NowMs());
            const char* n = a_event->produceItem->GetName();
            spdlog::info("[harvest] player harvested '{}' (tagged for Pouch Expansion; "
                         "credit via container sink)",
                         n ? n : "?");
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

// ── The Field Kit viewer (M2). Read-only: it renders the pouch, takes no
// input beyond the open/close toggle, so no mouse/keyboard is routed into
// ImGui. P1 turns this window into the flask-configuration UI and swaps the
// hotkey opener for the lesser power (DESIGN §3.3).
namespace menuhook {

    ID3D11Device*        g_device  = nullptr;
    ID3D11DeviceContext* g_context = nullptr;
    std::atomic<bool>    g_d3dReady{ false };
    float                g_bbW = 0.0f;
    float                g_bbH = 0.0f;

    // Render-thread-only UI state.
    int   g_selectedSlot = -1;
    float g_cursorX = -1.0f;
    float g_cursorY = -1.0f;

    // Real typefaces (baked at backbuffer scale in D3DInit; optional — the
    // draw falls back per-font). Ported from MEO along with the skins.
    ImFont* g_fontBody = nullptr;
    ImFont* g_fontHead = nullptr;
    ImFont* g_fontSans = nullptr;

    // ── Menu skins (ported from MEO; re-titled for MAO). Square corners, flat
    // fills — ImGui's honest range, closer to Skyrim's UI than its debug grey.
    // iMenuStyle picks one live.
    struct MenuSkin {
        const char* name;
        ImVec4      winBg, panel, border, text, dim, sel, accent, btn, track, danger;
        bool        sans;
        const char* title;
    };
    inline constexpr MenuSkin kSkins[4] = {
        { "Ebony & Brass",
          { 0.04f, 0.04f, 0.06f, 0.95f }, { 0.07f, 0.07f, 0.10f, 0.95f },
          { 0.55f, 0.48f, 0.27f, 0.60f }, { 0.91f, 0.89f, 0.84f, 1.00f },
          { 0.58f, 0.55f, 0.47f, 1.00f }, { 0.34f, 0.29f, 0.16f, 0.85f },
          { 0.78f, 0.70f, 0.45f, 1.00f }, { 0.13f, 0.11f, 0.07f, 0.90f },
          { 1.00f, 1.00f, 1.00f, 0.08f }, { 0.76f, 0.29f, 0.24f, 1.00f },
          false, "FIELD KIT" },
        { "Dwemer Parchment",
          { 0.92f, 0.88f, 0.80f, 0.97f }, { 0.95f, 0.92f, 0.85f, 1.00f },
          { 0.54f, 0.45f, 0.25f, 0.85f }, { 0.21f, 0.17f, 0.12f, 1.00f },
          { 0.48f, 0.43f, 0.34f, 1.00f }, { 0.86f, 0.81f, 0.66f, 1.00f },
          { 0.43f, 0.29f, 0.16f, 1.00f }, { 0.89f, 0.84f, 0.72f, 1.00f },
          { 0.00f, 0.00f, 0.00f, 0.10f }, { 0.55f, 0.23f, 0.18f, 1.00f },
          false, "FIELD KIT" },
        { "Soul Cairn",
          { 0.07f, 0.06f, 0.13f, 0.95f }, { 0.10f, 0.08f, 0.19f, 0.95f },
          { 0.35f, 0.31f, 0.55f, 0.70f }, { 0.85f, 0.84f, 0.92f, 1.00f },
          { 0.55f, 0.52f, 0.66f, 1.00f }, { 0.16f, 0.14f, 0.31f, 0.90f },
          { 0.53f, 0.85f, 0.92f, 1.00f }, { 0.13f, 0.10f, 0.23f, 0.90f },
          { 1.00f, 1.00f, 1.00f, 0.08f }, { 0.76f, 0.29f, 0.24f, 1.00f },
          false, "FIELD KIT" },
        { "Quicksilver",
          { 0.04f, 0.05f, 0.06f, 0.88f }, { 0.07f, 0.08f, 0.10f, 0.92f },
          { 0.22f, 0.25f, 0.29f, 1.00f }, { 0.83f, 0.85f, 0.88f, 1.00f },
          { 0.47f, 0.50f, 0.54f, 1.00f }, { 0.14f, 0.19f, 0.23f, 0.90f },
          { 0.56f, 0.72f, 0.80f, 1.00f }, { 0.09f, 0.11f, 0.13f, 0.90f },
          { 1.00f, 1.00f, 1.00f, 0.07f }, { 0.76f, 0.29f, 0.24f, 1.00f },
          true, "F I E L D   K I T" },
    };
    ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
        return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w };
    }
    int g_appliedSkin = -1;

    void ApplyMenuStyle(const MenuSkin& s) {
        auto& style             = ImGui::GetStyle();
        style.WindowRounding    = 0.0f;
        style.ChildRounding     = 0.0f;
        style.FrameRounding     = 0.0f;
        style.ScrollbarRounding = 0.0f;
        style.WindowBorderSize  = 1.0f;
        style.ChildBorderSize   = 1.0f;
        style.WindowPadding     = ImVec2(18.0f, 14.0f);
        style.ItemSpacing       = ImVec2(10.0f, 7.0f);
        style.FramePadding      = ImVec2(10.0f, 7.0f);
        style.ScrollbarSize     = 14.0f;
        style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
        auto* c                    = style.Colors;
        c[ImGuiCol_WindowBg]       = s.winBg;
        c[ImGuiCol_ChildBg]        = s.panel;
        c[ImGuiCol_PopupBg]        = s.panel;
        c[ImGuiCol_Border]         = s.border;
        c[ImGuiCol_Separator]      = s.border;
        c[ImGuiCol_Text]           = s.text;
        c[ImGuiCol_TextDisabled]   = s.dim;
        c[ImGuiCol_Header]         = s.sel;
        c[ImGuiCol_HeaderHovered]  = Mix(s.sel, s.accent, 0.25f);
        c[ImGuiCol_HeaderActive]   = Mix(s.sel, s.accent, 0.40f);
        c[ImGuiCol_Button]         = s.btn;
        c[ImGuiCol_ButtonHovered]  = Mix(s.btn, s.accent, 0.25f);
        c[ImGuiCol_ButtonActive]   = Mix(s.btn, s.accent, 0.40f);
        c[ImGuiCol_ScrollbarBg]    = s.track;
        c[ImGuiCol_ScrollbarGrab]  = ImVec4(s.dim.x, s.dim.y, s.dim.z, 0.60f);
        c[ImGuiCol_TitleBg]        = s.winBg;
        c[ImGuiCol_TitleBgActive]  = s.winBg;
        c[ImGuiCol_NavHighlight]   = s.accent;
    }

    // Keyboard/gamepad -> ImGui nav keys, so the menu drives from the pad (the
    // Steam Deck has no mouse). Ported from MEO's proven maps.
    ImGuiKey DIKToImGuiKey(std::uint32_t a_dik) {
        switch (a_dik) {
        case 0xC8: return ImGuiKey_UpArrow;
        case 0xD0: return ImGuiKey_DownArrow;
        case 0xCB: return ImGuiKey_LeftArrow;
        case 0xCD: return ImGuiKey_RightArrow;
        case 0x1C: return ImGuiKey_Enter;   // Return
        case 0x39: return ImGuiKey_Space;
        default:   return ImGuiKey_None;
        }
    }
    ImGuiKey GamepadToImGuiKey(std::uint32_t a_key) {
        using K = RE::BSWin32GamepadDevice::Key;
        switch (static_cast<K>(a_key)) {
        case K::kUp:    return ImGuiKey_GamepadDpadUp;
        case K::kDown:  return ImGuiKey_GamepadDpadDown;
        case K::kLeft:  return ImGuiKey_GamepadDpadLeft;
        case K::kRight: return ImGuiKey_GamepadDpadRight;
        case K::kA:     return ImGuiKey_GamepadFaceDown;   // activate
        default:        return ImGuiKey_None;
        }
    }

    // The interactive Field Kit: essence stores, flask slots, and known
    // blueprints. Click a flask, then a blueprint, to configure it (P1c).
    void DrawFieldKit() {
        auto& io           = ImGui::GetIO();
        io.MouseDrawCursor = true;
        const int       skinIdx = std::clamp(g_menuStyle.load(), 0, 3);
        const MenuSkin& skin    = kSkins[skinIdx];
        if (g_appliedSkin != skinIdx) {
            ApplyMenuStyle(skin);
            g_appliedSkin = skinIdx;
            spdlog::info("[menu] skin: {}", skin.name);
        }
        ImFont* fBody = skin.sans ? (g_fontSans ? g_fontSans : g_fontBody) : g_fontBody;
        ImFont* fHead = skin.sans ? fBody : (g_fontHead ? g_fontHead : g_fontBody);
        // Real typeface is baked at backbuffer scale; global scale is only the
        // fallback when no font file loaded.
        io.FontGlobalScale = fBody ? 1.0f : std::max(1.0f, io.DisplaySize.y / 1080.0f);
        if (fBody) {
            ImGui::PushFont(fBody);
        }
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.40f, io.DisplaySize.y * 0.62f),
                                 ImGuiCond_Appearing);
        if (!ImGui::Begin("MAO Field Kit", nullptr,
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoTitleBar)) {
            ImGui::End();
            if (fBody) {
                ImGui::PopFont();
            }
            return;
        }
        {  // centered title in the display face + accent color
            if (fHead) {
                ImGui::PushFont(fHead);
            }
            const float tw = ImGui::CalcTextSize(skin.title).x;
            ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowSize().x - tw) * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Text, skin.accent);
            ImGui::TextUnformatted(skin.title);
            ImGui::PopStyleColor();
            if (fHead) {
                ImGui::PopFont();
            }
        }
        ImGui::Separator();
        if (IngredientMode()) {
            ImGui::Text("INGREDIENT MODE — flasks consume recipe ingredients");
        } else {
            ImGui::Text("Essence   Base %u    Catalyst %u    Apex %u", g_pouch.base.load(),
                        g_pouch.catalyst.load(), g_pouch.apex.load());
        }
        ImGui::Spacing();

        // ── Flasks ──
        ImGui::TextDisabled("FLASKS  —  select one, then a variant below");
        ImGui::Separator();
        // Snapshot the perk-scaled charge cap under the lock — RecomputeCapacity
        // writes it from the task thread while this menu renders.
        std::uint32_t chargesCap = 2;
        {
            std::scoped_lock lk(g_flasksLock);
            chargesCap = g_chargesPerFlask;
            if (g_selectedSlot >= static_cast<int>(g_flaskCount)) {
                g_selectedSlot = -1;
            }
            for (int i = 0; i < static_cast<int>(g_flaskCount); ++i) {
                const auto& f    = g_flasks[i];
                const char* name = "(empty)";
                if (f.repPotion) {
                    if (auto* p = RE::TESForm::LookupByID<RE::AlchemyItem>(f.repPotion)) {
                        if (p->GetName() && *p->GetName()) {
                            name = p->GetName();
                        }
                    }
                }
                char label[160];
                std::snprintf(label, sizeof(label), "Flask %d:  %s    [%u/%u]##flask%d", i + 1,
                              name, f.charges, chargesCap, i);
                if (ImGui::Selectable(label, g_selectedSlot == i)) {
                    g_selectedSlot = i;
                }
            }
        }
        ImGui::Spacing();

        // ── Discovered variants (the specific potions you've found/bought) ──
        ImGui::TextDisabled("VARIANTS  —  cost fills all %u charges", chargesCap);
        ImGui::Separator();
        {
            std::scoped_lock lk(g_discoveredLock);
            const bool field = !g_menu.station.load();  // power opener = limited field kit
            if (field) {
                const int left = std::max(0, kFieldChangeLimit - g_fieldChanges.load());
                ImGui::TextDisabled("FIELD KIT — %d change(s) left; a field change starts empty",
                                    left);
            }
            if (g_discovered.empty()) {
                ImGui::TextDisabled("None yet — pick up potions to analyze them.");
            } else {
                if (g_selectedSlot < 0) {
                    ImGui::TextDisabled("Select a flask above first.");
                }
                ImGui::BeginChild("variants", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f));
                // Sort by TYPE with commonly-used categories up top (marth),
                // instead of g_discovered's hash order. Category from the
                // primary effect's name; within a category, cluster by effect
                // then potion name. Category headers make the grouping visible.
                struct VEntry {
                    RE::FormID         pid;
                    RE::AlchemyItem*   alch;
                    RE::EffectSetting* eff;
                    bool               coat;
                    int                cat;
                    const char*        effName;
                    const char*        potName;
                };
                static const char* kCatLabels[] = { "Restoratives", "Fortify",
                                                    "Resist / Cure", "Other", "Coatings" };
                std::vector<VEntry> vlist;
                vlist.reserve(g_discovered.size());
                for (const RE::FormID pid : g_discovered) {
                    auto* a = RE::TESForm::LookupByID<RE::AlchemyItem>(pid);
                    if (!a || a->effects.empty() || !a->effects[0] || !a->effects[0]->baseEffect) {
                        continue;
                    }
                    auto*                  e   = a->effects[0]->baseEffect;
                    const bool             c   = e->IsHostile() || e->IsDetrimental();
                    const char*            en  = e->GetName();
                    const std::string_view ev  = en ? en : "";
                    auto has = [&](const char* s) { return ev.find(s) != std::string_view::npos; };
                    int cat = 3;  // other beneficial
                    if (c) {
                        cat = 4;  // coatings last
                    } else if (has("Restore")) {
                        cat = 0;  // most-used
                    } else if (has("Fortify")) {
                        cat = 1;
                    } else if (has("Resist") || has("Cure")) {
                        cat = 2;
                    }
                    const char* pn2 = a->GetName();
                    vlist.push_back({ pid, a, e, c, cat, en ? en : "", (pn2 && *pn2) ? pn2 : "(unknown)" });
                }
                std::sort(vlist.begin(), vlist.end(), [](const VEntry& x, const VEntry& y) {
                    if (x.cat != y.cat) return x.cat < y.cat;
                    if (int d = std::strcmp(x.effName, y.effName)) return d < 0;
                    return std::strcmp(x.potName, y.potName) < 0;
                });
                int lastCat = -1;
                for (const auto& v : vlist) {
                    if (v.cat != lastCat) {
                        lastCat = v.cat;
                        ImGui::TextDisabled("%s", kCatLabels[v.cat]);
                    }
                    const RE::FormID pid  = v.pid;
                    auto*            alch = v.alch;
                    auto*            eff  = v.eff;
                    const char*      pn   = v.potName;
                    const bool       coat = v.coat;
                    const bool       ingrMode = IngredientMode();
                    const FlaskCost cost     = VariantCost(alch) * chargesCap;
                    IngrCharge      ic{};
                    std::string     costStr;
                    if (ingrMode) {  // recipe items, from the render-safe snapshot
                        ic      = IngrCostPerCharge(alch);
                        costStr = IngrCostString(ic, chargesCap);
                    } else {
                        costStr = CostString(cost);
                    }
                    char label[224];
                    std::snprintf(label, sizeof(label), "%-26.80s %s %s##v%08X",
                                  (pn && *pn) ? pn : "(unknown)", coat ? "[coating]" : "        ",
                                  costStr.c_str(), pid);
                    // Coatings are selectable only once Vanguard is held (P2);
                    // relaxed for testing via kCoatingsRequireVanguard.
                    const bool coatOk = !coat || !kCoatingsRequireVanguard || g_hasVanguard.load();
                    // Field mode is free up front (fills over time) but capped per
                    // trip; station mode pays essence — or ingredients when OFF.
                    // A no-recipe variant (pairs == 0) is unpriceable in
                    // ingredient mode and could NEVER refill — gray it in field
                    // mode too (Fable 3f73d4b finding 1).
                    const bool canChange =
                        field ? (g_fieldChanges.load() < kFieldChangeLimit &&
                                 (!ingrMode || ic.pairs > 0))
                              : (ingrMode ? CanAffordIngrSnapshot(ic, chargesCap)
                                          : CanAfford(cost));
                    const bool enabled   = g_selectedSlot >= 0 && coatOk && canChange;
                    if (!enabled) {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::Selectable(label)) {
                        const std::size_t slot  = static_cast<std::size_t>(g_selectedSlot);
                        const RE::FormID  p     = pid;
                        const bool        field = !g_menu.station.load();  // power = field mode
                        // Debounce identical re-selects (marth: MEO's gem pouch
                        // double-counted click+release). ImGui itself fires once
                        // per click, but a duplicated input event in the frame's
                        // list would double-dispatch — and in FIELD mode that
                        // silently burns the one change/trip. Cheap insurance.
                        static RE::FormID    lastPid  = 0;
                        static std::size_t   lastSlot = static_cast<std::size_t>(-1);
                        static std::uint64_t lastMs   = 0;
                        const std::uint64_t  now      = NowMs();
                        if (!(p == lastPid && slot == lastSlot && now - lastMs < 250)) {
                            lastPid  = p;
                            lastSlot = slot;
                            lastMs   = now;
                            SKSE::GetTaskInterface()->AddTask(
                                [slot, p, field]() { ConfigureFlask(slot, p, field); });
                        }
                    }
                    if (!enabled) {
                        ImGui::EndDisabled();
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("Pad: D-pad/stick move, A select, B close.  KB: arrows, Enter, Esc.");
        ImGui::End();
        if (fBody) {
            ImGui::PopFont();
        }
    }

    // Renderer-init hook: grab the device/context/swapchain, stand up ImGui.
    struct D3DInitHook {
        static void thunk() {
            func();
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (!renderer) {
                spdlog::error("[menu] renderer singleton missing — viewer disabled");
                return;
            }
            auto* swapChain = renderer->data.renderWindows[0].swapChain;
            if (!swapChain) {
                spdlog::error("[menu] swapchain missing — viewer disabled");
                return;
            }
            DXGI_SWAP_CHAIN_DESC sd{};
            if (swapChain->GetDesc(&sd) < 0) {
                spdlog::error("[menu] IDXGISwapChain::GetDesc failed — viewer disabled");
                return;
            }
            g_device  = renderer->data.forwarder;
            g_context = renderer->data.context;
            ImGui::CreateContext();
            auto& io       = ImGui::GetIO();
            io.IniFilename = nullptr;  // never write imgui.ini into the game dir
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            // Bake real typefaces at backbuffer scale (the default bitmap font
            // is blurry above 1080p). All optional; each falls back per-font.
            {
                const float uiScale =
                    std::max(1.0f, static_cast<float>(sd.BufferDesc.Height) / 1080.0f);
                constexpr const char* kBodyTTF = "Data/SKSE/Plugins/MAO/fonts/body.ttf";
                constexpr const char* kHeadTTF = "Data/SKSE/Plugins/MAO/fonts/head.ttf";
                constexpr const char* kSansTTF = "Data/SKSE/Plugins/MAO/fonts/sans.ttf";
                if (std::ifstream(kBodyTTF).good()) {
                    g_fontBody = io.Fonts->AddFontFromFileTTF(kBodyTTF, std::floor(19.0f * uiScale));
                }
                if (std::ifstream(kHeadTTF).good()) {
                    g_fontHead = io.Fonts->AddFontFromFileTTF(kHeadTTF, std::floor(27.0f * uiScale));
                }
                if (std::ifstream(kSansTTF).good()) {
                    g_fontSans = io.Fonts->AddFontFromFileTTF(kSansTTF, std::floor(16.5f * uiScale));
                }
                if (!g_fontHead) {
                    g_fontHead = g_fontBody;
                }
                spdlog::info("[menu] fonts: body={} head={} sans={} (scale {:.2f})",
                             g_fontBody ? "ok" : "default", g_fontHead ? "ok" : "default",
                             g_fontSans ? "ok" : "default", uiScale);
            }
            if (!ImGui_ImplWin32_Init(sd.OutputWindow) || !ImGui_ImplDX11_Init(g_device, g_context)) {
                spdlog::error("[menu] ImGui backend init failed — viewer disabled");
                return;
            }
            g_bbW = static_cast<float>(sd.BufferDesc.Width);
            g_bbH = static_cast<float>(sd.BufferDesc.Height);
            g_d3dReady.store(true);
            spdlog::info("[menu] ImGui initialized ({}x{})", sd.BufferDesc.Width,
                         sd.BufferDesc.Height);
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr auto                          id     = REL::RelocationID(75595, 77226);
        static constexpr auto                          offset = REL::VariantOffset(0x9, 0x275, 0x0);
    };

    // Present hook: draw the viewer each frame while it is open.
    struct DXGIPresentHook {
        static void thunk(std::uint32_t a_p1) {
            func(a_p1);
            if (!g_d3dReady.load() || !g_menu.open.load()) {
                return;
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            auto& io = ImGui::GetIO();
            if (g_bbW > 0.0f) {
                io.DisplaySize = ImVec2(g_bbW, g_bbH);
            }
            // On open, push the cursor to center once — ImGui only ever learns
            // it from move events, so the first click before any mouse move
            // would otherwise land at an invalid position.
            if (g_menu.cursorInit.exchange(false)) {
                g_cursorX = (g_bbW > 0.0f ? g_bbW : io.DisplaySize.x) * 0.5f;
                g_cursorY = (g_bbH > 0.0f ? g_bbH : io.DisplaySize.y) * 0.5f;
                io.AddMousePosEvent(g_cursorX, g_cursorY);
            }
            ImGui::NewFrame();
            DrawFieldKit();
            ImGui::EndFrame();
            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr auto                          id     = REL::RelocationID(75461, 77246);
        static constexpr auto                          offset = REL::Offset(0x9);
    };

    // Input dispatch hook. While the menu is open it feeds ImGui (mouse +
    // keyboard/gamepad nav) and swallows everything so the world stays deaf;
    // while closed it only watches for the optional button opener (the power
    // is the primary opener). Ported from MEO's proven routing.
    struct InputDispatchHook {
        static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent** a_events) {
            if (!a_events || !g_d3dReady.load()) {
                func(a_source, a_events);
                return;
            }
            constexpr std::uint32_t kGamepadB = 0x2000;  // BSWin32GamepadDevice::Key::kB
            auto  isOpener = [&](RE::INPUT_DEVICE a_dev, std::uint32_t a_code) {
                return (a_dev == RE::INPUT_DEVICE::kKeyboard && g_openHotkey != 0 &&
                        a_code == g_openHotkey) ||
                       (a_dev == RE::INPUT_DEVICE::kGamepad && g_openButtonGamepad != 0 &&
                        a_code == g_openButtonGamepad);
            };
            // The POWER opens the kit via its cast event — but while the kit is
            // open we null the whole event list below, so the game never sees
            // the shout/power button and no second cast event can ever fire.
            // The toggle-close therefore has to be handled HERE. Resolve the
            // shout binding through the game's own control map so a rebound
            // button (or the Deck's layout) still works.
            auto isShout = [&](RE::INPUT_DEVICE a_dev, std::uint32_t a_code) {
                auto* cm = RE::ControlMap::GetSingleton();
                if (!cm) {
                    return false;
                }
                const auto mapped =
                    cm->GetMappedKey("Shout", a_dev, RE::ControlMap::InputContextID::kGameplay);
                return mapped != 0xFF && mapped != static_cast<std::uint32_t>(-1) &&
                       mapped == a_code;
            };
            const bool wasOpen  = g_menu.open.load();
            bool       justOpen = false;
            auto&      io        = ImGui::GetIO();

            for (auto* e = *a_events; e; e = e->next) {
                if (e->eventType == RE::INPUT_EVENT_TYPE::kMouseMove && wasOpen) {
                    auto* m  = static_cast<RE::MouseMoveEvent*>(e);
                    g_cursorX = std::clamp(g_cursorX + static_cast<float>(m->mouseInputX), 0.0f,
                                           io.DisplaySize.x);
                    g_cursorY = std::clamp(g_cursorY + static_cast<float>(m->mouseInputY), 0.0f,
                                           io.DisplaySize.y);
                    io.AddMousePosEvent(g_cursorX, g_cursorY);
                    continue;
                }
                if (e->eventType == RE::INPUT_EVENT_TYPE::kThumbstick && wasOpen) {
                    auto* th = static_cast<RE::ThumbstickEvent*>(e);
                    if (th->IsLeft()) {
                        static bool held[4] = { false, false, false, false };
                        auto        edge    = [&](int a_i, bool a_on, ImGuiKey a_k) {
                            if (held[a_i] != a_on) {
                                held[a_i] = a_on;
                                io.AddKeyEvent(a_k, a_on);
                            }
                        };
                        edge(0, th->yValue > 0.5f, ImGuiKey_GamepadDpadUp);
                        edge(1, th->yValue < -0.5f, ImGuiKey_GamepadDpadDown);
                        edge(2, th->xValue < -0.5f, ImGuiKey_GamepadDpadLeft);
                        edge(3, th->xValue > 0.5f, ImGuiKey_GamepadDpadRight);
                    }
                    continue;
                }
                if (e->eventType != RE::INPUT_EVENT_TYPE::kButton) {
                    continue;
                }
                auto* b = static_cast<RE::ButtonEvent*>(e);
                if (!b->IsDown() && !b->IsUp()) {
                    continue;  // ignore held-repeat
                }
                const bool          down = b->IsDown();
                const auto          dev  = b->device.get();
                const std::uint32_t code = b->GetIDCode();

                if (!wasOpen) {
                    if (down && isOpener(dev, code)) {
                        OpenFieldKit(false);
                        justOpen = true;
                    }
                    continue;
                }
                // Open: feed ImGui / handle close.
                switch (dev) {
                case RE::INPUT_DEVICE::kMouse:
                    if (code <= 4) {
                        io.AddMouseButtonEvent(static_cast<int>(code), down);
                    } else if (code == 8 && down) {
                        io.AddMouseWheelEvent(0.0f, 1.0f);
                    } else if (code == 9 && down) {
                        io.AddMouseWheelEvent(0.0f, -1.0f);
                    }
                    break;
                case RE::INPUT_DEVICE::kKeyboard:
                    if (code == 0x01 && down) {  // Esc closes
                        CloseFieldKit();
                    } else if (down && (isOpener(dev, code) || isShout(dev, code))) {
                        CloseFieldKit();  // opener OR the power/shout button toggles
                    } else if (auto k = DIKToImGuiKey(code); k != ImGuiKey_None) {
                        io.AddKeyEvent(k, down);
                    }
                    break;
                case RE::INPUT_DEVICE::kGamepad:
                    if (code == kGamepadB && down) {  // B closes
                        CloseFieldKit();
                    } else if (down && (isOpener(dev, code) || isShout(dev, code))) {
                        CloseFieldKit();  // opener OR the power/shout button toggles
                    } else if (auto k = GamepadToImGuiKey(code); k != ImGuiKey_None) {
                        io.AddKeyEvent(k, down);
                    }
                    break;
                default:
                    break;
                }
            }
            if (wasOpen || justOpen) {
                *a_events = nullptr;  // the game sees no input while the menu is open
            }
            func(a_source, a_events);
        }
        static inline REL::Relocation<decltype(thunk)> func;
        static constexpr auto                          id     = REL::RelocationID(67315, 68617);
        static constexpr auto                          offset = REL::Offset(0x7B);
    };

    template <class T>
    void write_thunk_call() {
        auto&                                 trampoline = SKSE::GetTrampoline();
        const REL::Relocation<std::uintptr_t> hook{ T::id, T::offset };
        T::func = trampoline.write_call<5>(hook.address(), T::thunk);
    }

    void Install() {
        SKSE::AllocTrampoline(64);
        write_thunk_call<D3DInitHook>();
        write_thunk_call<DXGIPresentHook>();
        write_thunk_call<InputDispatchHook>();
        spdlog::info("[menu] render + input hooks installed (kb key 0x{:X}, pad button 0x{:X})",
                     g_openHotkey.load(), g_openButtonGamepad.load());
    }

}  // namespace menuhook

// ── The Open Field Kit power (P1a). MAO is native-first: the DLL grants the
// lesser power and a cast-event sink TOGGLES the viewer — no Papyrus, no quest.
// The power is the opener (Marth's design). The gamepad-button opener is
// disabled by default (iOpenButtonGamepad=0) because on the Steam Deck the
// View button doubles as Select — binding the opener there collides. B still
// closes; a keyboard fallback stays available for non-Deck testing.
// The Field Kit opens via ALCHEMY STATION TAKEOVER (copied from MEO's proven
// enchanting-station takeover), replacing the old lesser-power opener. The
// The "Open Field Kit" lesser power opens the kit in FIELD mode away from a
// station (view essences; limited flask changes). Grant it if the player lacks
// it (retries each load, so a mid-save ESP enable still lands).
void GrantFieldKitPower() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (player && g_fieldKitSpell && !player->HasSpell(g_fieldKitSpell)) {
        player->AddSpell(g_fieldKitSpell);
        spdlog::info("[power] granted the Open Field Kit power");
    }
}

// Casting the power opens the field kit. Lesser-power casts fire a spell-cast
// event; open on the task thread.
class CastSink : public RE::BSTEventSink<RE::TESSpellCastEvent> {
public:
    static CastSink* GetSingleton() {
        static CastSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
                                          RE::BSTEventSource<RE::TESSpellCastEvent>*) override {
        if (a_event && a_event->object && a_event->object->IsPlayerRef() && g_fieldKitSpell &&
            a_event->spell == g_fieldKitSpell->GetFormID()) {
            // TOGGLE: casting the power again closes the kit (marth). DEBOUNCED
            // — the engine can dispatch more than one cast event for a single
            // press, and an undebounced toggle would open-then-close within the
            // same press and look like nothing happened.
            static std::atomic<std::uint64_t> lastCastMs{ 0 };
            const std::uint64_t               now  = NowMs();
            const std::uint64_t               prev = lastCastMs.load();
            if (now - prev < 400) {
                return RE::BSEventNotifyControl::kContinue;  // same press
            }
            lastCastMs.store(now);
            SKSE::GetTaskInterface()->AddTask([]() {
                if (g_menu.open.load()) {
                    CloseFieldKit();
                } else {
                    OpenFieldKit(false);  // field mode
                }
            });
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    static MenuSink* GetSingleton() {
        static MenuSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        if (a_event && a_event->opening && a_event->menuName == RE::CraftingMenu::MENU_NAME) {
            SKSE::GetTaskInterface()->AddTask([]() {
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto  furn   = player ? player->GetOccupiedFurniture() : RE::ObjectRefHandle{};
                auto  ref    = furn.get();
                auto* base   = ref ? ref->GetBaseObject() : nullptr;
                auto* f      = base ? base->As<RE::TESFurniture>() : nullptr;
                using BT     = RE::TESFurniture::WorkBenchData::BenchType;
                if (f && (f->workBenchData.benchType == BT::kAlchemy ||
                          f->workBenchData.benchType == BT::kAlchemyExperiment)) {
                    // Alchemy is replaced by the Field Kit: dismiss the vanilla
                    // crafting menu and let our menu own the station.
                    if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                        q->AddMessage(RE::CraftingMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide,
                                      nullptr);
                    }
                    OpenFieldKit(true);
                    // Stand the player out of the alchemy furniture immediately.
                    // Left seated, the engine sees "occupied alchemy table + no
                    // crafting menu" and re-activates the CraftingMenu every few
                    // seconds; each ~1-frame re-open flashes the vanilla menu
                    // chrome through our overlay. Ejecting breaks that loop.
                    player->NotifyAnimationGraph("IdleForceDefaultState");
                }
            });
        } else if (a_event && !a_event->opening &&
                   a_event->menuName == RE::JournalMenu::MENU_NAME) {
            // MCM lives in the Journal (pause) menu; MCM Helper flushes settings
            // to MAO.ini on close. Re-read config + reapply perk overrides now.
            SKSE::GetTaskInterface()->AddTask([]() {
                ReadConfig();
                RecomputeCapacity("mcm");
            });
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

// Set when LoadCallback skips a record from a NEWER MAO. SKSE does NOT
// round-trip unread co-save records, so the next save silently destroys them —
// warn the player on-screen once per load (kPostLoadGame consumes the flag).
std::atomic<bool> g_newerCoSave{ false };

// ── SKSE co-save (kSerVersion 3): POCH (pouch counters) + BLPT (discovered
// variants, v3 potion-keyed) + FLSK (slots). Loader hardened per INVARIANTS.
void SaveCallback(SKSE::SerializationInterface* a_intfc) {
    if (!a_intfc->OpenRecord(kRecPouch, kSerVersion)) {
        spdlog::error("[save] OpenRecord('POCH') failed");
        return;
    }
    const std::uint32_t pb = g_pouch.base.load(), pc = g_pouch.catalyst.load(),
                        pa = g_pouch.apex.load();
    a_intfc->WriteRecordData(pb);  // plain u32s — never serialize the atomic object
    a_intfc->WriteRecordData(pc);
    a_intfc->WriteRecordData(pa);
    spdlog::info("[save] pouch B={} C={} A={}", pb, pc, pa);

    if (a_intfc->OpenRecord(kRecBlueprints, kSerVersion)) {  // v3: discovered potion forms
        std::scoped_lock lk(g_discoveredLock);
        const std::uint32_t n = static_cast<std::uint32_t>(g_discovered.size());
        a_intfc->WriteRecordData(n);
        for (const RE::FormID pid : g_discovered) {
            a_intfc->WriteRecordData(pid);
        }
        spdlog::info("[save] {} discovered variant(s)", n);
    } else {
        spdlog::error("[save] OpenRecord('BLPT') failed");
    }

    if (a_intfc->OpenRecord(kRecFlasks, kSerVersion)) {  // v2: adds repPotion per slot
        std::scoped_lock lk(g_flasksLock);
        a_intfc->WriteRecordData(g_flaskCount);
        a_intfc->WriteRecordData(g_chargesPerFlask);
        for (const auto& f : g_flasks) {  // always all kMaxFlaskSlots slots
            a_intfc->WriteRecordData(f.blueprint);
            a_intfc->WriteRecordData(f.repPotion);
            a_intfc->WriteRecordData(f.charges);
        }
        spdlog::info("[save] flasks: {} slot(s), {} charge cap", g_flaskCount, g_chargesPerFlask);
    } else {
        spdlog::error("[save] OpenRecord('FLSK') failed");
    }
}

void LoadCallback(SKSE::SerializationInterface* a_intfc) {
    g_pouch.Reset();
    {
        std::scoped_lock lk(g_discoveredLock);
        g_discovered.clear();
    }
    std::uint32_t type = 0, version = 0, length = 0;
    // Short-read helper: every count-driven read must be verified — a truncated
    // record must bail, never fabricate state (MEO INVARIANTS: bound counts,
    // bail on short read, clamp at ingestion).
    auto readOk = [&](auto& a_val) { return a_intfc->ReadRecordData(a_val) == sizeof(a_val); };
    while (a_intfc->GetNextRecordInfo(type, version, length)) {
        if (version > kSerVersion) {
            spdlog::error("[load] a co-save record (v{}) is from a NEWER MAO — skipped; "
                          "SAVING NOW WILL DESTROY IT",
                          version);
            g_newerCoSave.store(true);
            continue;
        }
        if (type == kRecPouch) {
            std::uint32_t pb = 0, pc = 0, pa = 0;
            if (readOk(pb) && readOk(pc) && readOk(pa)) {
                // v1.0.1 note: Catalyst/Apex were GOLD-denominated before this
                // version and are now valued in small tier units, so a balance
                // earned under the old economy buys much more. Deliberately NOT
                // rescaled (marth): "I have no interest in messing with
                // peoples stored essence, they have what they have."
                g_pouch.base.store(pb);
                g_pouch.catalyst.store(pc);
                g_pouch.apex.store(pa);
            } else {
                spdlog::error("[load] POCH: short read — pouch reset");
                g_pouch.Reset();
            }
        } else if (type == kRecBlueprints) {
            std::uint32_t n = 0;
            if (!readOk(n)) {
                spdlog::error("[load] BLPT: short read on count — record dropped");
                continue;
            }
            // Bound before looping: a corrupt count would otherwise drive up to
            // 2^32 iterations. Real saves hold hundreds of variants at most.
            constexpr std::uint32_t kMaxBlueprints = 65536;
            if (n > kMaxBlueprints) {
                spdlog::error("[load] BLPT: count {} is implausible — clamped to {} "
                              "(corrupt record?)",
                              n, kMaxBlueprints);
                n = kMaxBlueprints;
            }
            std::scoped_lock lk(g_discoveredLock);
            for (std::uint32_t i = 0; i < n; ++i) {
                if (version >= 3) {  // v3: a discovered potion form
                    RE::FormID pid = 0, res = 0;
                    if (!readOk(pid)) {
                        spdlog::error("[load] BLPT: short read at entry {}/{} — rest dropped", i, n);
                        break;
                    }
                    if (pid && a_intfc->ResolveFormID(pid, res) && res) {
                        g_discovered.insert(res);
                    }
                } else {  // v1/v2 were effect-keyed — migrate to a variant
                    RE::FormID    mgef = 0, rep = 0, mRes = 0, rRes = 0;
                    std::uint32_t repValue = 0;
                    if (!readOk(mgef)) {
                        spdlog::error("[load] BLPT: short read at entry {}/{} — rest dropped", i, n);
                        break;
                    }
                    if (version >= 2) {
                        if (!readOk(rep) || !readOk(repValue)) {
                            spdlog::error("[load] BLPT: short read at entry {}/{} — rest dropped",
                                          i, n);
                            break;
                        }
                    }
                    // v2 kept the found rep potion; v1 only the effect — fall
                    // back to the load-order rep so the known effect stays usable.
                    if (rep && a_intfc->ResolveFormID(rep, rRes) && rRes) {
                        g_discovered.insert(rRes);
                    } else if (a_intfc->ResolveFormID(mgef, mRes) && mRes) {
                        if (RE::FormID r = RepPotionFor(mRes)) {
                            g_discovered.insert(r);
                        } else {
                            spdlog::warn("[load] migration: effect {:08X} has no load-order potion "
                                         "— dropped (re-discover to restore)",
                                         mRes);
                        }
                    }
                }
            }
        } else if (type == kRecFlasks) {
            std::scoped_lock lk(g_flasksLock);
            if (!readOk(g_flaskCount) || !readOk(g_chargesPerFlask)) {
                spdlog::error("[load] FLSK: short read on header — record dropped");
                g_flaskCount      = 2;  // revert baseline (RevertCallback values)
                g_chargesPerFlask = 2;
                continue;
            }
            // Clamp against a corrupt/edited record — g_flaskCount bounds hot
            // loops (FindFlaskSlot on every container event + drink); an OOB
            // value would read/write past g_flasks[kMaxFlaskSlots].
            g_flaskCount      = std::min(g_flaskCount, static_cast<std::uint32_t>(kMaxFlaskSlots));
            g_chargesPerFlask = std::clamp(g_chargesPerFlask, 1u, 99u);
            for (auto& f : g_flasks) {
                RE::FormID bpId = 0, repId = 0, bpRes = 0, repRes = 0;
                bool       ok   = readOk(bpId);
                if (ok && version >= 2) {  // v2: rep potion form per slot
                    ok = readOk(repId);
                }
                if (ok) {
                    ok = readOk(f.charges);
                }
                if (!ok) {
                    spdlog::error("[load] FLSK: short read mid-slots — rest left empty");
                    f = Flask{};
                    break;
                }
                f.blueprint = (bpId && a_intfc->ResolveFormID(bpId, bpRes)) ? bpRes : 0;
                f.repPotion = (repId && a_intfc->ResolveFormID(repId, repRes)) ? repRes : 0;
            }
        }
    }
    // A slot's currently-equipped variant must be discoverable (so reconfiguring
    // that slot can re-select it). Migrated saves can have a flask rep that the
    // BLPT migration didn't seed — backfill it here.
    std::vector<RE::FormID> flaskReps;
    {
        std::scoped_lock lk(g_flasksLock);
        for (const auto& f : g_flasks) {
            if (f.repPotion) {
                flaskReps.push_back(f.repPotion);
            }
        }
    }
    std::size_t variants = 0;
    {
        std::scoped_lock lk(g_discoveredLock);
        for (const RE::FormID id : flaskReps) {
            g_discovered.insert(id);
        }
        variants = g_discovered.size();
    }
    spdlog::info("[load] pouch B={} C={} A={}; {} discovered variant(s)", g_pouch.base.load(),
                 g_pouch.catalyst.load(), g_pouch.apex.load(), variants);
}

void RevertCallback(SKSE::SerializationInterface*) {
    g_newerCoSave.store(false);  // re-armed per load; LoadCallback may set it again
    {
        std::scoped_lock lk(g_coatingLock);  // coating clock is runtime-only
        g_coatingExpiryMs = 0;
        g_coatingPoison   = 0;
    }
    g_pouch.Reset();
    {
        std::scoped_lock lk(g_discoveredLock);
        g_discovered.clear();
    }
    {
        std::scoped_lock lk(g_flasksLock);
        g_flasks = {};
        g_flaskCount      = 2;
        g_chargesPerFlask = 2;
    }
    g_menu.open.store(false);
    spdlog::info("[revert] pouch + blueprints + flasks cleared");
}

void OnMessage(SKSE::MessagingInterface::Message* a_message) {
    switch (a_message->type) {
    case SKSE::MessagingInterface::kDataLoaded: {
        ReadConfig();
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        holder->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::GetSingleton());
        holder->AddEventSink<RE::TESSleepStopEvent>(SleepSink::GetSingleton());
        holder->AddEventSink<RE::TESSpellCastEvent>(CastSink::GetSingleton());  // field-kit power
        RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
        RE::TESHarvestedEvent::GetEventSource()->AddEventSink(HarvestSink::GetSingleton());
        if (auto* dh = RE::TESDataHandler::GetSingleton()) {
            g_fieldKitSpell = dh->LookupForm<RE::SpellItem>(kFieldKitSpellID, kPluginName);
            int nf          = 0;
            for (std::uint32_t i = 0; i < g_flaskForms.size(); ++i) {
                g_flaskForms[i] =
                    dh->LookupForm<RE::AlchemyItem>(kFlaskBaseID + i, kPluginName);
                nf += g_flaskForms[i] ? 1 : 0;
                // Snapshot the baked placeholder effect for later restore.
                if (auto* f = g_flaskForms[i]; f && !f->effects.empty() && f->effects[0]) {
                    const auto& ei       = f->effects[0]->effectItem;
                    g_flaskPlaceholder[i] = { f->effects[0]->baseEffect, ei.magnitude, ei.area,
                                              ei.duration, true };
                }
            }
            spdlog::info("[flask] {}/{} flask forms resolved", nf, g_flaskForms.size());
            // Resolve MAO's own flag perks (MEO vehicle: vanilla perks are no
            // longer touched — no renames, no reads).
            int np = 0, na = 0;
            for (int i = 0; i < PK_COUNT; ++i) {
                g_perkForm[i] = dh->LookupForm<RE::BGSPerk>(kPerkDefs[i].id, kPluginName);
                if (g_perkForm[i]) {
                    ++np;
                    if (i >= PK_ALCH1 && i <= PK_ALCH5) ++na;
                }
            }
            g_capacityPerksResolved = (na > 0);
            // Tree mode: the installer-written patch puts these perks into the
            // AVAlchemy constellation (player buys them with perk points).
            // Without it, RecomputeCapacity auto-grants them by skill threshold.
            g_treeMode.store(dh->LookupModByName("MAO - Patch.esp") != nullptr);
            // Fluid Motion: detect a drink-animation mod (plugin or SKSE dll);
            // absent -> the perk disables itself (marth 2026-07-16).
            const char* animHit = nullptr;
            for (const char* pl : kDrinkAnimPlugins) {
                if (dh->LookupModByName(pl)) {
                    animHit = pl;
                    break;
                }
            }
            if (!animHit) {
                for (const char* dl : kDrinkAnimDlls) {
                    if (std::filesystem::exists(dl)) {
                        animHit = dl;
                        break;
                    }
                }
            }
            g_drinkAnimMod.store(animHit != nullptr);
            spdlog::info("[perks] Fluid Motion: {}",
                         animHit ? std::format("drink-animation mod detected ({})", animHit)
                                 : "no drink-animation mod found — perk disables itself");
            spdlog::info("[perks] resolved {}/{} MAO flag perks ({}/5 calibration ranks); "
                         "mode {}; capacity {} (0=missing -> holds co-saved)",
                         np, static_cast<int>(PK_COUNT), na,
                         g_treeMode.load() ? "TREE (MAO - Patch.esp)" : "auto-grant",
                         g_capacityPerksResolved ? "live" : "held");
        }
        spdlog::info("[power] Open Field Kit spell: {}", g_fieldKitSpell ? "found" : "MISSING (is MAO.esp enabled?)");
        LoadTierMap();  // before BuildRecipeTable — it tiers ingredients per this map
        BuildRecipeTable();
        BuildEffectPotionTable();
        InstallDrinkHook();
        StartRefillTimer();
        const auto gameVersion = REL::Module::get().version();
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("MAO native v%s loaded", kPluginVersion);
        }
        spdlog::info("kDataLoaded: MAO v{} live on runtime {}; sinks + viewer + power registered",
                     kPluginVersion, gameVersion.string());
        break;
    }
    case SKSE::MessagingInterface::kPostLoadGame:
    case SKSE::MessagingInterface::kNewGame:
        // Player exists here (after LoadCallback/Revert). Grant the power if
        // absent; retries every load, so a mid-save ESP enable still lands.
        SKSE::GetTaskInterface()->AddTask([]() {
            GrantFieldKitPower();
            RecomputeCapacity("load");  // authoritative capacity BEFORE we sync items,
            SyncFlaskItems();           // so a shrunk kit doesn't grant now-dead slots
            StripStaleCoatings();       // time-based coatings never cross a reload
            // Downgrade warning: skipped newer-MAO records are NOT round-tripped
            // by SKSE — saving now destroys them. Say so where the player can
            // see it, not just the log (MEO release lesson).
            if (g_newerCoSave.exchange(false)) {
                RE::DebugMessageBox(
                    "MAO: this save contains data from a NEWER version of MAO, which "
                    "this older build cannot read. That data was NOT loaded — if you "
                    "save now, it will be PERMANENTLY LOST. Update MAO before saving.");
            }
        });
        break;
    default:
        break;
    }
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    SetupLog();
    // The INI is re-read at kDataLoaded, but the render hooks are installed
    // here (before the renderer initializes), so seed the hotkey now.
    ReadConfig();
    menuhook::Install();

    const auto gameVersion = REL::Module::get().version();
    spdlog::info("MAO native v{} loading; runtime {}", kPluginVersion, gameVersion.string());
    if (gameVersion != REL::Version(1, 6, 1170, 0)) {
        spdlog::warn("Untested runtime {} (built against 1.6.1170)", gameVersion.string());
    }

    auto* serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID(kSerID);
    serialization->SetSaveCallback(SaveCallback);
    serialization->SetLoadCallback(LoadCallback);
    serialization->SetRevertCallback(RevertCallback);

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    spdlog::info("SKSEPluginLoad complete; serialization + messaging registered");
    return true;
}
