// MAO native plugin — MAO.dll (CommonLibSSE-NG).
//
// Marth's Alchemy Overhaul: flask-based alchemy. Single-use potions are
// replaced by permanent Flasks charged from abstracted Material Essences the
// player gathers in the field. State lives here and serializes to the SKSE
// co-save — no Papyrus save bloat. See Docs/DESIGN.md and Docs/P0_PLAN.md.
//
// Built in staged, individually CI-green milestones (Docs/BUILD.md), one hook
// class per step so a CTD bisects to one change (MRO doctrine):
//   M0 (this build): skeleton — DLL loads, logs its version + the runtime.
//                    Zero hooks. Proves the Linux→CI→artifact pipeline.
//   M1: the gathering loop — TESContainerChanged + TESHarvested event sinks,
//       value-weighted essence credit, the 'POCH' co-save record.
//   M2: the Field Kit power → ImGui read-only essence viewer (render hook).
//
// ── SAVE-SAFETY RULES (inherited from MEO; apply from the first co-save) ──
//   1. The co-save 'POCH' schema is VERSIONED. Readers for every shipped
//      version stay forever; writers write only the newest. Never reorder or
//      remove fields — extend via a version bump + migration in LoadCallback.
//   2. MAO.esp FormIDs (once the ESP exists at M2) are frozen generator
//      constants. Forms are only ever ADDED, never renumbered or deleted.

#include <spdlog/sinks/basic_file_sink.h>

namespace {

constexpr auto kPluginVersion = "0.0.1 (M0 skeleton)";

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

void OnMessage(SKSE::MessagingInterface::Message* a_message) {
    if (a_message->type == SKSE::MessagingInterface::kDataLoaded) {
        const auto gameVersion = REL::Module::get().version();
        if (auto* console = RE::ConsoleLog::GetSingleton()) {
            console->Print("MAO native v%s loaded", kPluginVersion);
        }
        spdlog::info("kDataLoaded: MAO v{} live on runtime {}; no hooks yet (M0 skeleton)",
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

    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    spdlog::info("SKSEPluginLoad complete; messaging registered");
    return true;
}
