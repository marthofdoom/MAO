using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;

// OFFLINE COST SIMULATOR.
//
// Replays the DLL's VariantCost over a REAL load order so a pricing change can
// be read as a table BEFORE it is built into the plugin. This exists because
// v1.0.1 shipped a pricing model that was validated on the ingredient side only:
// the potion side was never printed against an actual quality ladder, and it
// turned out Requiem's Restore Health rungs (20/40/60/80/100/500 gold) all
// priced identically at the bottom and 38x at the top. That table would have
// taken one command to see. Now it does.
//
// Mirrors plugin.cpp exactly: BuildRecipeTable (2 cheapest ingredients sharing
// the effect), IngredientUnits, VariantQuality, VariantCost.
static partial class Commands
{
    enum T { Base = 0, Catalyst = 1, Apex = 2 }

    readonly record struct Ing(uint Value, T Tier, string Name);

    struct Cost
    {
        public double B, C, A;
        public void Add(T t, double n)
        {
            if (t == T.Base) B += n; else if (t == T.Catalyst) C += n; else A += n;
        }
        public override string ToString() =>
            $"{Math.Round(B),4}B {Math.Round(C),4}C {Math.Round(A),3}A";
    }

    // ---- the models under test -------------------------------------------

    // SHIPPED v1.0.1: linear value/median, clamped. Fails on a heavy-tailed pool
    // — the bottom of every ladder collapses into the floor and the top slams
    // the cap.
    static double QualityShipped(uint gold, double median) =>
        Math.Clamp(gold / median, 0.25, 8.0);

    // CANDIDATE: log-ratio. Monotonic in gold value across every effect (so a
    // higher-value rung can never undercut a lower one), but the log tames the
    // heavy tail so a 25x outlier is ~3x cost, not ~20x.
    static double Tot(Cost c) => c.B + 4 * c.C + 12 * c.A;   // tier-weighted essence

    static double LogDiv = 2.0, QMax = 4.0, BaseScale = 1.0;
    static double QualityLog(uint gold, double median) =>
        Math.Clamp(1.0 + Math.Log2(Math.Max(1u, gold) / median) / LogDiv, 0.4, QMax);

    static Cost CostOf(
        IIngestibleGetter potion, Dictionary<FormKey, (Ing a, Ing b)> recipes,
        ILinkCache cache, double[] tierMedian, double median, bool shipped,
        double catGate, double apexGate, double pct)
    {
        var fc = new Cost();
        if (potion.Effects.Count == 0) return fc;
        uint gold = (uint)Math.Max(0, potion.Value);
        double q = shipped ? QualityShipped(gold, median) : QualityLog(gold, median);
        double basis = q * 1.0 /*costRate*/ * 1.3 /*essenceTax*/;

        double Units(Ing i) => i.Tier == T.Base
            ? i.Value
            : (i.Tier == T.Apex ? 1.0 : 3.0) * (i.Value / Math.Max(1.0, tierMedian[(int)i.Tier]));

        // Primary + same-polarity riders, each priced as its own recipe pair.
        bool PrimaryHostile()
        {
            var m0 = potion.Effects[0].BaseEffect.TryResolve(cache);
            return m0 is not null &&
                   (m0.Flags.HasFlag(MagicEffect.Flag.Hostile) || m0.Flags.HasFlag(MagicEffect.Flag.Detrimental));
        }
        bool hostile0 = PrimaryHostile();
        for (int i = 0; i < potion.Effects.Count; i++)
        {
            var e = potion.Effects[i];
            var m = e.BaseEffect.TryResolve(cache);
            if (m is null) continue;
            if (i > 0)
            {
                bool h = m.Flags.HasFlag(MagicEffect.Flag.Hostile) || m.Flags.HasFlag(MagicEffect.Flag.Detrimental);
                if (h != hostile0) continue;  // off-polarity drawback: free
            }
            if (!recipes.TryGetValue(m.FormKey, out var r))
            {
                if (i > 0) fc.Add(T.Catalyst, 6);
                continue;
            }
            if (shipped)
            {
                // per-INGREDIENT integer floor — this is the flattener: a pair of
                // 1-gold reagents rounds to 1+1 for every quality below ~1.
                fc.Add(r.a.Tier, Math.Max(1, Math.Round(Units(r.a) * basis)));
                fc.Add(r.b.Tier, Math.Max(1, Math.Round(Units(r.b) * basis)));
            }
            else
            {
                // accumulate in FLOAT; round once per tier at the end.
                double sc = BaseScale;
                fc.Add(r.a.Tier, Units(r.a) * basis * (r.a.Tier == T.Base ? sc : 1.0));
                fc.Add(r.b.Tier, Units(r.b) * basis * (r.b.Tier == T.Base ? sc : 1.0));
            }
        }

        if (shipped)
        {
            if (q > 1.0) fc.Add(T.Catalyst, Math.Max(6, Math.Round(6 * q)));
            if (q > 1.2) fc.Add(T.Apex, Math.Max(3, Math.Round(3 * q)));
        }
        else
        {
            // Gates by PERCENTILE of the potion pool (portable across load
            // orders: "top ~30%" means the same thing everywhere), amounts flat
            // so cost stays linear in quality rather than quadratic.
            if (pct > catGate) fc.Add(T.Catalyst, 6);
            if (pct > apexGate) fc.Add(T.Apex, 3);
        }
        if (!shipped)
        {
            fc.B = Math.Max(fc.B > 0 ? 1 : 0, Math.Round(fc.B));
            fc.C = Math.Max(fc.C > 0 ? 1 : 0, Math.Round(fc.C));
            fc.A = Math.Max(fc.A > 0 ? 1 : 0, Math.Round(fc.A));
        }
        return fc;
    }

