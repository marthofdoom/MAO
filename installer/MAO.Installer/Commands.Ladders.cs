// MAO shared named-ladder detector. Shared with MAO.Synthesis via
// <Compile Include> (the Commands.Tiers.cs pattern) so both patchers emit
// byte-identical ladder data for the same load order.
//
// A LADDER is one potion family's quality progression — the thing a player
// perceives as "the same potion, better": Requiem's
//   Potion of Restore Health (Diluted / Faint / Fair / Good / Remarkable /
//   Surpassing)                                   20/40/60/80/100/500 gold
// or vanilla's
//   Potion of Minor/-/Plentiful/Vigorous/Extreme/Ultimate Healing
//                                                 17/73/57/79/123/251 gold.
//
// The DLL needs ladders for exactly two things (DESIGN §1/§2):
//   (a) the top iApexTopRungs rungs of a family are APEX-GUARANTEED
//       ("surpassing and the tier below have apex guaranteed") — positional,
//       so no numeric quality threshold can express it; and
//   (b) a rung whose own primary MGEF has NO ingredient source (Requiem's
//       potion-only REQ_Alch_RestoreHealthComplete) borrows its FAMILY's
//       reagent pair instead of a phantom {5 gold, Base} default.
// This knowledge belongs HERE, at patch time over the whole load order — not
// in a runtime string heuristic in the DLL (marth). The result is emitted into
// mao_tiers.json ("ladders") and the DLL only consumes it.
//
// WHY a multi-signal grouping (each alone demonstrably fails):
//   - MGEF grouping: Requiem's top rung deliberately uses its OWN effect no
//     ingredient or other potion shares — the original inversion bug.
//   - parenthetical stripping only: vanilla ranks by ADJECTIVE ("Potion of
//     Ultimate Healing"), no parenthetical, so every rung reads singleton and
//     the Apex guarantee silently never fires on a vanilla-ish list.
//   - raw name grouping: four separate Requiem "Healing Poultice" forms, all
//     4 gold, are ONE item — not a 4-rung ladder (distinct-VALUE guard).
//
// SIGNALS combined here, per potion:
//   paren   — trailing "(rank)" stripped: "Potion of X (Good)" -> "potion of x"
//   rank    — a known rank-adjective token removed: "Potion of Minor Healing"
//             -> "~ of healing" (vocabulary below; unknown adjectives fall
//             through to the edid signal)
//   numeral — a trailing roman/arabic numeral token dropped ("Health Potion II")
//   vessel  — the vessel noun normalized to "~" ("Draught/Philter/Elixir of X"
//             — CACO-style noun-swap ranks — and trailing "X Potion/Elixir")
//   edid    — EditorID with trailing digits stripped (RestoreHealth01..06 is
//             the strongest vanilla signal); union-merged with the name axis
//   name    — identical display names with different gold values (leveled
//             variants the player sees as one item)
// Groups from the name axis and the edid axis are UNION-MERGED (either signal
// linking two potions puts them in one family), then every candidate family
// must pass VALIDATION before it becomes a ladder:
//   - same polarity (a potion family and a poison family never merge);
//   - >= 2 distinct gold values (the Healing Poultice guard);
//   - effect-archetype coherence: the modal (archetype, actor value) of the
//     members' primary MGEFs must cover at least HALF the family. Requiem's
//     odd top rung passes (5/6 share ValueModifier/Health — and the odd MGEF
//     still modifies Health); an accidental name collision of unrelated
//     potions does not.
// Rungs are ranked by GOLD VALUE — the same load-order-authored metric
// VariantQuality prices from, so "top rung" and "highest quality" can never
// disagree (vanilla's name order even disagrees with its value order:
// Plentiful 57g < Healing 73g — gold is the authority, by design).
// Failure mode by construction: a list with no real ladders degrades to
// few/no groups (singletons everywhere) — it must never INVENT ladders.

using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Skyrim;

static partial class Commands
{
    public sealed record LadderRung(IIngestibleGetter Potion, uint Value, int Rung);
    public sealed record Ladder(
        string Stem, string Signal, FormKey? RepEffect, int Height, List<LadderRung> Rungs);

    // Rank adjectives seen across the major alchemy conventions (vanilla,
    // Requiem, CACO/Apothecary-style mods). Token-exact, lowercase — never
    // substring (ANTI_PATTERNS: name lists die; this one is only a GROUPING
    // hint that validation must confirm, so an unknown word merely means the
    // edid signal has to carry that list).
    static readonly HashSet<string> RankWords = new(StringComparer.Ordinal)
    {
        // vanilla potion line
        "minor", "plentiful", "vigorous", "extreme", "ultimate",
        // vanilla poison line
        "weak", "potent", "virulent", "deadly",
        // Requiem parenthetical ranks (also seen bare in mods)
        "diluted", "faint", "fair", "good", "remarkable", "surpassing",
        // common mod vocabulary
        "lesser", "greater", "grand", "major", "superior", "supreme",
        "concentrated", "pure", "refined", "improved", "strong", "powerful",
        "mild", "weakened", "fine", "exceptional", "exquisite", "perfect",
        "legendary", "masterful", "crude", "simple", "small", "large",
    };

