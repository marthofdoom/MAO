// MAO.Installer — install-time inspector for marth's Alchemy Overhaul.
//
// Resolves an MO2 profile's load order on Linux (no VFS running), reads it
// with Mutagen, and provides read-only inspection of the alchemy actor-value
// perk tree the load order shipped with. The write-patch (perk-tree override)
// is intentionally NOT implemented yet — it is blocked on a design decision.
// This app is scaffolding + read-only commands, copied from the proven MEO
// installer and trimmed to what MAO needs today.
//
// Usage:
//   MAO.Installer stats        <MO2-or-game root> <profile-or-plugins.txt|auto>
//   MAO.Installer tree         <MO2-or-game root> <profile-or-plugins.txt|auto> [AVIF EditorID]
//   MAO.Installer tree-effects <MO2-or-game root> <profile-or-plugins.txt|auto> [AVIF EditorID]
//   MAO.Installer perk         <MO2-or-game root> <profile-or-plugins.txt|auto> <PERK EditorID>

using System.Diagnostics;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;

const string Usage =
    "usage: MAO.Installer <stats|tree|tree-effects|perk|write-tiers|write-patch> <MO2-or-game root> <profile-or-plugins.txt|auto> [args]\n" +
    "       tree/tree-effects default the AVIF to AVAlchemy when no EditorID is given\n" +
    "       write-tiers [out.json] classifies every ingredient into a rarity tier\n" +
    "                   by availability and writes it (default data/mao_tiers.json)\n" +
    "       write-patch [out.esp] replaces the winning AVAlchemy tree's craft perks\n" +
    "                   with MAO's and writes the override patch (default MAO - Patch.esp)";
if (args.Length == 0 || args[0] is "-h" or "--help" or "help")
{
    Console.WriteLine(Usage);
    return 0;
}
if (args.Length < 3)
{
    Console.Error.WriteLine(Usage);
    return 1;
}

string cmd = args[0], mo2Root = args[1], profile = args[2];
// Trailing options: --sub <esp> replaces the same-named plugin's file (test a
// regenerated plugin in place), --add <esp> appends at the end of the order.
var subs = new List<string>();
var adds = new List<string>();
var positional = new List<string>();
for (int i = 3; i < args.Length; i++)
{
    if (args[i] == "--sub") subs.Add(args[++i]);
    else if (args[i] == "--add") adds.Add(args[++i]);
    else positional.Add(args[i]);
}

List<(string Name, string Path)> resolved;
List<string> missing;
LoadOrder<IModListingGetter<ISkyrimModGetter>> loadOrder;
ILinkCache cache;
try
{
resolved = Mo2LoadOrder.Resolve(mo2Root, profile, out missing);
// Self-exclusion: never read our own output — the source tree must be what
// the load order looks like WITHOUT the patch, or re-runs would compound.
var dropped = resolved.RemoveAll(r =>
    r.Name.Equals("MAO - Patch.esp", StringComparison.OrdinalIgnoreCase));
if (dropped > 0) Console.WriteLine($"excluded {dropped} installed MAO output esp(s) from the read");
foreach (var s in subs)
{
    var fname = Path.GetFileName(s);
    var idx = resolved.FindIndex(r => r.Name.Equals(fname, StringComparison.OrdinalIgnoreCase));
    if (idx < 0) return Commands.Fail($"--sub {fname}: not in load order");
    resolved[idx] = (resolved[idx].Name, Path.GetFullPath(s));
    Console.WriteLine($"substituted {fname} -> {s}");
}
foreach (var a in adds)
{
    resolved.Add((Path.GetFileName(a), Path.GetFullPath(a)));
    Console.WriteLine($"appended {Path.GetFileName(a)}");
}
Console.WriteLine($"{resolved.Count} plugins resolved, {missing.Count} missing");
if (missing.Count > 0)
    Console.WriteLine("  missing: " + string.Join(", ", missing.Take(10)));

var sw = Stopwatch.StartNew();
var listings = new List<IModListingGetter<ISkyrimModGetter>>();
foreach (var (name, path) in resolved)
{
    var mod = SkyrimMod.CreateFromBinaryOverlay(
        new ModPath(ModKey.FromNameAndExtension(name), path),
        SkyrimRelease.SkyrimSE);
    listings.Add(new ModListing<ISkyrimModGetter>(mod, enabled: true));
}
loadOrder = new LoadOrder<IModListingGetter<ISkyrimModGetter>>(listings);
cache = loadOrder.ToImmutableLinkCache();
Console.WriteLine($"load order read in {sw.Elapsed.TotalSeconds:F1}s");
}
catch (Exception ex)
{
    // Inherited from MEO m35 (audit): the load-order build (Resolve, overlay
    // creation, LoadOrder) reads the filesystem and is the MOST likely thing to
    // throw in the field (bad plugins.txt, missing modlist.txt, duplicate/corrupt
    // esp). Keep it INSIDE a try/catch so a double-clicked Wine console never
    // dies silently.
    Console.Error.WriteLine("LOAD ORDER READ FAILED — MAO cannot continue:");
    Console.Error.WriteLine(ex.ToString());
    return 1;
}