    public static int SimCost(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache,
        string tiersPath, string needle, double catGate, double apexGate,
        double logDiv, double qMax, double baseScale, bool shipped)
    {
        LogDiv = logDiv; QMax = qMax; BaseScale = baseScale;
        // --- tier map (exactly what the DLL loads) ---
        var tierByKey = new Dictionary<(string, uint), T>();
        var tierVals = new List<int>[3] { new(), new(), new() };
        if (File.Exists(tiersPath))
        {
            using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(tiersPath));
            foreach (var e in doc.RootElement.GetProperty("tiers").EnumerateArray())
            {
                var plugin = e.GetProperty("plugin").GetString() ?? "";
                var fid = Convert.ToUInt32(e.GetProperty("fid").GetString()!.Replace("0x", ""), 16);
                var t = e.GetProperty("tier").GetString() switch
                { "Apex" => T.Apex, "Catalyst" => T.Catalyst, _ => T.Base };
                tierByKey[(plugin.ToLowerInvariant(), fid)] = t;
                tierVals[(int)t].Add(e.GetProperty("value").GetInt32());
            }
        }
        else return Fail($"tier map not found: {tiersPath}");
        var tierMedian = new double[3];
        for (int i = 0; i < 3; i++)
        {
            tierVals[i].Sort();
            tierMedian[i] = tierVals[i].Count > 0 ? tierVals[i][tierVals[i].Count / 2] : new[] { 5.0, 20.0, 60.0 }[i];
        }

        // --- recipe table: 2 cheapest ingredients carrying each effect ---
        var byEffect = new Dictionary<FormKey, List<Ing>>();
        foreach (var ing in lo.PriorityOrder.Ingredient().WinningOverrides())
        {
            if (ing.Value <= 0) continue;
            var key = (ing.FormKey.ModKey.FileName.String.ToLowerInvariant(), ing.FormKey.ID);
            var t = tierByKey.TryGetValue(key, out var tt) ? tt : T.Base;
            var rec = new Ing((uint)ing.Value, t, ing.Name?.String ?? ing.EditorID ?? "?");
            foreach (var e in ing.Effects)
            {
                if (!byEffect.TryGetValue(e.BaseEffect.FormKey, out var l))
                    byEffect[e.BaseEffect.FormKey] = l = new List<Ing>();
                l.Add(rec);
            }
        }
        var recipes = new Dictionary<FormKey, (Ing a, Ing b)>();
        foreach (var (k, v) in byEffect)
        {
            v.Sort((x, y) => x.Value.CompareTo(y.Value));
            recipes[k] = (v[0], v.Count > 1 ? v[1] : v[0]);
        }

        // --- potion pool: median + percentile rank ---
        var potions = lo.PriorityOrder.Ingestible().WinningOverrides()
            .Where(p => p.Value > 0 && p.Effects.Count > 0).ToList();
        var vals = potions.Select(p => (uint)p.Value).OrderBy(v => v).ToList();
        double median = Math.Max(1, vals[vals.Count / 2]);
        double Pct(uint v)
        {
            int lo_ = 0, hi = vals.Count;
            while (lo_ < hi) { int m = (lo_ + hi) / 2; if (vals[m] < v) lo_ = m + 1; else hi = m; }
            return (double)lo_ / vals.Count;
        }