    // Vessel nouns: the container word players read past. Leading "<vessel> of
    // X" and trailing "X <vessel>" both normalize the noun to "~" so CACO-style
    // noun-swap ranks (Draught -> Potion -> Philter -> Elixir of X) group.
    static readonly HashSet<string> VesselWords = new(StringComparer.Ordinal)
    {
        "potion", "draught", "philter", "philtre", "elixir", "tonic", "brew",
        "mixture", "concoction", "solution", "tincture", "decoction",
        "infusion", "extract", "phial", "vial", "flask", "bottle", "dose",
    };

    static readonly HashSet<string> RomanNumerals = new(StringComparer.Ordinal)
    { "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x" };

    // One potion's normalized family stem + which signals fired producing it.
    readonly record struct StemInfo(
        string Stem, bool Paren, bool Rank, bool Numeral, string? Vessel);

    static StemInfo StemOf(string name)
    {
        var s = name.Trim();
        bool paren = false, rank = false, numeral = false;
        string? vessel = null;
        // Trailing parenthetical = a rank marker in Requiem's convention.
        // Stripped unconditionally — a flavor parenthetical that merges two
        // unrelated potions is caught by the validation gauntlet, not here.
        if (s.EndsWith(')'))
        {
            int open = s.LastIndexOf('(');
            if (open > 0) { s = s[..open].TrimEnd(); paren = true; }
        }
        var toks = s.ToLowerInvariant()
            .Split(' ', StringSplitOptions.RemoveEmptyEntries).ToList();
        // Trailing numeral rank ("Health Potion II", "Potion 3").
        if (toks.Count > 1 &&
            (RomanNumerals.Contains(toks[^1]) || toks[^1].All(char.IsDigit)))
        { toks.RemoveAt(toks.Count - 1); numeral = true; }
        // Vessel noun -> "~" (leading "<vessel> of ..." or trailing "... <vessel>").
        if (toks.Count > 2 && toks[1] == "of" && VesselWords.Contains(toks[0]))
        { vessel = toks[0]; toks[0] = "~"; }
        else if (toks.Count > 1 && VesselWords.Contains(toks[^1]))
        { vessel = toks[^1]; toks[^1] = "~"; }
        // Rank adjectives out, wherever they sit ("Potion of Minor Healing",
        // "Weak Lingering Poison").
        int removed = toks.RemoveAll(t => RankWords.Contains(t));
        if (removed > 0) rank = true;
        // A stem must retain a real word — "~"/"of" alone would collapse every
        // vessel in the load order into one absurd mega-family.
        if (!toks.Any(t => t != "~" && t != "of"))
            return new StemInfo("", paren, rank, numeral, vessel);
        return new StemInfo(string.Join(' ', toks), paren, rank, numeral, vessel);
    }

    // EditorID axis: trailing digits stripped (RestoreHealth01..06). Only
    // meaningful when digits WERE stripped — a full unique edid groups nothing.
    static string? EdidStemOf(string? edid)
    {
        if (string.IsNullOrEmpty(edid)) return null;
        int end = edid.Length;
        while (end > 0 && char.IsDigit(edid[end - 1])) end--;
        if (end == edid.Length) return null;          // no digits: no signal
        var stem = edid[..end].TrimEnd('_', '-');
        return stem.Length >= 4 ? stem.ToLowerInvariant() : null;
    }

