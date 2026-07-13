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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

constexpr auto kPluginVersion = "0.5.1 (P1c flasks + skins)";

constexpr std::uint32_t kSerID         = 'MAO1';
constexpr std::uint32_t kRecPouch      = 'POCH';
constexpr std::uint32_t kRecBlueprints = 'BLPT';
constexpr std::uint32_t kRecFlasks     = 'FLSK';
constexpr std::uint32_t kSerVersion    = 1;
constexpr RE::FormID    kPlayerID      = 0x14;

// MAO.esp forms (P1a). FROZEN — must match MAO_GenerateESP.py. ESL-flagged
// plugin, so LookupForm takes the low local ID + the plugin name.
constexpr const char* kPluginName      = "MAO.esp";
constexpr RE::FormID  kFieldKitSpellID = 0x801;  // Open Field Kit lesser power
RE::SpellItem*        g_fieldKitSpell  = nullptr;

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

// ── Known blueprints (P1b): the set of effect profiles the player has
// discovered by analyzing found potions. Keyed by the potion's primary
// MagicEffect FormID, so all strength variants of an effect collapse into one
// blueprint (the design's "family by effect"). Persisted in 'BLPT'. Guarded by
// a mutex — the render thread reads it while the task thread inserts.
std::unordered_set<RE::FormID> g_blueprints;
std::mutex                     g_blueprintsLock;

// ── Flasks (P1c). Permanent kit slots, each holding a blueprint payload +
// charges. Native slots for now (physical drinkable item forms + drinking
// arrive in P1d). Slot/charge counts are the unperked baseline here; P1e
// scales them via the perk ladder. Guarded — render reads, task writes.
constexpr std::size_t kMaxFlaskSlots = 6;  // design ceiling (6 flasks / 9 charges)

struct Flask {
    RE::FormID    blueprint = 0;  // 0 = empty
    std::uint32_t charges   = 0;
};
std::array<Flask, kMaxFlaskSlots> g_flasks{};
std::uint32_t                     g_flaskCount     = 2;  // unlocked slots (P1e: perk-scaled)
std::uint32_t                     g_chargesPerFlask = 2;  // max charges (P1e: perk-scaled)
std::mutex                        g_flasksLock;

// P1c essence cost to configure/refill a flask: flat Base per charge. A
// placeholder economy (MCM-tunable later); tier-aware cost comes with the
// real blueprint tiers.
constexpr std::uint32_t kConfigCostPerCharge = 5;

// ── Field Kit viewer state (M2). open/close is driven by the input hook and
// read by the present hook — both run off the game's render/input threads, so
// these are atomics.
struct MenuState {
    std::atomic<bool> open{ false };
    std::atomic<bool> cursorInit{ false };  // push cursor to center on next open
};
MenuState g_menu;