int rc;
try
{
    rc = cmd switch
    {
        "stats" => Commands.Stats(loadOrder),
        "tree" => Commands.DumpTree(loadOrder, cache, positional.ElementAtOrDefault(0) ?? "AVAlchemy"),
        "tree-effects" => Commands.DumpTreeEffects(loadOrder, cache, positional.ElementAtOrDefault(0) ?? "AVAlchemy"),
        "perk" => Commands.DumpPerk(loadOrder, cache, positional[0]),
        "write-tiers" => Commands.WriteTiers(loadOrder, cache, positional.ElementAtOrDefault(0) ?? "data/mao_tiers.json"),
        "write-patch" => Commands.WritePatch(loadOrder, cache, positional.ElementAtOrDefault(0) ?? "MAO - Patch.esp"),
        _ => Commands.Fail($"unknown command {cmd}"),
    };
}
catch (Exception ex)
{
    Console.Error.WriteLine(ex.ToString());
    rc = 1;
}
return rc;

static partial class Commands
{
    public static int Stats(LoadOrder<IModListingGetter<ISkyrimModGetter>> lo)
    {
        long ench = 0, perk = 0, lvli = 0, avif = 0;
        foreach (var l in lo.ListedOrder)
        {
            var m = l.Mod!;
            ench += m.ObjectEffects.Count;
            perk += m.Perks.Count;
            lvli += m.LeveledItems.Count;
            avif += m.ActorValueInformation.Count;
        }
        Console.WriteLine($"totals: ENCH={ench} PERK={perk} LVLI={lvli} AVIF={avif}");
        return 0;
    }

    public static int DumpTree(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string edid)
    {
        var av = lo.PriorityOrder.ActorValueInformation().WinningOverrides()
            .FirstOrDefault(a => a.EditorID == edid);
        if (av is null) return Fail($"{edid} not found");
        Console.WriteLine($"{edid} winning override: {av.FormKey}");
        Console.WriteLine($"perk tree nodes: {av.PerkTree.Count}");
        foreach (var n in av.PerkTree)
        {
            var perkId = n.Perk.TryResolve(cache, out var p)
                ? $"{p.EditorID} [{p.FormKey}]" : $"<{n.Perk.FormKey}>";
            Console.WriteLine(
                $"  idx={n.Index,3} fnam={(n.FNAM is {} fb ? Convert.ToHexString(fb.ToArray()) : "null")} grid=({n.PerkGridX},{n.PerkGridY})" +
                $" pos=({n.HorizontalPosition:F2},{n.VerticalPosition:F2})" +
                $" skill={n.AssociatedSkill}" +
                $" -> [{string.Join(",", n.ConnectionLineToIndices)}]  {perkId}");
        }
        return 0;
    }

