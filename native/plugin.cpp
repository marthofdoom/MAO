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
//   M1 (this build): THE GATHERING LOOP. TESContainerChanged sink is the sole
//       essence-credit point (catches harvest, loot, container, barter,
//       script AddItem); the ingredient is removed so nothing lingers in bags
//       (essence is never an inventory item). Tier picks the bucket, gold
//       value picks the amount. TESHarvested sink tags harvests for the future
//       Field-Extraction / Pouch-Expansion perks (logs only — never credits,
//       so one harvest = one credit). Pouch persists in the 'POCH' co-save.
//   M2: the Field Kit power → ImGui read-only essence viewer (render hook).
//
// ── SAVE-SAFETY RULES (inherited from MEO; apply from the first co-save) ──
//   1. The co-save 'POCH' schema is VERSIONED. Readers for every shipped
//      version stay forever; writers write only the newest. Never reorder or
//      remove fields — extend via a version bump + migration in LoadCallback.
//   2. MAO.esp FormIDs (once the ESP exists at M2) are frozen generator
//      constants. Forms are only ever ADDED, never renumbered or deleted.

#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

namespace {

constexpr auto kPluginVersion = "0.1.0 (M1 gathering)";

constexpr std::uint32_t kSerID       = 'MAO1';
constexpr std::uint32_t kRecPouch    = 'POCH';
constexpr std::uint32_t kSerVersion  = 1;
constexpr RE::FormID    kPlayerID    = 0x14;

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

// ── Config (Data/SKSE/Plugins/MAO.ini). The full MCM option set arrives with
// P1; P0 seeds the surface with the one toggle it needs.
bool g_notify = true;  // bNotify — per-pickup on-screen essence notifications

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
    // comments, parse "key = value". P0 reads one key; the surface is here so
    // P1's MCM has somewhere to write.
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
    spdlog::info("[config] bNotify={}", g_notify);
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
        auto* ingr = form ? form->As<RE::IngredientItem>() : nullptr;
        if (!ingr) {
            return RE::BSEventNotifyControl::kContinue;
        }
        const char*      cname = ingr->GetName();
        std::string_view name  = cname ? cname : "";
        if (IsExcluded(name)) {
            spdlog::info("[gather] EXCLUDED '{}' ({:08X}) — kept as item, no conversion",
                         name, a_event->baseObj);
            return RE::BSEventNotifyControl::kContinue;
        }

        const Tier          tier    = TierOf(name);
        const int           value   = ingr->value;
        const int           count   = a_event->itemCount;
        const std::uint32_t perItem = YieldFor(value, tier);
        const std::uint32_t total   = perItem * static_cast<std::uint32_t>(count);
        const std::string   nameStr(name);

        // Mutating inventory inside a container-changed event is unsafe; defer
        // one frame via the task interface (the same discipline MEO uses).
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
}

void LoadCallback(SKSE::SerializationInterface* a_intfc) {
    g_pouch = Pouch{};
    std::uint32_t type = 0, version = 0, length = 0;
    while (a_intfc->GetNextRecordInfo(type, version, length)) {
        if (type != kRecPouch) {
            continue;
        }
        if (version > kSerVersion) {
            spdlog::error("[load] 'POCH' v{} is from a NEWER MAO — pouch left empty this session",
                          version);
            continue;
        }
        a_intfc->ReadRecordData(g_pouch.base);
        a_intfc->ReadRecordData(g_pouch.catalyst);
        a_intfc->ReadRecordData(g_pouch.apex);
    }
    spdlog::info("[load] pouch B={} C={} A={}", g_pouch.base, g_pouch.catalyst, g_pouch.apex);
}

void RevertCallback(SKSE::SerializationInterface*) {
    g_pouch = Pouch{};
    spdlog::info("[revert] pouch cleared");
}

void OnMessage(SKSE::MessagingInterface::Message* a_message) {
    if (a_message->type == SKSE::MessagingInterface::kDataLoaded) {
        ReadConfig();
        RE::ScriptEventSourceHolder::GetSingleton()
            ->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::GetSingleton());
        RE::TESHarvestedEvent::GetEventSource()->AddEventSink(HarvestSink::GetSingleton());
        const auto gameVersion = REL::Module::get().version();
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("MAO native v%s loaded", kPluginVersion);
        }
        spdlog::info("kDataLoaded: MAO v{} live on runtime {}; gathering sinks registered",
                     kPluginVersion, gameVersion.string());
    }
}

}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    SetupLog();

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
