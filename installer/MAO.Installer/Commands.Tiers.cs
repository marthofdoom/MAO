// MAO shared tier-classifier code. Extracted from Program.cs so the Synthesis
// patcher (installer/MAO.Synthesis) can reuse the EXACT same logic — the tiers
// JSON it writes is byte-identical to the standalone installer's for the same
// load order (MEO's Commands.cs sharing pattern).

using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;

static partial class Commands
{
    public static int Fail(string msg)
    {
        Console.Error.WriteLine(msg);
        return 1;
    }

    // Rarity classifier (MEO's WriteCalibration analogue): read the winning
    // load order, score every ingredient by AVAILABILITY — how obtainable it is
    // in THIS modlist, NOT its gold value — and write the tiers JSON the DLL
    // loads. Higher score = more common = Base; score 0 (creature-drops / quest
    // items with no world source) = scarcest = Apex.
    //
    // Signals, summed per INGR FormKey over WINNING overrides:
    //   FLOR/TREE .Ingredient  ×3  — harvestable from the world is the strongest
    //                                availability signal (pick it in the field).
    //   CONT .Items[].Item.Item ×1 — vendor chests, barrels, world containers.
    //   LVLI .Entries[].Data.Reference ×1 — loot tables.
    public static int WriteTiers(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string outPath)
    {
        // Ingredients worth classifying: one entry per WINNING INGR override,
        // with a real name. INGR has no "Playable" record flag (unlike ARMO/
        // WEAP); a reagent that carries a real Name IS a player-facing,
        // obtainable ingredient, so a non-empty Name is the playable filter
        // here. Nameless/templated/deleted stubs fall out naturally.
        var ingr = new Dictionary<FormKey, IIngredientGetter>();
        foreach (var i in lo.PriorityOrder.Ingredient().WinningOverrides())
            if (!string.IsNullOrWhiteSpace(i.Name?.String))
                ingr[i.FormKey] = i;
        if (ingr.Count == 0) return Fail("no named INGR records in the load order");

        // Raw reference counts per signal (kept separate so the availability
        // score is a tunable function of them, not a fixed sum).
        var harvest = ingr.Keys.ToDictionary(k => k, _ => 0);  // FLOR + TREE producers
        var cont = ingr.Keys.ToDictionary(k => k, _ => 0);     // CONT static placements
        var lvli = ingr.Keys.ToDictionary(k => k, _ => 0);     // LVLI loot-table entries
        int floraHits = 0, treeHits = 0, contHits = 0, lvliHits = 0;

        foreach (var f in lo.PriorityOrder.Flora().WinningOverrides())
            if (!f.Ingredient.IsNull && harvest.ContainsKey(f.Ingredient.FormKey))
            { harvest[f.Ingredient.FormKey]++; floraHits++; }
        foreach (var t in lo.PriorityOrder.Tree().WinningOverrides())
            if (!t.Ingredient.IsNull && harvest.ContainsKey(t.Ingredient.FormKey))
            { harvest[t.Ingredient.FormKey]++; treeHits++; }
        foreach (var c in lo.PriorityOrder.Container().WinningOverrides())
            foreach (var e in c.Items ?? [])
            {
                var fk = e.Item.Item.FormKey;
                if (cont.ContainsKey(fk)) { cont[fk]++; contHits++; }
            }
        foreach (var l in lo.PriorityOrder.LeveledItem().WinningOverrides())
            foreach (var e in l.Entries ?? [])
                if (e.Data is { } d && lvli.ContainsKey(d.Reference.FormKey))
                { lvli[d.Reference.FormKey]++; lvliHits++; }
        var loot = ingr.Keys.ToDictionary(k => k, k => cont[k] + lvli[k]);

        // OBTAINABILITY. How reliably you can put the ingredient in your hands:
        //   harvest ×60  FLOR/TREE producer — farmable from the world forever;
        //                the decisive availability signal.
        //   cont    ×20  static container placement (barrels, vendor chests) —
        //                a reliable, repeatable place to grab it.
        //   lvli    ×4   leveled-list entry — RNG loot; being in many lists is
        //                weak evidence you will ever get the drop, so it is
        //                CAPPED (min(lvli,LvliCap)) so a widely-listed reagent
        //                (Vampire Dust: 17 lists in LoreRim) can't out-score a
        //                farmable one.
        const int FloraWeight = 60, ContWeight = 20, LvliWeight = 4, LvliCap = 3;
        var obtain = ingr.Keys.ToDictionary(
            k => k, k => harvest[k] * FloraWeight + cont[k] * ContWeight
                       + Math.Min(lvli[k], LvliCap) * LvliWeight);

        // GOLD VALUE (INGR base value). A premium reagent reads as "worth
        // seeking" even when not farmable; a cheap one does not, no matter how
        // scarce. This is the co-factor that keeps cheap-but-unfarmable junk
        // (Blue Dartwing 15g, antlers, pearls) OUT of Apex and lets the truly
        // premium drops (Daedra Heart 250g, Void Salts, Vampire Dust) IN.
        var value = ingr.Keys.ToDictionary(k => k, k => (int)ingr[k].Value);

        // HARD RULE (marth 2026-07-13): an ingredient with NO source at all —
        // no flora/tree, no container, no leveled list — cannot be gathered.
        // These are quest/templated leftovers (Berit's Ashes). Apex must be
        // *obtainable*, so they go straight to Base and are pulled from the
        // ranked pool entirely.
        bool NoSource(FormKey k) => harvest[k] == 0 && cont[k] == 0 && lvli[k] == 0;
        var pool = ingr.Keys.Where(k => !NoSource(k)).ToList();
        int orphans = ingr.Count - pool.Count;

        // BLENDED RARITY over the pool. Two independent percentile ranks so the
        // very different scales of "obtain score" and "gold value" combine
        // fairly:
        //   availRank — rarest (lowest obtain) first, 0 = hardest to get.
        //   valueRank — priciest (highest value) first, 0 = most valuable.
        // rarity = 15% "hard to obtain" + 85% "worth a lot"; LOW = Apex-like.
        // VALUE-DOMINANT by design (marth 2026-07-13 calibration): our obtain
        // signal can't see caught fish / stream reagents, so it falsely reads
        // cheap fish (15g) as "rare" and floated them into Apex at 60/40. Gold
        // value is the clean signal — it's what makes Daedra Heart (250g) and the
        // salts read as Apex while 15g fish drop to Catalyst. Availability is only
        // a 15% tiebreak: strong enough to order equally-priced reagents by how
        // farmable they are, but NOT strong enough to demote the single most
        // valuable reagent out of Apex just because it's widely looted (on the
        // 75-CC Deck load Daedra Heart has obtain=112 yet is still Apex at 0.15;
        // at 0.25 it wrongly sank below 30g Curios rares).
        const double AvailW = 0.15, ValueW = 0.85;
        string NameOf(FormKey k) => ingr[k].Name?.String ?? ingr[k].EditorID ?? "";
        var availRank = PercentileRank(pool.OrderBy(k => obtain[k]).ThenBy(NameOf).ToList());
        var valueRank = PercentileRank(pool.OrderByDescending(k => value[k]).ThenBy(NameOf).ToList());
        var rarity = pool.ToDictionary(k => k, k => AvailW * availRank[k] + ValueW * valueRank[k]);

        // RANK-based buckets over the pool. Percentiles keep tier sizes stable no
        // matter the load-order size. Apex = rarest+priciest 12%, Catalyst = next
        // 30%, Base = the common ~58% (plus every 0-source orphan).
        var order = pool.OrderBy(k => rarity[k]).ThenBy(NameOf).ToList();
        int n = order.Count;
        int apexCut = (int)(n * 0.12);
        int catCut = (int)(n * 0.42);
        var tierByKey = new Dictionary<FormKey, string>(ingr.Count);
        for (int i = 0; i < n; i++)
            tierByKey[order[i]] = i < apexCut ? "Apex" : i < catCut ? "Catalyst" : "Base";
        foreach (var k in ingr.Keys)
            if (!tierByKey.ContainsKey(k)) tierByKey[k] = "Base";  // 0-source orphans
        string Tier(FormKey k) => tierByKey[k];

        // Emit pool in rarity order (Apex first), then orphans, so the JSON reads
        // rarest-to-commonest.
        var emitOrder = order.Concat(ingr.Keys.Where(k => !order.Contains(k))
            .OrderByDescending(k => value[k]).ThenBy(NameOf)).ToList();
        var entries = new List<object>();
        var tierCount = new Dictionary<string, int> { ["Apex"] = 0, ["Catalyst"] = 0, ["Base"] = 0 };
        foreach (var fk in emitOrder)
        {
            var rec = ingr[fk];
            var tier = tierByKey[fk];
            tierCount[tier]++;
            entries.Add(new Dictionary<string, object>
            {
                ["plugin"] = fk.ModKey.FileName.String,
                ["fid"] = $"0x{fk.ID:X6}",
                ["name"] = rec.Name?.String ?? rec.EditorID ?? "?",
                ["tier"] = tier,
                ["obtain"] = obtain[fk],
                ["value"] = value[fk],
                ["lvli"] = lvli[fk],
            });
        }

        // Per-tier median ingredient gold value. The DLL divides by these to
        // value Catalyst/Apex ingredients RELATIVE to their own category
        // (v1.0.1 pricing redo) — they are load-order specific, so they must be
        // emitted here rather than hardcoded.
        static double Median(List<int> xs)
        {
            if (xs.Count == 0) return 0;
            xs.Sort();
            return xs[xs.Count / 2];
        }
        var tierStats = new Dictionary<string, object>();
        foreach (var (key, name) in new[] { ("base", "Base"), ("catalyst", "Catalyst"), ("apex", "Apex") })
        {
            var vals = order.Where(k => tierByKey[k] == name).Select(k => value[k]).ToList();
            tierStats[key] = new Dictionary<string, object>
            {
                ["count"] = vals.Count,
                ["median"] = Median(vals),
            };
        }

        // ── NAMED-LADDER INDEX (see Commands.Ladders.cs for the heuristic).
        // Emitted alongside the tiers so the DLL can apply the POSITIONAL Apex
        // guarantee (top iApexTopRungs rungs of a family) and price sourceless
        // rungs off their family's real reagent pair — the DLL only consumes
        // this; it does no name parsing of its own (marth: programmatically
        // understood in the patcher, not a string convention in the DLL).
        // Pool mirrors the DLL's BuildEffectPotionTable filter: no food, and
        // never MAO's own flask forms.
        var potionPool = lo.PriorityOrder.Ingestible().WinningOverrides()
            .Where(p => !p.Flags.HasFlag(Ingestible.Flag.FoodItem))
            .Where(p => !p.FormKey.ModKey.Name.StartsWith("MAO", StringComparison.OrdinalIgnoreCase))
            .ToList();
        // MGEFs with a real ingredient source (mirrors BuildRecipeTable's
        // value>0 filter): a ladder's representative effect must be one the
        // DLL's recipe table can actually price.
        var sourcedEffects = new HashSet<FormKey>();
        foreach (var i in lo.PriorityOrder.Ingredient().WinningOverrides())
            if (i.Value > 0)
                foreach (var e in i.Effects) sourcedEffects.Add(e.BaseEffect.FormKey);
        var rejects = new List<string>();
        var ladders = DetectLadders(potionPool, cache, sourcedEffects, rejects);
        var ladderJson = new List<object>();
        var signalCount = new Dictionary<string, int>();
        int rungTotal = 0, apexMarked = 0;                 // apex count at the DLL default N=2
        foreach (var lad in ladders)
        {
            foreach (var s in lad.Signal.Split('+'))
                signalCount[s] = signalCount.GetValueOrDefault(s) + 1;
            var rungsJson = new List<object>();
            foreach (var r in lad.Rungs)
            {
                rungTotal++;
                if (r.Rung >= lad.Height - Math.Min(2, lad.Height - 1)) apexMarked++;
                rungsJson.Add(new Dictionary<string, object>
                {
                    ["plugin"] = r.Potion.FormKey.ModKey.FileName.String,
                    ["fid"] = $"0x{r.Potion.FormKey.ID:X6}",
                    ["name"] = r.Potion.Name?.String ?? "?",
                    ["value"] = (int)r.Value,
                    ["rung"] = r.Rung,
                });
            }
            var lj = new Dictionary<string, object>
            {
                ["stem"] = lad.Stem,
                ["signal"] = lad.Signal,
                ["height"] = lad.Height,
                ["rungs"] = rungsJson,
            };
            // The family's representative MGEF — the DLL looks its recipe up in
            // its own g_effectRecipe. Absent when no rung's primary effect has
            // an ingredient source (rungs still get the Apex position marking).
            if (lad.RepEffect is { } eff)
                lj["effect"] = new Dictionary<string, object>
                {
                    ["plugin"] = eff.ModKey.FileName.String,
                    ["fid"] = $"0x{eff.ID:X6}",
                };
            ladderJson.Add(lj);
        }

        // QUALITY ANCHOR. VariantQuality reads 1.0 at this gold value. It is
        // deliberately NOT the potion median: MAO ignores food at every point
        // of the economy (plugin.cpp: "Food is NOT a potion — leave it"), so
        // bread cannot help anchor potion quality — but the honest no-food
        // MEDIAN (126 on marth's Requiem list, 192 on Default) sits ABOVE
        // five of Requiem's six Restore Health rungs, slams the entry rungs
        // into the quality floor and prices the everyday ladder near free.
        // What the validated tuning was ACTUALLY anchored to (measured, marth
        // 2026-07-20) is a low percentile of the no-food pool: the old
        // food-included median landed at ~p13-p18 of it on two very different
        // profiles — that accident is why it worked. So emit the percentile
        // EXPLICITLY: p15 of the no-food, non-MAO potion pool. Quality 1.0
        // then means "a real potion, the cheapest you'd actually brew" — the
        // entry rungs (Diluted/Minor) sit just below it, the working rungs
        // just above, exactly where the Catalyst line is supposed to bite.
        // LVLI-distribution filtering was ruled out (distributed-only medians
        // are within 2 gold of the full pool — essentially every potion form
        // is distributed).
        const double AnchorPercentile = 0.15;
        var anchorVals = potionPool
            .Where(p => p.Value > 0 && p.Effects.Count > 0)
            .Select(p => (int)p.Value).OrderBy(v => v).ToList();
        int Pctl(double f) => anchorVals.Count == 0 ? 0
            : anchorVals[Math.Clamp((int)(anchorVals.Count * f), 0, anchorVals.Count - 1)];
        var potionStats = new Dictionary<string, object>
        {
            ["count"] = anchorVals.Count,
            // The value the DLL consumes: quality 1.0 = this many gold.
            ["anchor"] = Pctl(AnchorPercentile),
            ["anchorPercentile"] = AnchorPercentile,
            // Diagnostic only — the pool's true median, to eyeball the gap.
            ["median"] = Pctl(0.50),
        };

        var doc = new Dictionary<string, object>
        {
            ["from"] = $"{ingr.Count} ingredients scored over {lo.ListedOrder.Count()} plugins",
            ["tierStats"] = tierStats,
            ["potionStats"] = potionStats,
            ["ladders"] = ladderJson,
            ["tiers"] = entries,
        };
        var dir = Path.GetDirectoryName(Path.GetFullPath(outPath));
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.WriteAllText(outPath, System.Text.Json.JsonSerializer.Serialize(doc,
            new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));

        Console.WriteLine($"scored {ingr.Count} ingredients: " +
            $"{floraHits} flora + {treeHits} tree (x{FloraWeight}), {contHits} container, {lvliHits} lvli references");
        Console.WriteLine($"pool {pool.Count} ranked (avail {AvailW:0.00} + value {ValueW:0.00}), " +
            $"{orphans} 0-source -> Base");
        Console.WriteLine($"rank cuts: Apex rarest {apexCut}, Catalyst next {catCut - apexCut}, Base rest");
        Console.WriteLine($"tiers: Apex={tierCount["Apex"]}  Catalyst={tierCount["Catalyst"]}  Base={tierCount["Base"]}");
        Console.WriteLine($"ladders: {ladders.Count} detected over {potionPool.Count} potions " +
            $"({string.Join(", ", signalCount.OrderByDescending(kv => kv.Value).Select(kv => $"{kv.Key} {kv.Value}"))}); " +
            $"{rungTotal} rungs, {apexMarked} apex-marked at top-2 " +
            $"({100.0 * apexMarked / Math.Max(1, potionPool.Count):0.0}% of pool); " +
            $"{rejects.Count} group(s) rejected by validation");
        Console.WriteLine($"quality anchor: {potionStats["anchor"]} gold (p{AnchorPercentile * 100:0} of " +
            $"{anchorVals.Count} no-food potions, pool p50 {potionStats["median"]}; emitted — the DLL consumes this)");
        // Sanity reference: report where the named Vampire Dust ingredient lands,
        // with its raw signals, so the tiering can be eyeballed. NOTE: the design
        // note's FormKey 0x0003AD5F is vanilla FROST SALTS, not Vampire Dust — so
        // we locate it by name.
        var vd = ingr.FirstOrDefault(kv =>
            (kv.Value.Name?.String ?? "").Equals("Vampire Dust", StringComparison.OrdinalIgnoreCase));
        if (vd.Value is not null)
            Console.WriteLine($"ref: Vampire Dust [{vd.Key}] harvest={harvest[vd.Key]} " +
                              $"cont={cont[vd.Key]} lvli={lvli[vd.Key]} obtain={obtain[vd.Key]} " +
                              $"value={value[vd.Key]} -> {Tier(vd.Key)}");
        else
            Console.WriteLine("ref: no ingredient named 'Vampire Dust' in this load order");
        if (Environment.GetEnvironmentVariable("MAO_TIER_DEBUG") == "1")
        {
            var dbg = ingr.Keys.Select(k => new Dictionary<string, object>
            {
                ["name"] = ingr[k].Name?.String ?? ingr[k].EditorID ?? "?",
                ["harvest"] = harvest[k], ["cont"] = cont[k], ["lvli"] = lvli[k],
                ["loot"] = loot[k], ["obtain"] = obtain[k], ["value"] = value[k],
                ["tier"] = tierByKey[k],
            }).OrderBy(e => (int)e["obtain"]).ToList();
            File.WriteAllText(outPath + ".debug.json", System.Text.Json.JsonSerializer.Serialize(
                dbg, new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
            Console.WriteLine($"wrote {outPath}.debug.json (raw components)");
        }
        if (Environment.GetEnvironmentVariable("MAO_LADDER_DEBUG") == "1")
        {
            // Rejected groups + full accepted ladders, for eyeballing a new
            // list's naming convention (the MAO_TIER_DEBUG pattern).
            var dbg = new Dictionary<string, object>
            {
                ["rejected"] = rejects,
                ["ladders"] = ladders.Select(l => new Dictionary<string, object>
                {
                    ["stem"] = l.Stem, ["signal"] = l.Signal, ["height"] = l.Height,
                    ["rungs"] = l.Rungs.Select(r =>
                        $"{r.Potion.Name?.String} [{r.Potion.FormKey}] {r.Value}g rung {r.Rung}").ToList(),
                }).ToList(),
            };
            File.WriteAllText(outPath + ".ladders.json", System.Text.Json.JsonSerializer.Serialize(
                dbg, new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
            Console.WriteLine($"wrote {outPath}.ladders.json (ladder detection detail)");
        }
        Console.WriteLine($"wrote {outPath}");
        return 0;
    }

    // Map an ordered list to a [0,1] percentile per key: first element -> 0,
    // last -> 1. Used so obtain-score and gold-value (wildly different scales)
    // can be blended on equal footing.
    static Dictionary<FormKey, double> PercentileRank(List<FormKey> ordered)
    {
        var rank = new Dictionary<FormKey, double>(ordered.Count);
        int last = ordered.Count - 1;
        for (int i = 0; i < ordered.Count; i++)
            rank[ordered[i]] = last == 0 ? 0.0 : (double)i / last;
        return rank;
    }
}
