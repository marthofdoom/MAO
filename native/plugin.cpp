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
//   M2 (this build): THE FIELD KIT VIEWER. An ImGui overlay (D3D11 present
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr auto kPluginVersion = "0.15.0 (MCM: tuning + perk debug page)";

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

// ── Essence pouch: the abstracted store. Three tier counters, nothing more.
// This is the entire P0 persisted state; it lives here and serializes to the
// co-save, never as an inventory item.
enum class Tier : std::uint8_t { Base = 0, Catalyst = 1, Apex = 2 };

struct Pouch {
    std::uint32_t base     = 0;
    std::uint32_t catalyst = 0;
    std::uint32_t apex     = 0;
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
// PK_ALCH1..5 are the vanilla Alchemist chain (separate records, ranks 1..5).
enum PerkIdx : int {
    PK_ALCH1, PK_ALCH2, PK_ALCH3, PK_ALCH4, PK_ALCH5,
    PK_CAPSTONE, PK_BENEFACTOR, PK_EXPERIMENTER,
    PK_PHYSICIAN, PK_POISONER, PK_GREENTHUMB, PK_SNAKEBLOOD, PK_CONCPOISON,
    PK_COUNT
};
// own = the form lives in MAO.esp (our capstone); otherwise it's a vanilla
// Skyrim.esm alchemy perk. The capstone replaces vanilla Purity as the 6/9
// ceiling — MAO's own perk so the load-order-aware installer can place it later.
struct PerkDef { const char* label; RE::FormID id; bool own; const char* iniKey; };
constexpr PerkDef kPerkDefs[PK_COUNT] = {
    { "Alchemist I  (+1 charge)",             0xBE127, false, "bPerkAlch1" },
    { "Alchemist II  (+1 flask)",             0xC07CA, false, "bPerkAlch2" },
    { "Alchemist III  (refill eff, P2)",      0xC07CB, false, "bPerkAlch3" },
    { "Alchemist IV  (+1 flask +2 chg)",      0xC07CC, false, "bPerkAlch4" },
    { "Alchemist V  (rest refill, P2)",       0xC07CD, false, "bPerkAlch5" },
    { "Master's Crucible  (+2 flask +4 chg)", 0x816,   true,  "bPerkCapstone" },  // MAO capstone
    { "Benefactor  (Apex -35%)",              0x58216, false, "bPerkBenefactor" },
    { "Experimenter  (+10% gather)",          0x58218, false, "bPerkExperimenter" },
    { "Physician  (P2)",                      0x58215, false, "bPerkPhysician" },
    { "Poisoner  (P2)",                       0x58217, false, "bPerkPoisoner" },
    { "Green Thumb  (P2)",                    0x105F2E, false, "bPerkGreenThumb" },
    { "Snakeblood  (P2)",                     0x105F2C, false, "bPerkSnakeblood" },
    { "Concentrated Poison  (P2)",            0x105F2F, false, "bPerkConcPoison" },
};
RE::BGSPerk*               g_perkForm[PK_COUNT] = {};
std::atomic<std::uint32_t> g_perkMask{ 0 };  // bit i = kPerkDefs[i] is effectively active
// MCM debug (MEO-style override): when g_perkDebug, capacity/efficiency are
// driven by g_perkWantMask (the MCM toggles) INSTEAD of the real perks — no
// AddPerk/RemovePerk, so real progression is never disturbed and it reverts
// cleanly when the toggle is turned off.
std::atomic<bool>          g_perkDebug{ false };
std::atomic<std::uint32_t> g_perkWantMask{ 0 };
bool              g_capacityPerksResolved = false;  // vanilla Alchemist chain present?
std::atomic<int>  g_alchemistRank{ 0 };
std::atomic<bool> g_hasCapstone{ false };
std::atomic<bool> g_hasBenefactor{ false };   // read in VariantCost (render + task)
std::atomic<bool> g_hasExperimenter{ false };  // read in the gather credit (task)

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
    // If the vanilla Alchemist chain isn't present (Requiem / missing masters)
    // and we're not overriding, hold the co-saved counts rather than force the
    // baseline and clamp away charges the player legitimately earned.
    if (!g_capacityPerksResolved && !debug) {
        spdlog::info("[perks] {}: no vanilla capacity perks resolved — holding co-saved "
                     "{} flasks / {} charges",
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
    spdlog::info("[perks] {}{}: Alchemist rank {}, Capstone {}, Benefactor {}, Experimenter {} "
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
// concentration × tax, added to that ingredient's tier pool.
struct RecipeIng { std::uint32_t value = 5; Tier tier = Tier::Base; };
struct Recipe { RecipeIng a, b; };
std::unordered_map<RE::FormID, Recipe> g_effectRecipe;
std::uint32_t g_catalystUnit = 40;   // representative Catalyst ingredient value (surcharge unit)
std::uint32_t g_apexUnit     = 120;  // representative Apex ingredient value
std::mutex    g_effectCostLock;
float                                         g_essenceTax = 1.3f;   // fEssenceTax
// Flask cost = round(effect baseCost * magnitude * g_costRate * tax) per charge
// (linear in magnitude = "each concentration level costs 1x more"). The flask's
// ESSENCE TIER steps up with concentration = magnitude / the effect's weakest
// (standard) magnitude: >= g_catalystLevel -> Catalyst, >= g_apexLevel -> Apex.
float g_costRate      = 1.0f;  // fCostRate
float g_catalystLevel = 3.0f;  // fCatalystLevel — concentration x for Catalyst tier
float g_apexLevel     = 6.0f;  // fApexLevel — concentration x for Apex tier

// Effect -> the strongest load-order potion/poison carrying it as its primary
// effect. This is what physically embodies a blueprint (the flask item + what
// the drink hook casts). Deriving it from the load order — instead of only the
// exact potion you found — means every discovered blueprint is usable, incl.
// blueprints from pre-P1d saves that never stored a representative.
std::unordered_map<RE::FormID, std::pair<RE::FormID, std::uint32_t>> g_effectPotion;
std::unordered_map<RE::FormID, float> g_effectMinMag;  // MGEF -> weakest (standard) primary-effect magnitude
std::mutex                            g_effectPotionLock;

// Flask helpers — defined below near the container sink; forward-declared for
// ConfigureFlask / the drink hook above them.
int  FindFlaskSlot(RE::FormID a_form);
bool IsFlaskForm(RE::FormID a_form);
void RenameFlask(std::size_t a_slot, RE::AlchemyItem* a_variant);

// ── Field Kit viewer state (M2). open/close is driven by the input hook and
// read by the present hook — both run off the game's render/input threads, so
// these are atomics.
struct MenuState {
    std::atomic<bool> open{ false };
    std::atomic<bool> cursorInit{ false };  // push cursor to center on next open
    std::atomic<bool> station{ false };     // opened from an alchemy station (takeover)
};
MenuState g_menu;

// Open/close the Field Kit. When opened from a station, closing forces the
// player up out of the furniture (the vanilla crafting menu was hidden).
void OpenFieldKit(bool a_station) {
    g_menu.station.store(a_station);
    g_menu.cursorInit.store(true);
    g_menu.open.store(true);
    // Refresh perk-scaled capacity in case a perk was taken since the last load.
    SKSE::GetTaskInterface()->AddTask([]() { RecomputeCapacity("open"); });
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
bool          g_notify     = true;    // bNotify — per-pickup essence notifications
std::uint32_t g_openHotkey = 0x25;    // iOpenHotkey — keyboard DirectInput scancode; 0x25 = K
// iOpenButtonGamepad — RE::BSWin32GamepadDevice::Key bitflag, or 0 = disabled.
// DEFAULT 0: the power is the opener. On the Steam Deck the View button (0x20)
// doubles as Select, so binding an opener there collides — leave it off.
std::uint32_t g_openButtonGamepad = 0x0;
int           g_menuStyle         = 0;    // iMenuStyle — Field Kit skin 0..3
std::uint32_t g_refillSeconds     = 300;  // iRefillSeconds — real-time refill cadence

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

// P0 quest-ingredient exclusion: these must NEVER dissolve into essence.
// Crimson Nirnroot drives "A Return To Your Roots". P1 broadens this to a
// quest-item flag check plus the generated exclusion set.
constexpr std::array<std::string_view, 1> kExclusions{ { "Crimson Nirnroot" } };

// Per-tier yield rate: essence = ceil(goldValue * rate), min 1. All 1.0 for
// P0 (amount == value, trivial to verify in the log); becomes the real
// MCM-tunable curve in P1. Tier still only selects the bucket.
constexpr std::array<float, 3> kTierRate{ { 1.0f, 1.0f, 1.0f } };

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

bool IsExcluded(std::string_view a_name) {
    for (const auto& ex : kExclusions) {
        if (ContainsCI(a_name, ex)) {
            return true;
        }
    }
    return false;
}

std::uint32_t YieldFor(int a_value, Tier a_tier) {
    const float raw = static_cast<float>(std::max(0, a_value)) * kTierRate[static_cast<int>(a_tier)];
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
    // Per effect, the (value, tier) of every ingredient carrying it; plus the
    // pool of Catalyst / Apex ingredient values for the surcharge units.
    std::unordered_map<RE::FormID, std::vector<std::pair<std::uint32_t, Tier>>> byEffect;
    std::vector<std::uint32_t> catVals, apexVals;
    std::uint32_t              ingredients = 0;
    for (auto* ingr : dh->GetFormArray<RE::IngredientItem>()) {
        if (!ingr || ingr->value <= 0) {
            continue;
        }
        ++ingredients;
        const char*         nm = ingr->GetName();
        const Tier          it = TierOf(nm ? nm : "");
        const std::uint32_t v  = static_cast<std::uint32_t>(ingr->value);
        if (it == Tier::Catalyst) {
            catVals.push_back(v);
        } else if (it == Tier::Apex) {
            apexVals.push_back(v);
        }
        for (auto* eff : ingr->effects) {
            if (eff && eff->baseEffect) {
                byEffect[eff->baseEffect->GetFormID()].push_back({ v, it });
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
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        // Cheapest 2 ingredients sharing the effect = the vanilla recipe. Keep
        // both so the base cost can split across their (possibly different) tiers.
        const auto& i0 = vec[0];
        const auto& i1 = vec.size() > 1 ? vec[1] : vec[0];
        g_effectRecipe[id] = { { std::max(1u, i0.first), i0.second },
                               { std::max(1u, i1.first), i1.second } };
    }
    g_catalystUnit = median(catVals, 40);
    g_apexUnit     = median(apexVals, 120);
    spdlog::info("[econ] recipe table: {} effects from {} ingredients; catalystUnit={} apexUnit={}",
                 g_effectRecipe.size(), ingredients, g_catalystUnit, g_apexUnit);
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
Tier TierAbove(Tier t) { return t == Tier::Base ? Tier::Catalyst : Tier::Apex; }

// Compound per-charge cost for a specific variant (Marth's model):
//   BASE = each recipe ingredient's value x concentration x tax, added to THAT
//          ingredient's own tier pool (so a Base+Catalyst recipe splits into
//          part Base + part Catalyst by design).
//   +CAT = high concentration (>= fCatalystLevel) needs a tier-up ingredient's
//          worth; doubles at >= fApexLevel (Vigorous -> 1, Extreme+ -> 2).
//   +APEX = any multi-effect potion needs an Apex ingredient's worth.
// Concentration = magnitude / the effect's weakest magnitude (magnitude-ranked).
FlaskCost VariantCost(RE::AlchemyItem* a_alch) {
    FlaskCost fc{};
    if (!a_alch || a_alch->effects.empty() || !a_alch->effects[0] ||
        !a_alch->effects[0]->baseEffect) {
        return fc;
    }
    auto*       eff = a_alch->effects[0]->baseEffect;
    const float mag = a_alch->effects[0]->effectItem.magnitude > 0.0f
                          ? a_alch->effects[0]->effectItem.magnitude
                          : 1.0f;
    Recipe        rec{};
    std::uint32_t catUnit = 40, apexUnit = 120;
    {
        std::scoped_lock lk(g_effectCostLock);
        auto             it = g_effectRecipe.find(eff->GetFormID());
        if (it != g_effectRecipe.end()) {
            rec = it->second;
        }
        catUnit  = g_catalystUnit;
        apexUnit = g_apexUnit;
    }
    float minMag = 0.0f;
    {
        std::scoped_lock lk(g_effectPotionLock);
        auto             it = g_effectMinMag.find(eff->GetFormID());
        if (it != g_effectMinMag.end()) {
            minMag = it->second;
        }
    }
    const float conc  = (minMag > 0.01f) ? std::max(1.0f, mag / minMag) : 1.0f;
    const float basis = conc * g_costRate * g_essenceTax;
    // BASE splits per ingredient, each in its own tier.
    for (const RecipeIng& ing : { rec.a, rec.b }) {
        AddTier(fc, ing.tier,
                std::max(1u, static_cast<std::uint32_t>(std::lround(ing.value * basis))));
    }
    if (conc >= g_catalystLevel) {  // surcharge sits one tier above the recipe's rarest
        const Tier          su   = TierAbove(std::max(rec.a.tier, rec.b.tier));
        const std::uint32_t unit = (su == Tier::Apex) ? apexUnit : catUnit;
        AddTier(fc, su, unit * (conc >= g_apexLevel ? 2u : 1u));
    }
    if (a_alch->effects.size() > 1) {
        fc.apex += apexUnit;
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
bool SpendCost(const FlaskCost& c) {
    if (!CanAfford(c)) {
        return false;
    }
    g_pouch.base -= c.base;
    g_pouch.catalyst -= c.catalyst;
    g_pouch.apex -= c.apex;
    return true;
}
std::string CostString(const FlaskCost& c) {
    std::string s;
    if (c.base) s += std::format("{}B ", c.base);
    if (c.catalyst) s += std::format("{}C ", c.catalyst);
    if (c.apex) s += std::format("{}A ", c.apex);
    return s.empty() ? std::string("free") : s;
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
    for (auto* alch : dh->GetFormArray<RE::AlchemyItem>()) {
        if (!alch || alch->IsFood() || alch->effects.empty() || !alch->effects[0] ||
            !alch->effects[0]->baseEffect) {
            continue;
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
    std::scoped_lock lk(g_effectPotionLock);
    g_effectPotion.clear();
    g_effectMinMag = std::move(minMag);
    for (const auto& [e, b] : best) {
        g_effectPotion[e] = { b.form, b.value };
    }
    spdlog::info("[potions] rep table: {} effect(s)", g_effectPotion.size());
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
void ConfigureFlask(std::size_t a_slot, RE::FormID a_potion) {
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

    // Coatings (detrimental) can't be prepared into a drink-flask yet: a poison
    // is USED via the weapon-application path (not DrinkPotion), so vanilla
    // would just consume the flask. The coating mechanic is P2.
    if (alch->effects[0]->baseEffect->IsHostile() || alch->effects[0]->baseEffect->IsDetrimental()) {
        spdlog::info("[flask] slot {} DENIED — coating (weapon application is P2)", a_slot);
        if (g_notify) {
            RE::DebugNotification("Coatings can't be prepared yet (coming with the Vanguard perk).");
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
    if (!SpendCost(cost)) {
        spdlog::info("[flask] slot {} configure DENIED — need {} (have B={} C={} A={})", a_slot,
                     CostString(cost), g_pouch.base, g_pouch.catalyst, g_pouch.apex);
        if (g_notify) {
            RE::DebugNotification(std::format("Not enough essence — need {}.", CostString(cost)).c_str());
        }
        return;
    }
    std::uint32_t hadCharges = 0;
    {
        std::scoped_lock lk(g_flasksLock);
        hadCharges       = g_flasks[a_slot].charges;
        g_flasks[a_slot] = { a_blueprint, rep, g_chargesPerFlask };  // rep = variant source
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
    spdlog::info("[flask] slot {} <- '{}' (variant {:08X}, {} charges); spent {}(was {} charges); "
                 "pouch B={} C={} A={}",
                 a_slot, vName ? vName : "?", rep, g_chargesPerFlask, CostString(cost), hadCharges,
                 g_pouch.base, g_pouch.catalyst, g_pouch.apex);
    if (g_notify) {
        RE::DebugNotification(
            std::format("Flask {} set: {}", a_slot + 1, vName ? vName : "potion").c_str());
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
    // Reset the name of any flask form NOT configured this save (global form
    // state could carry a stale "Flask: X" from another save this session).
    for (std::size_t i = 0; i < g_flaskForms.size(); ++i) {
        if (!synced[i] && g_flaskForms[i]) {
            g_flaskForms[i]->fullName = RE::BSFixedString("Field Flask");
        }
    }
}

// ── Refill (P1d-3): dry/partial flasks top back up from essence, on sleep and
// on a real-time timer. Refilling costs essence per charge (a maintenance cost;
// perks cut it in P1e). Charges are native, so refill never touches the
// inventory item. Runs on the task thread.
void RefillFlasks(const char* a_trigger) {
    if (!RE::PlayerCharacter::GetSingleton()) {
        return;  // no game loaded (timer fires at the main menu too)
    }
    std::uint32_t refilled = 0, slots = 0;
    {
        std::scoped_lock lk(g_flasksLock);
        for (std::uint32_t i = 0; i < g_flaskCount; ++i) {
            auto& f = g_flasks[i];
            if (!f.blueprint || f.repPotion == 0 || f.charges >= g_chargesPerFlask) {
                continue;
            }
            auto*           valch = RE::TESForm::LookupByID<RE::AlchemyItem>(f.repPotion);
            const FlaskCost fc    = valch ? VariantCost(valch) : FlaskCost{};
            std::uint32_t   added = 0;
            while (f.charges < g_chargesPerFlask && SpendCost(fc)) {  // one charge's worth
                ++f.charges;
                ++added;
            }
            if (added) {
                refilled += added;
                ++slots;
            }
        }
    }
    if (refilled) {
        spdlog::info("[refill] {}: +{} charge(s) across {} flask(s); pouch B={}", a_trigger,
                     refilled, slots, g_pouch.base);
        if (g_notify) {
            RE::DebugNotification(std::format("Field Kit replenished (+{} charges).", refilled).c_str());
        }
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

void StartRefillTimer() {
    static std::atomic<bool> started{ false };
    if (started.exchange(true)) {
        return;  // once
    }
    std::thread([]() {
        for (;;) {
            const std::uint32_t secs = g_refillSeconds ? g_refillSeconds : 300;
            std::this_thread::sleep_for(std::chrono::seconds(secs));
            SKSE::GetTaskInterface()->AddTask([]() { RefillFlasks("timer"); });
        }
    }).detach();
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
                // A coating (detrimental effect) isn't drunk — it applies to a
                // weapon. The application mechanic is P2; for now, don't harm
                // the player and don't spend a charge.
                bool coating = false;
                if (auto* eff = RE::TESForm::LookupByID<RE::EffectSetting>(bpMgef)) {
                    coating = eff->IsHostile() || eff->IsDetrimental();
                }
                if (coating) {
                    spdlog::info("[drink] flask slot {} is a coating — not drinkable (P2)", slot);
                    if (g_notify) {
                        RE::DebugNotification("That's a coating — weapon application coming soon.");
                    }
                    return true;
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
                int remaining = -1;
                {
                    std::scoped_lock lk(g_flasksLock);
                    if (g_flasks[slot].repPotion == variant && g_flasks[slot].charges > 0) {
                        g_flasks[slot].charges--;
                        remaining = static_cast<int>(g_flasks[slot].charges);
                    }
                }
                if (remaining >= 0) {
                    if (auto* caster =
                            a_this->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)) {
                        caster->CastSpellImmediate(vAlch, false, a_this, 1.0f, false, 0.0f, a_this);
                    }
                    PlayDrinkSound(a_this, vAlch);
                    spdlog::info("[drink] flask slot {} drunk; {} charge(s) left", slot, remaining);
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
    if (key == "bNotify") {
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
    } else if (key == "fCatalystLevel") {
        g_catalystLevel = std::clamp(static_cast<float>(std::strtod(val.c_str(), nullptr)), 1.1f, 50.0f);
    } else if (key == "fApexLevel") {
        g_apexLevel = std::clamp(static_cast<float>(std::strtod(val.c_str(), nullptr)), 1.2f, 100.0f);
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
    for (const char* path : { "Data/SKSE/Plugins/MAO.ini", "Data/MCM/Settings/MAO.ini" }) {
        std::ifstream f(path);
        std::string   line;
        while (std::getline(f, line)) {
            ApplyIniLine(line);
        }
    }
    spdlog::info("[config] bNotify={} iOpenHotkey=0x{:X} debugPerks={} wantMask=0x{:X}", g_notify,
                 g_openHotkey, g_perkDebug.load(), g_perkWantMask.load());
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
void RenameFlask(std::size_t a_slot, RE::AlchemyItem* a_variant) {
    if (a_slot >= g_flaskForms.size() || !g_flaskForms[a_slot] || !a_variant) {
        return;
    }
    const char* vn = a_variant->GetName();
    g_flaskForms[a_slot]->fullName =
        RE::BSFixedString(std::format("Flask: {}", (vn && *vn) ? vn : "Alchemy").c_str());
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
            const Tier tier  = TierOf(name);
            const int  value = ingr->value;
            std::uint32_t total = YieldFor(value, tier) * static_cast<std::uint32_t>(count);
            if (g_hasExperimenter.load()) {  // Field Extraction: +10% gather yield
                total = std::max(1u, static_cast<std::uint32_t>(std::lround(total * 1.10)));
            }
            const std::string nameStr(name);
            SKSE::GetTaskInterface()->AddTask([ingr, count, tier, total, value, nameStr]() {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
                    return;
                }
                player->RemoveItem(ingr, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                CreditPouch(tier, total);
                spdlog::info("[gather] +{} {} essence <- {}x '{}' (value {}); pouch B={} C={} A={}",
                             total, TierName(tier), count, nameStr, value,
                             g_pouch.base, g_pouch.catalyst, g_pouch.apex);
                if (g_notify) {
                    RE::DebugNotification(
                        std::format("+{} {} Essence ({})", total, TierName(tier), nameStr).c_str());
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
            const int           value   = alch->GetGoldValue();
            const RE::FormID    potForm = alch->GetFormID();
            const std::uint32_t total   = YieldFor(value, Tier::Base) * static_cast<std::uint32_t>(count);
            const char*         pcName  = alch->GetName();
            const std::string   potName(pcName ? pcName : "potion");
            const std::string   bpName(bpCName ? bpCName : "");
            SKSE::GetTaskInterface()->AddTask([alch, count, bpKey, potForm, total, potName,
                                               bpName]() {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
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
                CreditPouch(Tier::Base, total);
                spdlog::info("[discover] '{}' analyzed ({}); effect '{}'; +{} Base essence; "
                             "pouch B={} C={} A={}",
                             potName, bpKey ? (learned ? "NEW variant" : "known") : "no-effect",
                             bpName.empty() ? "?" : bpName, total, g_pouch.base, g_pouch.catalyst,
                             g_pouch.apex);
                if (g_notify) {
                    if (learned) {
                        RE::DebugNotification(std::format("Discovered: {}", potName).c_str());
                    }
                    RE::DebugNotification(std::format("+{} Base Essence ({})", total, potName).c_str());
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
            const char* n = a_event->produceItem->GetName();
            spdlog::info("[harvest] player harvested '{}' (tag only; credit via container sink)",
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
        const int       skinIdx = std::clamp(g_menuStyle, 0, 3);
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
        ImGui::Text("Essence   Base %u    Catalyst %u    Apex %u", g_pouch.base, g_pouch.catalyst,
                    g_pouch.apex);
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
            if (g_discovered.empty()) {
                ImGui::TextDisabled("None yet — pick up potions to analyze them.");
            } else {
                if (g_selectedSlot < 0) {
                    ImGui::TextDisabled("Select a flask above first.");
                }
                ImGui::BeginChild("variants", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f));
                for (const RE::FormID pid : g_discovered) {
                    auto* alch = RE::TESForm::LookupByID<RE::AlchemyItem>(pid);
                    if (!alch || alch->effects.empty() || !alch->effects[0] ||
                        !alch->effects[0]->baseEffect) {
                        continue;
                    }
                    auto*           eff  = alch->effects[0]->baseEffect;
                    const char*     pn   = alch->GetName();
                    const bool      coat = eff->IsHostile() || eff->IsDetrimental();
                    const FlaskCost cost = VariantCost(alch) * chargesCap;
                    char            label[224];
                    std::snprintf(label, sizeof(label), "%-26.80s %s %s##v%08X",
                                  (pn && *pn) ? pn : "(unknown)", coat ? "[coating]" : "        ",
                                  CostString(cost).c_str(), pid);
                    const bool enabled = g_selectedSlot >= 0 && !coat && CanAfford(cost);
                    if (!enabled) {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::Selectable(label)) {
                        const std::size_t slot = static_cast<std::size_t>(g_selectedSlot);
                        const RE::FormID  p    = pid;
                        SKSE::GetTaskInterface()->AddTask([slot, p]() { ConfigureFlask(slot, p); });
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
                    } else if (auto k = DIKToImGuiKey(code); k != ImGuiKey_None) {
                        io.AddKeyEvent(k, down);
                    }
                    break;
                case RE::INPUT_DEVICE::kGamepad:
                    if (code == kGamepadB && down) {  // B closes
                        CloseFieldKit();
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
                     g_openHotkey, g_openButtonGamepad);
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
// dormant "Open Field Kit" SPEL stays frozen in the ESP; remove it from any
// save that got it during the power era so it doesn't clutter Powers.
void RemoveFieldKitPower() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (player && g_fieldKitSpell && player->HasSpell(g_fieldKitSpell)) {
        player->RemoveSpell(g_fieldKitSpell);
        spdlog::info("[power] removed the legacy Open Field Kit power (opener is now the station)");
    }
}

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

// ── SKSE co-save: the 'POCH' record (schema v1). Three tier counters.
void SaveCallback(SKSE::SerializationInterface* a_intfc) {
    if (!a_intfc->OpenRecord(kRecPouch, kSerVersion)) {
        spdlog::error("[save] OpenRecord('POCH') failed");
        return;
    }
    a_intfc->WriteRecordData(g_pouch.base);
    a_intfc->WriteRecordData(g_pouch.catalyst);
    a_intfc->WriteRecordData(g_pouch.apex);
    spdlog::info("[save] pouch B={} C={} A={}", g_pouch.base, g_pouch.catalyst, g_pouch.apex);

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
    g_pouch = Pouch{};
    {
        std::scoped_lock lk(g_discoveredLock);
        g_discovered.clear();
    }
    std::uint32_t type = 0, version = 0, length = 0;
    while (a_intfc->GetNextRecordInfo(type, version, length)) {
        if (version > kSerVersion) {
            spdlog::error("[load] a co-save record (v{}) is from a NEWER MAO — skipped", version);
            continue;
        }
        if (type == kRecPouch) {
            a_intfc->ReadRecordData(g_pouch.base);
            a_intfc->ReadRecordData(g_pouch.catalyst);
            a_intfc->ReadRecordData(g_pouch.apex);
        } else if (type == kRecBlueprints) {
            std::uint32_t n = 0;
            a_intfc->ReadRecordData(n);
            std::scoped_lock lk(g_discoveredLock);
            for (std::uint32_t i = 0; i < n; ++i) {
                if (version >= 3) {  // v3: a discovered potion form
                    RE::FormID pid = 0, res = 0;
                    a_intfc->ReadRecordData(pid);
                    if (pid && a_intfc->ResolveFormID(pid, res) && res) {
                        g_discovered.insert(res);
                    }
                } else {  // v1/v2 were effect-keyed — migrate to a variant
                    RE::FormID    mgef = 0, rep = 0, mRes = 0, rRes = 0;
                    std::uint32_t repValue = 0;
                    a_intfc->ReadRecordData(mgef);
                    if (version >= 2) {
                        a_intfc->ReadRecordData(rep);
                        a_intfc->ReadRecordData(repValue);
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
            a_intfc->ReadRecordData(g_flaskCount);
            a_intfc->ReadRecordData(g_chargesPerFlask);
            // Clamp against a corrupt/edited record — g_flaskCount bounds hot
            // loops (FindFlaskSlot on every container event + drink); an OOB
            // value would read/write past g_flasks[kMaxFlaskSlots].
            g_flaskCount      = std::min(g_flaskCount, static_cast<std::uint32_t>(kMaxFlaskSlots));
            g_chargesPerFlask = std::clamp(g_chargesPerFlask, 1u, 99u);
            for (auto& f : g_flasks) {
                RE::FormID bpId = 0, repId = 0, bpRes = 0, repRes = 0;
                a_intfc->ReadRecordData(bpId);
                if (version >= 2) {  // v2: rep potion form per slot
                    a_intfc->ReadRecordData(repId);
                }
                a_intfc->ReadRecordData(f.charges);
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
    spdlog::info("[load] pouch B={} C={} A={}; {} discovered variant(s)", g_pouch.base,
                 g_pouch.catalyst, g_pouch.apex, variants);
}

void RevertCallback(SKSE::SerializationInterface*) {
    g_pouch = Pouch{};
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
        RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
        RE::TESHarvestedEvent::GetEventSource()->AddEventSink(HarvestSink::GetSingleton());
        if (auto* dh = RE::TESDataHandler::GetSingleton()) {
            g_fieldKitSpell = dh->LookupForm<RE::SpellItem>(kFieldKitSpellID, kPluginName);
            int nf          = 0;
            for (std::uint32_t i = 0; i < g_flaskForms.size(); ++i) {
                g_flaskForms[i] =
                    dh->LookupForm<RE::AlchemyItem>(kFlaskBaseID + i, kPluginName);
                nf += g_flaskForms[i] ? 1 : 0;
            }
            spdlog::info("[flask] {}/{} flask forms resolved", nf, g_flaskForms.size());
            // P1e: resolve the vanilla alchemy perks that drive capacity + cost
            // (and the rest, so the debug menu can toggle them individually).
            int np = 0, na = 0;
            for (int i = 0; i < PK_COUNT; ++i) {
                g_perkForm[i] = dh->LookupForm<RE::BGSPerk>(
                    kPerkDefs[i].id, kPerkDefs[i].own ? kPluginName : "Skyrim.esm");
                if (g_perkForm[i]) {
                    ++np;
                    if (i >= PK_ALCH1 && i <= PK_ALCH5) ++na;
                }
            }
            g_capacityPerksResolved = (na > 0) || (g_perkForm[PK_CAPSTONE] != nullptr);
            spdlog::info("[perks] resolved {}/{} alchemy perks ({}/5 Alchemist ranks); "
                         "capacity {} (0=missing -> holds co-saved)",
                         np, static_cast<int>(PK_COUNT), na,
                         g_capacityPerksResolved ? "live" : "held");
        }
        spdlog::info("[power] Open Field Kit spell: {}", g_fieldKitSpell ? "found" : "MISSING (is MAO.esp enabled?)");
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
            RemoveFieldKitPower();
            RecomputeCapacity("load");  // authoritative capacity BEFORE we sync items,
            SyncFlaskItems();           // so a shrunk kit doesn't grant now-dead slots
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