    // Every perk in a skill tree + its NNAM rank chains: what the effects
    // actually do (entry points / abilities) and which perks it requires.
    // Evidence for the keep/replace classifier — never hardcode (prime directive).
    public static int DumpTreeEffects(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string edid)
    {
        var av = lo.PriorityOrder.ActorValueInformation().WinningOverrides()
            .FirstOrDefault(a => a.EditorID == edid);
        if (av is null) return Fail($"{edid} not found");
        foreach (var n in av.PerkTree)
        {
            if (!n.Perk.TryResolve(cache, out var perk)) continue;
            for (var p = perk; p is not null;
                 p = p.NextPerk.TryResolve(cache, out var np) ? np : null)
            {
                // Re-resolve to the WINNING override (NNAM may point at origin).
                if (!cache.TryResolve<IPerkGetter>(p.FormKey, out var w)) break;
                Console.WriteLine($"node {n.Index}: {w.EditorID} '{w.Name}'");
                foreach (var c in w.Conditions)
                    if (c is IConditionFloatGetter cf && cf.Data is IHasPerkConditionDataGetter hp)
                        Console.WriteLine($"    requires: {(hp.Perk.Link.TryResolve(cache, out var rp) ? rp.EditorID : hp.Perk.Link.FormKey.ToString())}");
                foreach (var eff in w.Effects)
                {
                    var kind = eff switch
                    {
                        IPerkEntryPointModifyValueGetter mv => $"entry {mv.EntryPoint} {mv.Modification} {mv.Value}",
                        IPerkEntryPointModifyValuesGetter mvs => $"entry {mvs.EntryPoint}",
                        IPerkEntryPointAddActivateChoiceGetter ac => $"entry {ac.EntryPoint} activate-choice",
                        IPerkEntryPointSelectSpellGetter ss => $"entry {ss.EntryPoint} spell={(ss.Spell.TryResolve(cache, out var sp) ? sp.EditorID : "?")}",
                        IPerkEntryPointSelectTextGetter st => $"entry {st.EntryPoint} text",
                        IPerkEntryPointModifyActorValueGetter mav => $"entry {mav.EntryPoint} av={mav.ActorValue}",
                        IPerkAbilityEffectGetter ab => $"ability {(ab.Ability.TryResolve(cache, out var asp) ? asp.EditorID : "?")}",
                        IPerkQuestEffectGetter q => "quest-stage",
                        _ => eff.GetType().Name,
                    };
                    var nc = (eff as IAPerkEffectGetter)?.Conditions.Count ?? -1;
                    Console.WriteLine($"    {kind} [conds={nc}]");
                    if (eff is IAPerkEffectGetter pe)
                        foreach (var pc in pe.Conditions)
                            foreach (var c in pc.Conditions)
                            {
                                var d = c.Data;
                                var args = string.Join(" ", new[]
                                {
                                    (d as IHasKeywordConditionDataGetter)?.Keyword.Link.TryResolve(cache, out var kw) == true ? kw.EditorID : null,
                                    (d as IEPMagic_SpellHasKeywordConditionDataGetter)?.Keyword.Link.TryResolve(cache, out var kw2) == true ? kw2.EditorID : null,
                                    (d as IWornHasKeywordConditionDataGetter)?.Keyword.Link.TryResolve(cache, out var kw3) == true ? kw3.EditorID : null,
                                    d is IGetIsObjectTypeConditionDataGetter or IEPMagic_SpellHasSkillConditionDataGetter
                                        ? string.Join(",", d.GetType().GetProperties()
                                            .Where(pr => pr.PropertyType.IsEnum || pr.PropertyType == typeof(int) || pr.PropertyType == typeof(uint) || pr.PropertyType == typeof(float))
                                            .Select(pr => $"{pr.Name}={pr.GetValue(d)}"))
                                        : null,
                                }.Where(x => x is not null));
                                var cmp = c is IConditionFloatGetter cff ? $" {cff.CompareOperator} {cff.ComparisonValue}" : "";
                                Console.WriteLine($"        [tab{pc.RunOnTabIndex}] {d.Function} {args}{cmp}");
                            }
                }
                if (p.NextPerk.IsNull) break;
            }
        }
        return 0;
    }