// ── Config (Data/SKSE/Plugins/MAO.ini). The full MCM option set arrives with
// P1; P0 seeds the surface with the keys it needs.
bool          g_notify     = true;    // bNotify — per-pickup essence notifications
std::uint32_t g_openHotkey = 0x25;    // iOpenHotkey — keyboard DirectInput scancode; 0x25 = K
// iOpenButtonGamepad — RE::BSWin32GamepadDevice::Key bitflag, or 0 = disabled.
// DEFAULT 0: the power is the opener. On the Steam Deck the View button (0x20)
// doubles as Select, so binding an opener there collides — leave it off.
std::uint32_t g_openButtonGamepad = 0x0;
int           g_menuStyle         = 0;  // iMenuStyle — Field Kit skin 0..3

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
constexpr std::array<TierOverride, 3> kTierOverrides{ {
    { "Daedra Heart", Tier::Apex },
    { "Nightshade",   Tier::Catalyst },
    { "Deathbell",    Tier::Catalyst },
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

bool SpendBase(std::uint32_t a_amount) {
    if (g_pouch.base < a_amount) {
        return false;
    }
    g_pouch.base -= a_amount;
    return true;
}

// Configure a flask slot with a blueprint (P1c). Runs on the task thread so it
// serializes with gathering/discovery. Deducts the essence cost up front and
// fills the flask to max charges; reassigning a flask that still holds charges
// purges them with no refund (the §3.1 overwrite penalty).
void ConfigureFlask(std::size_t a_slot, RE::FormID a_blueprint) {
    if (a_slot >= g_flaskCount || !a_blueprint) {
        return;
    }
    {
        std::scoped_lock lk(g_blueprintsLock);
        if (!g_blueprints.contains(a_blueprint)) {
            return;  // must be a discovered blueprint
        }
    }
    const std::uint32_t cost = kConfigCostPerCharge * g_chargesPerFlask;
    if (!SpendBase(cost)) {
        spdlog::info("[flask] slot {} configure DENIED — need {} Base (have {})",
                     a_slot, cost, g_pouch.base);
        if (g_notify) {
            RE::DebugNotification(std::format("Not enough Base Essence ({} needed).", cost).c_str());
        }
        return;
    }
    std::uint32_t hadCharges = 0;
    {
        std::scoped_lock lk(g_flasksLock);
        hadCharges           = g_flasks[a_slot].charges;
        g_flasks[a_slot].blueprint = a_blueprint;
        g_flasks[a_slot].charges   = g_chargesPerFlask;
    }
    const char* bpName = nullptr;
    if (auto* mgef = RE::TESForm::LookupByID<RE::EffectSetting>(a_blueprint)) {
        bpName = mgef->GetName();
    }
    spdlog::info("[flask] slot {} <- blueprint '{}' ({} charges); spent {} Base (purged {} old); "
                 "pouch B={}",
                 a_slot, bpName ? bpName : "?", g_chargesPerFlask, cost, hadCharges, g_pouch.base);
    if (g_notify) {
        RE::DebugNotification(
            std::format("Flask {} set: {}", a_slot + 1, bpName ? bpName : "effect").c_str());
    }
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
    }
}

void ReadConfig() {
    for (const char* path : { "Data/SKSE/Plugins/MAO.ini", "Data/MCM/Settings/MAO.ini" }) {
        std::ifstream f(path);
        std::string   line;
        while (std::getline(f, line)) {
            ApplyIniLine(line);
        }
    }
    spdlog::info("[config] bNotify={} iOpenHotkey=0x{:X}", g_notify, g_openHotkey);
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
            const Tier          tier    = TierOf(name);
            const int           value   = ingr->value;
            const std::uint32_t total   = YieldFor(value, tier) * static_cast<std::uint32_t>(count);
            const std::string   nameStr(name);
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
            RE::FormID  bpKey   = 0;
            const char* bpCName = nullptr;
            if (!alch->effects.empty() && alch->effects[0] && alch->effects[0]->baseEffect) {
                bpKey   = alch->effects[0]->baseEffect->GetFormID();
                bpCName = alch->effects[0]->baseEffect->GetName();
            }
            const int           value   = alch->GetGoldValue();
            const std::uint32_t total   = YieldFor(value, Tier::Base) * static_cast<std::uint32_t>(count);
            const char*         pcName  = alch->GetName();
            const std::string   potName(pcName ? pcName : "potion");
            const std::string   bpName(bpCName ? bpCName : "");
            SKSE::GetTaskInterface()->AddTask([alch, count, bpKey, total, potName, bpName]() {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
                    return;
                }
                player->RemoveItem(alch, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                bool learned = false;
                if (bpKey) {
                    std::scoped_lock lk(g_blueprintsLock);
                    learned = g_blueprints.insert(bpKey).second;
                }
                CreditPouch(Tier::Base, total);
                spdlog::info("[discover] '{}' analyzed -> blueprint '{}' ({}); +{} Base essence; "
                             "pouch B={} C={} A={}",
                             potName, bpName.empty() ? "?" : bpName,
                             bpKey ? (learned ? "NEW" : "known") : "no-effect", total,
                             g_pouch.base, g_pouch.catalyst, g_pouch.apex);
                if (g_notify) {
                    if (learned && !bpName.empty()) {
                        RE::DebugNotification(std::format("Blueprint learned: {}", bpName).c_str());
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

        const std::uint32_t cost = kConfigCostPerCharge * g_chargesPerFlask;

        // ── Flasks ──
        ImGui::TextDisabled("FLASKS  —  select one, then a blueprint below");
        ImGui::Separator();
        {
            std::scoped_lock lk(g_flasksLock);
            if (g_selectedSlot >= static_cast<int>(g_flaskCount)) {
                g_selectedSlot = -1;
            }
            for (int i = 0; i < static_cast<int>(g_flaskCount); ++i) {
                const auto& f      = g_flasks[i];
                const char* bpName = "(empty)";
                if (f.blueprint) {
                    if (auto* m = RE::TESForm::LookupByID<RE::EffectSetting>(f.blueprint)) {
                        if (m->GetName() && *m->GetName()) {
                            bpName = m->GetName();
                        }
                    }
                }
                char label[160];
                std::snprintf(label, sizeof(label), "Flask %d:  %s    [%u/%u]##flask%d", i + 1,
                              bpName, f.charges, g_chargesPerFlask, i);
                if (ImGui::Selectable(label, g_selectedSlot == i)) {
                    g_selectedSlot = i;
                }
            }
        }
        ImGui::Spacing();

        // ── Blueprints ──
        ImGui::TextDisabled("BLUEPRINTS  —  %u Base to set a flask", cost);
        ImGui::Separator();
        {
            std::scoped_lock lk(g_blueprintsLock);
            if (g_blueprints.empty()) {
                ImGui::TextDisabled("None yet — pick up potions to analyze them.");
            } else {
                if (g_selectedSlot < 0) {
                    ImGui::TextDisabled("Select a flask above first.");
                }
                const bool affordable = g_pouch.base >= cost;
                ImGui::BeginChild("blueprints", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f));
                for (const RE::FormID id : g_blueprints) {
                    auto*       m = RE::TESForm::LookupByID<RE::EffectSetting>(id);
                    const char* n = m ? m->GetName() : nullptr;
                    char        label[160];
                    std::snprintf(label, sizeof(label), "%s##bp%08X", (n && *n) ? n : "(unknown)",
                                  id);
                    const bool enabled = g_selectedSlot >= 0 && affordable;
                    if (!enabled) {
                        ImGui::BeginDisabled();
                    }
                    if (ImGui::Selectable(label)) {
                        const std::size_t slot = static_cast<std::size_t>(g_selectedSlot);
                        const RE::FormID  bp   = id;
                        SKSE::GetTaskInterface()->AddTask([slot, bp]() { ConfigureFlask(slot, bp); });
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
                        g_menu.open.store(true);
                        g_menu.cursorInit.store(true);
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
                        g_menu.open.store(false);
                    } else if (auto k = DIKToImGuiKey(code); k != ImGuiKey_None) {
                        io.AddKeyEvent(k, down);
                    }
                    break;
                case RE::INPUT_DEVICE::kGamepad:
                    if (code == kGamepadB && down) {  // B closes
                        g_menu.open.store(false);
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
void GrantFieldKitPower() {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !g_fieldKitSpell) {
        return;
    }
    if (!player->HasSpell(g_fieldKitSpell)) {
        player->AddSpell(g_fieldKitSpell);
        spdlog::info("[power] granted Open Field Kit power");
        RE::DebugNotification("Learned the Open Field Kit power.");
    }
}

class SpellCastSink : public RE::BSTEventSink<RE::TESSpellCastEvent> {
public:
    static SpellCastSink* GetSingleton() {
        static SpellCastSink singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
                                          RE::BSTEventSource<RE::TESSpellCastEvent>*) override {
        if (a_event && g_fieldKitSpell && a_event->spell == g_fieldKitSpell->GetFormID() &&
            a_event->object && a_event->object->IsPlayerRef()) {
            SKSE::GetTaskInterface()->AddTask([]() {
                const bool now = !g_menu.open.load();
                g_menu.open.store(now);
                if (now) {
                    g_menu.cursorInit.store(true);
                }
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

    if (a_intfc->OpenRecord(kRecBlueprints, kSerVersion)) {
        std::scoped_lock lk(g_blueprintsLock);
        const std::uint32_t n = static_cast<std::uint32_t>(g_blueprints.size());
        a_intfc->WriteRecordData(n);
        for (const RE::FormID id : g_blueprints) {
            a_intfc->WriteRecordData(id);
        }
        spdlog::info("[save] {} blueprint(s)", n);
    } else {
        spdlog::error("[save] OpenRecord('BLPT') failed");
    }

    if (a_intfc->OpenRecord(kRecFlasks, kSerVersion)) {
        std::scoped_lock lk(g_flasksLock);
        a_intfc->WriteRecordData(g_flaskCount);
        a_intfc->WriteRecordData(g_chargesPerFlask);
        for (const auto& f : g_flasks) {  // always all kMaxFlaskSlots slots
            a_intfc->WriteRecordData(f.blueprint);
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
        std::scoped_lock lk(g_blueprintsLock);
        g_blueprints.clear();
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
            std::scoped_lock lk(g_blueprintsLock);
            for (std::uint32_t i = 0; i < n; ++i) {
                RE::FormID id = 0, resolved = 0;
                a_intfc->ReadRecordData(id);
                // Remap across load-order changes; drop forms whose plugin is gone.
                if (a_intfc->ResolveFormID(id, resolved) && resolved) {
                    g_blueprints.insert(resolved);
                }
            }
        } else if (type == kRecFlasks) {
            std::scoped_lock lk(g_flasksLock);
            a_intfc->ReadRecordData(g_flaskCount);
            a_intfc->ReadRecordData(g_chargesPerFlask);
            for (auto& f : g_flasks) {
                RE::FormID id = 0, resolved = 0;
                a_intfc->ReadRecordData(id);
                a_intfc->ReadRecordData(f.charges);
                f.blueprint = (id && a_intfc->ResolveFormID(id, resolved)) ? resolved : 0;
            }
        }
    }
    std::size_t bp = 0;
    {
        std::scoped_lock lk(g_blueprintsLock);
        bp = g_blueprints.size();
    }
    spdlog::info("[load] pouch B={} C={} A={}; {} blueprint(s)", g_pouch.base, g_pouch.catalyst,
                 g_pouch.apex, bp);
}

void RevertCallback(SKSE::SerializationInterface*) {
    g_pouch = Pouch{};
    {
        std::scoped_lock lk(g_blueprintsLock);
        g_blueprints.clear();
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
        holder->AddEventSink<RE::TESSpellCastEvent>(SpellCastSink::GetSingleton());
        RE::TESHarvestedEvent::GetEventSource()->AddEventSink(HarvestSink::GetSingleton());
        if (auto* dh = RE::TESDataHandler::GetSingleton()) {
            g_fieldKitSpell = dh->LookupForm<RE::SpellItem>(kFieldKitSpellID, kPluginName);
        }
        spdlog::info("[power] Open Field Kit spell: {}", g_fieldKitSpell ? "found" : "MISSING (is MAO.esp enabled?)");
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
        SKSE::GetTaskInterface()->AddTask([]() { GrantFieldKitPower(); });
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
