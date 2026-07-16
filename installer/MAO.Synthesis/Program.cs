// MAO.Synthesis — the marth's Alchemy Overhaul patcher as a Synthesis patch.
// (Vehicle ported verbatim from MEO.Synthesis; only the tree target and the
// per-load-order artifact differ.)
//
// Does what the standalone installer does, calling the SHARED Commands.* code
// (MAO.Installer/Commands.Patch.cs + Commands.Tiers.cs):
//   (a) rewrites the alchemy perk tree (AVAlchemy) to MAO's flag perks,
//       into Synthesis's own output mod, and
//   (b) derives + writes Data/SKSE/Plugins/MAO/mao_tiers.json (the ingredient
//       rarity map the DLL loads) for this load order.

using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;
using Mutagen.Bethesda.Synthesis;

return await SynthesisPipeline.Instance
    .AddPatch<ISkyrimMod, ISkyrimModGetter>(RunPatch)
    .SetTypicalOpen(GameRelease.SkyrimSE, "MAO - Patch.esp")
    .Run(args);

static void RunPatch(IPatcherState<ISkyrimMod, ISkyrimModGetter> state)
{
    // Match the standalone installer's ESL flag: MAO - Patch.esp is a pure
    // override plugin, so it costs no load-order slot.
    state.PatchMod.IsSmallMaster = true;

    // (a) Perk-tree edit into Synthesis's output mod. Non-interactive KEEP-ALL —
    // Synthesis users curate foreign perks via their own load order. Uses
    // Synthesis's load order (Creation Club adds no alchemy perk-tree edits).
    var rc = Commands.ApplyPatch(state.LoadOrder, state.LinkCache, state.PatchMod);
    if (rc != 0)
        throw new Exception($"MAO ApplyPatch failed (rc={rc}) — see log above");

    // (b) Ingredient tier map. Build the load order OURSELVES so it includes
    // Creation Club content (Skyrim.ccc). Synthesis's load order omits CC
    // plugins that aren't in plugins.txt — and on an Anniversary Edition
    // install that is ALL of them (CC loads via Skyrim.ccc, not plugins.txt).
    // MEO measured ~24% coverage loss relying on Synthesis's order; CC packs
    // (Fish, Curios, Rare Curios) carry a large share of MAO's ingredients, so
    // the tier map has the same exposure. Mirrors the standalone installer's
    // ordering (base masters -> CC -> plugins), resolving from the data folder.
    var maoDir = Path.Combine(state.DataFolderPath, "SKSE", "Plugins", "MAO");
    Directory.CreateDirectory(maoDir);
    var tiersOutPath = Path.Combine(maoDir, "mao_tiers.json");

    var (tierLo, tierCache) = BuildTiersLoadOrder(state);
    var trc = Commands.WriteTiers(tierLo, tierCache, tiersOutPath);
    if (trc != 0)
        throw new Exception($"MAO WriteTiers failed (rc={trc}) — see log above");
}

// A load order that includes Creation Club (Skyrim.ccc), mirroring the
// standalone installer's ordering, so tier coverage matches it.
static (LoadOrder<IModListingGetter<ISkyrimModGetter>>, ILinkCache) BuildTiersLoadOrder(
    IPatcherState<ISkyrimMod, ISkyrimModGetter> state)
{
    string dataDir = state.DataFolderPath;
    string gameRoot = Directory.GetParent(dataDir)?.FullName ?? dataDir;
    string[] baseMasters = { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" };

    var cccPath = Path.Combine(gameRoot, "Skyrim.ccc");
    var ccc = File.Exists(cccPath)
        ? File.ReadLines(cccPath).Select(l => l.Trim())
              .Where(l => l.Length > 0 && !l.StartsWith('#')).ToList()
        : new List<string>();

    var pluginsTxt = (string)state.LoadOrderFilePath;
    var active = File.Exists(pluginsTxt)
        ? File.ReadLines(pluginsTxt).Where(l => l.StartsWith('*'))
              .Select(l => l[1..].Trim()).ToList()
        : new List<string>();

    var files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    foreach (var f in Directory.EnumerateFiles(dataDir))
        files.TryAdd(Path.GetFileName(f), f);

    var listings = new List<IModListingGetter<ISkyrimModGetter>>();
    foreach (var name in baseMasters.Concat(ccc).Concat(active)
                 .Distinct(StringComparer.OrdinalIgnoreCase))
    {
        if (name.Equals("MAO - Patch.esp", StringComparison.OrdinalIgnoreCase))
            continue;  // never read our own output
        if (!files.TryGetValue(name, out var full)) continue;
        var mod = SkyrimMod.CreateFromBinaryOverlay(
            new ModPath(ModKey.FromNameAndExtension(name), full), SkyrimRelease.SkyrimSE);
        listings.Add(new ModListing<ISkyrimModGetter>(mod, enabled: true));
    }
    var lo = new LoadOrder<IModListingGetter<ISkyrimModGetter>>(listings);
    return (lo, lo.ToImmutableLinkCache());
}