    public static List<Ladder> DetectLadders(
        IReadOnlyList<IIngestibleGetter> potions, ILinkCache cache,
        IReadOnlySet<FormKey> sourcedEffects, List<string>? rejects = null)
    {
        // Per-potion candidate: stem + signals + resolved primary-effect facts.
        var cands = new List<(IIngestibleGetter P, StemInfo S, string? EdidKey,
                              bool Hostile, string Sig)>();
        foreach (var p in potions)
        {
            var nm = p.Name?.String;
            if (string.IsNullOrWhiteSpace(nm)) continue;   // not player-facing
            if (p.Effects.Count == 0) continue;
            var m0 = p.Effects[0].BaseEffect.TryResolve(cache);
            if (m0 is null) continue;                      // dangling link: skip, never guess
            var stem = StemOf(nm);
            if (stem.Stem.Length == 0) continue;
            bool hostile = m0.Flags.HasFlag(MagicEffect.Flag.Hostile) ||
                           m0.Flags.HasFlag(MagicEffect.Flag.Detrimental);
            // Coherence signature: WHAT the primary effect does + to WHICH stat.
            // Requiem's odd top-rung MGEF keeps (archetype, actor value) even
            // though the MGEF itself is unique — that is what lets it group.
            var sig = $"{m0.Archetype.Type}/{m0.Archetype.ActorValue}";
            cands.Add((p, stem, EdidStemOf(p.EditorID), hostile, sig));
        }

        // Union-find over the two grouping axes. Polarity is part of the name
        // key so a potion family and a poison family can never merge even
        // under an aggressive stem.
        var parent = Enumerable.Range(0, cands.Count).ToArray();
        int Find(int i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; }
        void Union(int a, int b) { a = Find(a); b = Find(b); if (a != b) parent[b] = a; }
        var byName = new Dictionary<string, int>();
        var byEdid = new Dictionary<string, int>();
        for (int i = 0; i < cands.Count; i++)
        {
            var nameKey = $"{(cands[i].Hostile ? "!" : "+")}{cands[i].S.Stem}";
            if (byName.TryGetValue(nameKey, out var j)) Union(i, j); else byName[nameKey] = i;
            if (cands[i].EdidKey is { } ek)
            {
                var edidKey = $"{(cands[i].Hostile ? "!" : "+")}{ek}";
                if (byEdid.TryGetValue(edidKey, out var k)) Union(i, k); else byEdid[edidKey] = i;
            }
        }
        var groups = new Dictionary<int, List<int>>();
        for (int i = 0; i < cands.Count; i++)
        {
            if (!groups.TryGetValue(Find(i), out var l)) groups[Find(i)] = l = new List<int>();
            l.Add(i);
        }

        // ── VALIDATION GAUNTLET: a group only becomes a ladder if it looks
        // like one. Rejections degrade to "no positional guarantee", which is
        // the safe direction — the absolute fApexQuality line still exists.
        var ladders = new List<Ladder>();
        foreach (var g in groups.Values)
        {
            var members = g.Where(i => cands[i].P.Value > 0).ToList();  // value-0: unrankable
            if (members.Count < 2) continue;
            var stem = cands[members[0]].S.Stem;
            // Distinct VALUES, not distinct records (the Healing Poultice guard:
            // four identical 4-gold forms are one item, not a 4-rung ladder).
            var distinct = members.Select(i => (uint)cands[i].P.Value).Distinct().Count();
            if (distinct < 2) continue;
            // Oversized family = a stem collision, not a design (no author
            // ships a 30-rung progression of one potion).
            if (distinct > 30)
            { rejects?.Add($"'{stem}': {distinct} rungs — collision, rejected"); continue; }
            // Effect coherence: the modal primary-effect signature must cover
            // at least half the family. This is what stops a name collision of
            // unrelated potions from becoming a ladder, while still admitting
            // per-rank MGEF families (same archetype + actor value throughout).
            var modal = members.GroupBy(i => cands[i].Sig).OrderByDescending(x => x.Count()).First();
            if (modal.Count() * 2 < members.Count)
            {
                rejects?.Add($"'{stem}': incoherent effects " +
                    $"({string.Join(", ", members.GroupBy(i => cands[i].Sig).Select(x => $"{x.Key} x{x.Count()}"))}) — rejected");
                continue;
            }
            // Rungs ranked by GOLD — the metric quality prices from, so the
            // guarantee and the price can never disagree about "top".
            var byValue = members.GroupBy(i => (uint)cands[i].P.Value)
                                 .OrderBy(x => x.Key).ToList();
            var rungs = new List<LadderRung>();
            for (int r = 0; r < byValue.Count; r++)
                foreach (var i in byValue[r])
                    rungs.Add(new LadderRung(cands[i].P, byValue[r].Key, r));
            // Family recipe = the lowest rung whose primary MGEF has a real
            // ingredient source (the cheap rungs use ordinary reagents; the top
            // rungs are where the potion-only MGEFs live).
            FormKey? repEffect = null;
            foreach (var r in rungs)
            {
                var fk = r.Potion.Effects[0].BaseEffect.FormKey;
                if (sourcedEffects.Contains(fk)) { repEffect = fk; break; }
            }
            // Which signal(s) actually did the linking — diagnostics for the
            // audit ("this list groups by edid" is a finding, not noise).
            var sigs = new List<string>();
            if (members.Any(i => cands[i].S.Paren)) sigs.Add("paren");
            if (members.Any(i => cands[i].S.Rank)) sigs.Add("rank");
            if (members.Any(i => cands[i].S.Numeral)) sigs.Add("numeral");
            if (members.Select(i => cands[i].S.Vessel).Where(v => v is not null).Distinct().Count() > 1)
                sigs.Add("vessel");
            if (members.Select(i => $"{(cands[i].Hostile ? "!" : "+")}{cands[i].S.Stem}").Distinct().Count() > 1)
                sigs.Add("edid");                       // only the edid axis could have merged these
            if (sigs.Count == 0) sigs.Add("name");      // identical names, distinct values
            ladders.Add(new Ladder(stem, string.Join("+", sigs), repEffect,
                                   byValue.Count, rungs));
        }
        return ladders.OrderBy(l => l.Stem, StringComparer.Ordinal).ToList();
    }
}
