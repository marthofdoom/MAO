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
// the effect), IngredientUnits, VariantQuality, VariantCost — and, like the
// DLL's LoadLadders, CONSUMES the tiers json's "ladders" + "potionStats"
// sections rather than re-deriving them, so the table shown here is priced
// from the very data the DLL will load.
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
    static double QualityShipped(uint gold, double anchor) =>
        Math.Clamp(gold / anchor, 0.25, 8.0);

    // CANDIDATE: log-ratio. Monotonic in gold value across every effect (so a
    // higher-value rung can never undercut a lower one), but the log tames the
    // heavy tail so a 25x outlier is ~3x cost, not ~20x.
    static double Tot(Cost c) => c.B + 4 * c.C + 12 * c.A;   // tier-weighted essence

    static double LogDiv = 2.0, QMax = 4.0, BaseScale = 1.0;
    static double QualityLog(uint gold, double anchor) =>
        Math.Clamp(1.0 + Math.Log2(Math.Max(1u, gold) / anchor) / LogDiv, 0.4, QMax);

    static Cost CostOf(
        IIngestibleGetter potion, Dictionary<FormKey, (Ing a, Ing b)> recipes,
        ILinkCache cache, double[] tierMedian, double anchor, bool shipped,
        double catGate, double apexGate, double pct, bool apexRung,
        (Ing a, Ing b)? ladderFallback)
    {
        var fc = new Cost();
        if (potion.Effects.Count == 0) return fc;
        uint gold = (uint)Math.Max(0, potion.Value);
        double q = shipped ? QualityShipped(gold, anchor) : QualityLog(gold, anchor);
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
                // NO INGREDIENT CARRIES THIS EFFECT. Requiem's top rungs use
                // potion-only MGEFs (REQ_Alch_RestoreHealthComplete), which the
                // shipped DLL priced off a phantom {5 gold, Base} default pair —
                // then multiplied it by the rung's (large) quality. That phantom
                // WAS the 228B Surpassing flask: 2 x 5 x 5.9 x 1.3.
                // Fall back to the potion's own NAMED LADDER recipe instead: the
                // stronger rung of a family is brewed from that family's reagents.
                if (ladderFallback is null) { if (i > 0) fc.Add(T.Catalyst, 6); continue; }
                r = ladderFallback.Value;
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
            // DESIGN §1/§2 (marth): the guarantees are POSITIONAL, not merely
            // numeric. "Anything over 1.0 quality has Catalyst added even if the
            // ingredients don't show it, and surpassing and the tier below have
            // apex guaranteed the same way."
            //   Catalyst — any above-anchor potion (quality > 1.0 by construction,
            //              since quality is 1.0 at the pool's p15 anchor — "a
            //              real potion, the cheapest you'd actually brew").
            //   Apex     — the top iApexTopRungs rungs of the potion's own ladder,
            //              read from the tiers json's "ladders" section (the
            //              installer-detected index — see Commands.Ladders.cs).
            //              A fixed quality threshold cannot express this: ladders
            //              have different value spreads, so 1.9 catches the top
            //              two of Restore Health and only the top one of Restore
            //              Magicka. Rung position is the design's actual language.
            // Amounts stay FLAT so cost is linear in quality, not quadratic.
            // Surcharges GROW with the rung (marth). Safe now in a way it was not
            // in v1.0.1: quality there was a LINEAR value/median running to 8, so
            // scaling the surcharge on top of a base that also scaled made cost
            // quadratic (the 38x cliff). Quality is now log-scaled and capped at
            // 4.0, so the surcharge span is a bounded ~6..24 Catalyst / 3..12 Apex.
            if (q > catGate) fc.Add(T.Catalyst, Math.Round(6 * q));
            // Positional guarantee, but only for an ABOVE-MEDIAN rung — the top of
            // a junk ladder is still junk. apexGate is an absolute quality fallback
            // for a one-off with no ladder to top.
            if ((apexRung && q > catGate) || q >= apexGate) fc.Add(T.Apex, Math.Round(3 * q));
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
        // --- tier map + ladder index (exactly what the DLL loads) ---
        // The ladders section is the CONTRACT under test: sim-cost consumes the
        // emitted data the way plugin.cpp's LoadLadders does, NOT a
        // reimplementation of the detector — so what this table shows is what
        // the DLL will actually charge.
        var tierByKey = new Dictionary<(string, uint), T>();
        var tierVals = new List<int>[3] { new(), new(), new() };
        var rawLadders = new List<(string Stem, string Signal, int Height, FormKey? Effect,
                                   List<(FormKey Fk, uint Value, int Rung)> Rungs)>();
        bool hasLadderData = false;
        int emittedAnchor = 0;   // potionStats.anchor — the emitted quality reference (p15, no-food)
        if (File.Exists(tiersPath))
        {
            static FormKey Fk(System.Text.Json.JsonElement e) => new(
                ModKey.FromNameAndExtension(e.GetProperty("plugin").GetString()!),
                Convert.ToUInt32(e.GetProperty("fid").GetString()!.Replace("0x", ""), 16));
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
            if (doc.RootElement.TryGetProperty("potionStats", out var ps) &&
                ps.TryGetProperty("anchor", out var pa) && pa.GetInt32() > 0)
                emittedAnchor = pa.GetInt32();
            if (doc.RootElement.TryGetProperty("ladders", out var la))
            {
                hasLadderData = true;
                foreach (var l in la.EnumerateArray())
                {
                    var rungs = l.GetProperty("rungs").EnumerateArray()
                        .Select(r => (Fk(r), (uint)r.GetProperty("value").GetInt32(),
                                      r.GetProperty("rung").GetInt32())).ToList();
                    rawLadders.Add((l.GetProperty("stem").GetString() ?? "?",
                                    l.GetProperty("signal").GetString() ?? "?",
                                    l.GetProperty("height").GetInt32(),
                                    l.TryGetProperty("effect", out var ef) ? Fk(ef) : null,
                                    rungs));
                }
            }
        }
        else return Fail($"tier map not found: {tiersPath}");
        if (!hasLadderData)
            Console.WriteLine("WARNING: tiers json has NO ladder data (pre-1.0.2 file) — " +
                "positional Apex guarantees and family-recipe fallbacks are OFF " +
                "(this mirrors the DLL's DEGRADED path; re-run write-tiers)");
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

        // --- potion pool: quality anchor + percentile rank ---
        var potions = lo.PriorityOrder.Ingestible().WinningOverrides()
            .Where(p => p.Value > 0 && p.Effects.Count > 0).ToList();
        var vals = potions.Select(p => (uint)p.Value).OrderBy(v => v).ToList();
        // The quality ANCHOR (quality 1.0 = this gold value): the EMITTED
        // potionStats.anchor when the json carries one — the DLL consumes
        // exactly that (p15 of the no-food potion pool, see Commands.Tiers.cs).
        // Fallback mirrors the DLL's DEGRADED mode: the SAME percentile over
        // the SAME pool (no food, no MAO forms), so ACTIVE and DEGRADED can no
        // longer disagree by 5x the way emitted-median vs runtime-median did.
        var nfVals = potions.Where(p => !p.Flags.HasFlag(Ingestible.Flag.FoodItem))
            .Where(p => !p.FormKey.ModKey.Name.StartsWith("MAO", StringComparison.OrdinalIgnoreCase))
            .Select(p => (uint)p.Value).OrderBy(v => v).ToList();
        double NfPctl(double f) => nfVals.Count == 0 ? 1
            : Math.Max(1, nfVals[Math.Clamp((int)(nfVals.Count * f), 0, nfVals.Count - 1)]);
        double anchor = emittedAnchor > 0 ? emittedAnchor : NfPctl(0.15);
        double Pct(uint v)
        {
            int lo_ = 0, hi = vals.Count;
            while (lo_ < hi) { int m = (lo_ + hi) / 2; if (vals[m] < v) lo_ = m + 1; else hi = m; }
            return (double)lo_ / vals.Count;
        }

        // Materialize the emitted ladder index the way the DLL's LoadLadders
        // does: Apex position = the top min(iApexTopRungs=2, height-1) rungs;
        // family recipe = the ladder's representative MGEF looked up in the
        // recipe table built above.
        var apexRung = new HashSet<FormKey>();
        var ladderRecipe = new Dictionary<FormKey, (Ing a, Ing b)>();
        var ladderPos = new Dictionary<FormKey, (int Rung, int Height)>();
        foreach (var (_, _, height, effect, rungs) in rawLadders)
        {
            if (height < 2) continue;
            (Ing, Ing)? fam = effect is { } efk && recipes.TryGetValue(efk, out var fr)
                ? fr : null;
            foreach (var (fk, _, rung) in rungs)
            {
                ladderPos[fk] = (rung, height);
                if (rung >= height - Math.Min(2, height - 1)) apexRung.Add(fk);
                if (fam is not null) ladderRecipe[fk] = fam.Value;
            }
        }

        Console.WriteLine($"pool {vals.Count} ingestibles; quality anchor {anchor}" +
            $"{(emittedAnchor > 0 ? $" (emitted p15; degraded would compute {NfPctl(0.15)})" : " (computed p15 — json has no anchor)")}" +
            $"; no-food pool p50 {NfPctl(0.50)}; " +
            $"tier medians B={tierMedian[0]} C={tierMedian[1]} A={tierMedian[2]}");
        Console.WriteLine($"gates: catalyst>p{catGate * 100:0}, apex>p{apexGate * 100:0}   (x3 = a full 3-charge flask)\n");
        Console.WriteLine($"{"potion",-46}{"gold",5}{"pct",6}  {"ladder",-8}{"SHIPPED 1.0.1 (x3)",-26}{"CANDIDATE (x3)",-26}");

        // AUDIT MODE — the regression that would have caught both shipped bugs.
        //
        // Walks every EMITTED ladder (the same data the DLL prices from) rung
        // by rung and counts two failure classes: FLAT (a pricier rung costs
        // the same — integer rounding erased the progression) and CLIFF (a
        // rung costs more than 6x the one below — a discontinuity the player
        // reads as a bug). Pairs (height 2) are excluded to keep the counts
        // comparable with the pre-1.0.2 audit, which required 3+ rungs.
        if (needle == "*")
        {
            var potionByKey = potions.ToDictionary(p => p.FormKey, p => p);
            int flat = 0, cliff = 0, steps = 0, ladders = 0, apexPotions = 0;
            var signalCount = new Dictionary<string, int>();
            double worstCliff = 0; string worstName = "", flatName = "";
            foreach (var (stem, signal, height, _, rungList) in rawLadders)
            {
                foreach (var s in signal.Split('+'))
                    signalCount[s] = signalCount.GetValueOrDefault(s) + 1;
                apexPotions += rungList.Count(r => apexRung.Contains(r.Fk));
                if (height < 3) continue;        // a real ladder, not a pair
                // One representative per rung, lowest first.
                var reps = rungList.GroupBy(r => r.Rung).OrderBy(g => g.Key)
                    .Select(g => g.First()).ToList();
                if (reps.Any(r => !potionByKey.ContainsKey(r.Fk))) continue;
                ladders++;
                double prev = -1;
                foreach (var r in reps)
                {
                    var p = potionByKey[r.Fk];
                    double tot = Tot(CostOf(p, recipes, cache, tierMedian, anchor, shipped,
                                            catGate, apexGate, Pct((uint)p.Value), apexRung.Contains(r.Fk),
                                            ladderRecipe.TryGetValue(r.Fk, out var lr) ? lr : null));
                    if (prev >= 0)
                    {
                        steps++;
                        if (tot <= prev + 0.01)
                        {
                            flat++;
                            if (flatName == "") flatName = $"{stem} @ {p.Value}g";
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
            Console.WriteLine($"ladder data: {rawLadders.Count} emitted " +
                $"({string.Join(", ", signalCount.OrderByDescending(kv => kv.Value).Select(kv => $"{kv.Key} {kv.Value}"))}); " +
                $"{apexPotions} apex-guaranteed potions ({100.0 * apexPotions / Math.Max(1, potions.Count):0.0}% of pool)");
            Console.WriteLine($"{(shipped ? "SHIPPED " : "CANDIDATE")}: {ladders} ladders (3+ rungs), {steps} steps -> " +
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
            var fb = ladderRecipe.TryGetValue(p.FormKey, out var lr2) ? lr2 : ((Ing, Ing)?)null;
            bool ap = apexRung.Contains(p.FormKey);
            var s = CostOf(p, recipes, cache, tierMedian, anchor, true, catGate, apexGate, pct, ap, fb);
            var c = CostOf(p, recipes, cache, tierMedian, anchor, false, catGate, apexGate, pct, ap, fb);
            var s3 = new Cost { B = s.B * 3, C = s.C * 3, A = s.A * 3 };
            var c3 = new Cost { B = c.B * 3, C = c.C * 3, A = c.A * 3 };
            var nm = p.Name?.String ?? p.EditorID ?? "?";
            if (nm.Length > 45) nm = nm[..45];
            // ladder column: rung index / height, * = Apex-guaranteed position.
            var lad = ladderPos.TryGetValue(p.FormKey, out var lp)
                ? $"r{lp.Rung + 1}/{lp.Height}{(ap ? "*" : " ")}" : "";
            Console.WriteLine($"{nm,-46}{g,5}{pct * 100,5:0}%  {lad,-8}{s3,-26}{c3,-26}");
        }
        return 0;
    }
}