        Console.WriteLine($"pool {vals.Count} potions, median {median}; " +
            $"tier medians B={tierMedian[0]} C={tierMedian[1]} A={tierMedian[2]}");
        Console.WriteLine($"gates: catalyst>p{catGate * 100:0}, apex>p{apexGate * 100:0}   (x3 = a full 3-charge flask)\n");
        Console.WriteLine($"{"potion",-46}{"gold",5}{"pct",6}  {"SHIPPED 1.0.1 (x3)",-26}{"CANDIDATE (x3)",-26}");

        // AUDIT MODE — the regression that would have caught both shipped bugs.
        //
        // Grouping by MGEF is useless here: Requiem's top rung uses its OWN
        // effect (REQ_Alch_RestoreHealthComplete), so it was never compared
        // against the rung below it. Grouping by raw gold across the whole pool
        // is also wrong: cost legitimately depends on which reagents an effect
        // uses, so a cheap potion built from Apex reagents SHOULD cost more.
        //
        // What a player actually perceives is the NAMED LADDER — "Potion of
        // Restore Health (Diluted..Surpassing)". Strip the parenthetical rank and
        // every rung of one ladder lands in one group, across MGEF boundaries.
        // Two failures to count: FLAT (a pricier rung costs the same) and CLIFF
        // (a rung costs more than 6x the one below).
        if (needle == "*")
        {
            static string Stem(string n)
            {
                int i = n.LastIndexOf('(');
                return (i > 0 ? n[..i] : n).Trim();
            }
            int flat = 0, cliff = 0, steps = 0, ladders = 0;
            double worstCliff = 0; string worstName = "", flatName = "";
            foreach (var grp in potions
                .Where(p => !p.FormKey.ModKey.FileName.String.StartsWith("MAO", StringComparison.OrdinalIgnoreCase))
                .Where(p => !string.IsNullOrEmpty(p.Name?.String))
                .GroupBy(p => Stem(p.Name!.String!)))
            {
                var rungs = grp.GroupBy(p => (uint)p.Value).Select(g => g.First())
                               .OrderBy(p => p.Value).ToList();
                if (rungs.Count < 3) continue;   // a real ladder, not a pair
                ladders++;
                double prev = -1;
                foreach (var p in rungs)
                {
                    double tot = Tot(CostOf(p, recipes, cache, tierMedian, median, shipped,
                                            catGate, apexGate, Pct((uint)p.Value)));
                    if (prev >= 0)
                    {
                        steps++;
                        if (tot <= prev + 0.01)
                        {
                            flat++;
                            if (flatName == "") flatName = $"{Stem(p.Name!.String!)} @ {p.Value}g";
                        }
                        else if (tot > prev * 6)
                        {
                            cliff++;
                            if (tot / Math.Max(1, prev) > worstCliff)
                            { worstCliff = tot / Math.Max(1, prev); worstName = $"{p.Name?.String} ({prev:0} -> {tot:0})"; }
                        }
                    }
                    prev = tot;
                }
            }
            Console.WriteLine($"{(shipped ? "SHIPPED " : "CANDIDATE")}: {ladders} named ladders, {steps} steps -> " +
                $"{flat} FLAT ({100.0 * flat / Math.Max(1, steps):0}%), {cliff} CLIFF ({100.0 * cliff / Math.Max(1, steps):0}%)");
            if (flat > 0) Console.WriteLine($"   first flat: {flatName}");
            if (cliff > 0) Console.WriteLine($"   worst cliff: {worstName}  = {worstCliff:0.0}x");
            return 0;
        }

        foreach (var p in potions
            .Where(p => (p.Name?.String ?? "").Contains(needle, StringComparison.OrdinalIgnoreCase)
                     || (p.EditorID ?? "").Contains(needle, StringComparison.OrdinalIgnoreCase))
            .OrderBy(p => p.Value).Take(40))
        {
            uint g = (uint)p.Value;
            double pct = Pct(g);
            var s = CostOf(p, recipes, cache, tierMedian, median, true, catGate, apexGate, pct);
            var c = CostOf(p, recipes, cache, tierMedian, median, false, catGate, apexGate, pct);
            var s3 = new Cost { B = s.B * 3, C = s.C * 3, A = s.A * 3 };
            var c3 = new Cost { B = c.B * 3, C = c.C * 3, A = c.A * 3 };
            var nm = p.Name?.String ?? p.EditorID ?? "?";
            if (nm.Length > 45) nm = nm[..45];
            Console.WriteLine($"{nm,-46}{g,5}{pct * 100,5:0}%  {s3,-26}{c3,-26}");
        }
        return 0;
    }
}