    public static int DumpPerk(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string edid)
    {
        var perk = lo.PriorityOrder.Perk().WinningOverrides()
            .FirstOrDefault(p => p.EditorID == edid);
        if (perk is null) return Fail($"{edid} not found");
        Console.WriteLine($"{perk.EditorID} [{perk.FormKey}] '{perk.Name}'");
        Console.WriteLine($"  playable={perk.Playable} trait={perk.Trait} level={perk.Level}" +
                          $" numRanks={perk.NumRanks} hidden={perk.Hidden}");
        var next = perk.NextPerk.TryResolve(cache, out var np)
            ? $"{np.EditorID} [{np.FormKey}]" : perk.NextPerk.FormKeyNullable?.ToString() ?? "null";
        Console.WriteLine($"  nextPerk: {next}");
        foreach (var c in perk.Conditions)
        {
            if (c is IConditionFloatGetter cf)
            {
                var detail = cf.Data is IGetBaseActorValueConditionDataGetter av
                    ? $" av={av.ActorValue}" : $" data={cf.Data}";
                Console.WriteLine($"  cond: {cf.Data.Function} {cf.CompareOperator} {cf.ComparisonValue}" +
                                  $" flags={cf.Flags} runOn={cf.Data.RunOnType}{detail}");
            }
            else
                Console.WriteLine($"  cond: {c}");
        }
        foreach (var eff in perk.Effects)
        {
            var tn = eff.GetType().Name;
            var ep = eff.GetType().GetProperty("EntryPoint")?.GetValue(eff);
            var fn = eff.GetType().GetProperty("Function")?.GetValue(eff);
            Console.WriteLine($"  effect: {tn}" +
                              (ep != null ? $" entryPoint={ep}" : "") +
                              (fn != null ? $" function={fn}" : ""));
        }
        return 0;
    }
}

/// <summary>
/// Resolves an MO2 profile to concrete plugin paths without the VFS:
/// modlist.txt is highest-priority-first (first hit wins), with the
/// Stock Game data folder as final fallback. Case-insensitive on Linux.
/// </summary>
static class Mo2LoadOrder
{
    static readonly string[] BaseMasters =
        ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"];

    // Walk up from the exe (shipped in <MO2>/mods/MAO) to the portable MO2
    // root — the folder holding both ModOrganizer.ini and profiles/.
    public static string? FindRootAbove(string start)
    {
        for (var d = new DirectoryInfo(start); d is not null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "ModOrganizer.ini")) &&
                Directory.Exists(Path.Combine(d.FullName, "profiles")))
                return d.FullName;
        return null;
    }

    // ModOrganizer.ini's selected_profile is the default; only ask when the
    // instance has more than one profile.
    public static string? PickProfile(string root)
    {
        var profiles = Directory.EnumerateDirectories(Path.Combine(root, "profiles"))
            .Where(p => File.Exists(Path.Combine(p, "plugins.txt")))
            .Select(Path.GetFileName)
            .OfType<string>()
            .Order()
            .ToList();
        if (profiles.Count == 0) return null;

        var line = File.ReadLines(Path.Combine(root, "ModOrganizer.ini"))
            .FirstOrDefault(l => l.StartsWith("selected_profile=", StringComparison.Ordinal));
        var sel = line?["selected_profile=".Length..].Trim();
        if (sel is not null && sel.StartsWith("@ByteArray(") && sel.EndsWith(')'))
            sel = sel["@ByteArray(".Length..^1];
        var def = profiles.FirstOrDefault(p => p.Equals(sel, StringComparison.OrdinalIgnoreCase))
                  ?? profiles[0];
        if (profiles.Count == 1 || Console.IsInputRedirected)
        {
            Console.WriteLine($"profile: {def}");
            return def;
        }

        Console.WriteLine("profiles:");
        for (int i = 0; i < profiles.Count; i++)
            Console.WriteLine($"  {i + 1}. {profiles[i]}");
        Console.Write($"which profile? [Enter = {def}]: ");
        var a = Console.ReadLine()?.Trim();
        if (string.IsNullOrEmpty(a)) return def;
        if (int.TryParse(a, out var n) && n >= 1 && n <= profiles.Count) return profiles[n - 1];
        return profiles.FirstOrDefault(p => p.Equals(a, StringComparison.OrdinalIgnoreCase)) ?? def;
    }

    // ── Vanilla / non-MO2 compatibility ──────────────────────────────
    // A plain game install has no profiles: the load order is Data/ plus the
    // game's own plugins.txt (%LOCALAPPDATA%/Skyrim Special Edition). The
    // <root> argument may be the game folder (holds Data/Skyrim.esm) and the
    // <profile> argument a plugins.txt path, or "auto" to use the game's own.
    public static bool IsGameRoot(string root) =>
        File.Exists(Path.Combine(root, "Data", "Skyrim.esm"));

    public static string? FindGameRootAbove(string start)
    {
        for (var d = new DirectoryInfo(start); d is not null; d = d.Parent)
            if (IsGameRoot(d.FullName))
                return d.FullName;
        return null;
    }

    public static List<(string Name, string Path)> ResolveGame(
        string gameRoot, string pluginsArg, out List<string> missing)
    {
        var dataDir = Path.Combine(gameRoot, "Data");
        string? pluginsPath = null;
        if (!string.IsNullOrEmpty(pluginsArg) && pluginsArg != "auto" &&
            File.Exists(pluginsArg))
            pluginsPath = pluginsArg;
        else
        {
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            var cand = Path.Combine(local, "Skyrim Special Edition", "plugins.txt");
            if (File.Exists(cand)) pluginsPath = cand;
        }
        var plugins = pluginsPath is null
            ? new List<string>()
            : File.ReadLines(pluginsPath)
                .Where(l => l.StartsWith('*'))
                .Select(l => l[1..].Trim())
                .ToList();
        if (pluginsPath is null)
            Console.WriteLine("no plugins.txt found — base game + DLC + Creation Club only " +
                              "(pass a plugins.txt path as the profile argument)");
        else
            Console.WriteLine($"plugins.txt: {pluginsPath}");
        // Creation Club content loads via Skyrim.ccc (game root), between the
        // base masters and plugins.txt — the base-game AE mechanism, active
        // even with an empty plugins.txt.
        var ccc = new List<string>();
        var cccPath = Path.Combine(gameRoot, "Skyrim.ccc");
        if (File.Exists(cccPath))
        {
            ccc = File.ReadLines(cccPath)
                .Select(l => l.Trim())
                .Where(l => l.Length > 0 && !l.StartsWith('#'))
                .ToList();
            Console.WriteLine($"Skyrim.ccc: {ccc.Count} Creation Club plugin(s)");
        }
        var files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var f in Directory.EnumerateFiles(dataDir))
            files.TryAdd(Path.GetFileName(f), f);
        var outList = new List<(string, string)>();
        missing = [];
        foreach (var p in BaseMasters.Concat(ccc).Concat(plugins)
                     .Distinct(StringComparer.OrdinalIgnoreCase))
        {
            if (files.TryGetValue(p, out var full)) outList.Add((p, full));
            else missing.Add(p);
        }
        return outList;
    }

    public static List<(string Name, string Path)> Resolve(
        string mo2Root, string profile, out List<string> missing)
    {
        if (!File.Exists(Path.Combine(mo2Root, "ModOrganizer.ini")) && IsGameRoot(mo2Root))
            return ResolveGame(mo2Root, profile, out missing);  // vanilla / manual install

        var prof = Path.Combine(mo2Root, "profiles", profile);
        var plugins = File.ReadLines(Path.Combine(prof, "plugins.txt"))
            .Where(l => l.StartsWith('*'))
            .Select(l => l[1..].Trim())
            .ToList();
        var mods = File.ReadLines(Path.Combine(prof, "modlist.txt"))
            .Where(l => l.StartsWith('+') && !l.TrimEnd().EndsWith("_separator"))
            .Select(l => l[1..].Trim());

        var search = mods.Select(m => Path.Combine(mo2Root, "mods", m))
            .Append(Path.Combine(mo2Root, "Stock Game", "Data"));
        var dirMaps = new List<(string Dir, Dictionary<string, string> Files)>();
        foreach (var d in search)
        {
            if (!Directory.Exists(d)) continue;
            var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var f in Directory.EnumerateFiles(d))
                map.TryAdd(Path.GetFileName(f), Path.GetFileName(f));
            dirMaps.Add((d, map));
        }

        // Dedup — MO2 profiles routinely list base masters (*Skyrim.esm) and can
        // double-list a plugin; a duplicate ModKey makes LoadOrder throw.
        var order = BaseMasters.Concat(plugins).Distinct(StringComparer.OrdinalIgnoreCase);
        var outList = new List<(string, string)>();
        missing = [];
        foreach (var p in order)
        {
            var hit = dirMaps.FirstOrDefault(dm => dm.Files.ContainsKey(p));
            if (hit.Dir is not null)
                outList.Add((p, Path.Combine(hit.Dir, hit.Files[p])));
            else
                missing.Add(p);
        }
        return outList;
    }
}
