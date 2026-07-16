// MAO perk-tree patch. Ported VERBATIM from MEO's Commands.cs WritePatch/
// ApplyPatch (the release-proven vehicle) — only the target tree (AVAlchemy),
// the craft classifier (alchemy entry points, Docs/PERK_TREE_RECON.md census),
// the perk FormKeys and the node layout differ. Lives in its own file so the
// future MAO.Synthesis project can <Compile Include> the EXACT same logic —
// byte-identical outputs for the same load order.

using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;

static partial class Commands
{
    // Standalone tool: build the MAO - Patch.esp, apply the perk-tree edits into
    // it, and write it to outPath. The tree edits (incl. the curated foreign-perk
    // choices next to the output) all live in ApplyPatch, which Synthesis shares.
    public static int WritePatch(
        ILoadOrderGetter<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string outPath)
    {
        var patch = new SkyrimMod(ModKey.FromNameAndExtension("MAO - Patch.esp"),
                                  SkyrimRelease.SkyrimSE)
        {
            // Pure-override plugin: ESL-flag it so it costs no load-order slot.
            IsSmallMaster = true,
        };
        var rc = ApplyPatch(lo, cache, patch, Path.ChangeExtension(outPath, ".choices.json"));
        if (rc != 0) return rc;
        patch.WriteToBinary(outPath);
        Console.WriteLine($"wrote {outPath}");
        Console.WriteLine($"masters: {string.Join(", ", patch.ModHeader.MasterReferences.Select(m => m.Master))}");
        return 0;
    }

    // The perk-tree edit, applied into a CALLER-SUPPLIED patch mod so both the
    // standalone tool and the (future) Synthesis patcher run the exact same
    // logic. choicesPath: non-null = interactive/persisted keep-drop curation;
    // null (Synthesis) = non-interactive KEEP-ALL, never prompts, no file.
    public static int ApplyPatch(
        ILoadOrderGetter<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache,
        ISkyrimMod patch, string? choicesPath = null)
    {
        var av = lo.PriorityOrder.ActorValueInformation().WinningOverrides()
            .FirstOrDefault(a => a.EditorID == "AVAlchemy");
        if (av is null) return Fail("AVAlchemy not found");

        var mao = ModKey.FromNameAndExtension("MAO.esp");
        if (!lo.ListedOrder.Any(l => l.ModKey == mao))
            return Fail("MAO.esp not in load order — install MAO first");

        // MAO.esp-local perk FormIDs (MAO_GenerateESP MAO_PERKS band; FROZEN).
        FormKey Perk(uint local) => new(mao, local);
        var calHead   = Perk(0x818);   // Kit Calibration, ranked 1..5 via NNAM chain
        var fluid     = Perk(0x81D);   // Fluid Motion
        var vanguard  = Perk(0x81E);   // Vanguard Coating
        var apexStab  = Perk(0x81F);   // Apex Stabilization
        var fieldExt  = Perk(0x820);   // Field Extraction
        var corrosive = Perk(0x821);   // Corrosive Retention
        var pouch     = Perk(0x822);   // Pouch Expansion
        var extSynth  = Perk(0x823);   // Extended Synthesis
        var capstone  = Perk(0x816);   // Master Alchemist's Crucible

        var over = patch.ActorValueInformation.GetOrAddAsOverride(av);

        // Classify every source node by what its perk (and NNAM rank chain)
        // actually DOES — never by name (prime directive). Alchemy-craft entry
        // points mark the brewing system MAO replaces; anything else (harvest,
        // food, combat, passives riding the tree) is kept and rewired.
        static bool IsCraftEntry(APerkEntryPointEffect.EntryType ep)
        {
            // Census: Docs/PERK_TREE_RECON.md (vanilla + LoreRim).
            // ModAlchemyEffectiveness (brew magnitude — Alchemist/Physician/
            // Benefactor/Poisoner and Requiem's scalers), ModPotionsCreated
            // (potions-per-craft, Requiem Catalysis), ModInitialIngredient-
            // EffectsLearned (eat-to-learn, impossible in the flask design),
            // PurifyAlchemyIngredients (brew purity), ModPoisonDoseCount
            // (poison doses — coating charges replace them). Named by ENTRY
            // POINT, not perk name; harvest/food/combat perks survive.
            var s = ep.ToString();
            return s == "ModAlchemyEffectiveness" ||
                   s == "ModPotionsCreated" ||
                   s == "ModInitialIngredientEffectsLearned" ||
                   s == "PurifyAlchemyIngredients" ||
                   s == "ModPoisonDoseCount";
        }
        IEnumerable<IPerkGetter> RankChain(IPerkGetter first)
        {
            // Guard against an NNAM cycle in a malformed perk mod (self- or
            // mutually-referential NextPerk) — never rewalk a perk.
            var seen = new HashSet<FormKey>();
            for (var p = first; p is not null && seen.Add(p.FormKey);
                 p = !p.NextPerk.IsNull && p.NextPerk.TryResolve(cache, out var np) ? np : null)
            {
                yield return p;
            }
        }
        // Also claim perks whose runtime entries only fire for
        // FormType=Ingredient magic — eat-ingredient scalers (LoreRim's
        // Herbalist ×2/×5) are dead once conversion consumes ingredients;
        // the alchemy analogue of MEO's Enchantment-object catch.
        static bool RequiresIngredientObject(IAPerkEffectGetter e) =>
            e.Conditions.Any(pc => pc.Conditions.Any(c =>
                c is IConditionFloatGetter cf &&
                cf.CompareOperator == CompareOperator.EqualTo && cf.ComparisonValue == 1 &&
                cf.Data is IGetIsObjectTypeConditionDataGetter g &&
                g.GetType().GetProperty("FormType")?.GetValue(g)?.ToString() == "Ingredient"));
        bool IsCraftPerk(IPerkGetter perk) => RankChain(perk).Any(p =>
            p.Effects.OfType<IAPerkEntryPointEffectGetter>().Any(e => IsCraftEntry(e.EntryPoint)) ||
            p.Effects.OfType<IAPerkEffectGetter>().Any(RequiresIngredientObject));

        var removedIdx = new HashSet<uint>();
        var removedPerks = new HashSet<FormKey>();
        var keptPerkHeads = new List<IPerkGetter>();
        var keepCandidates = new List<(uint Idx, IPerkGetter Perk)>();
        foreach (var n in av.PerkTree)
        {
            if ((n.Index ?? 0) == 0) continue;
            if (!n.Perk.TryResolve(cache, out var perk) || IsCraftPerk(perk))
            {
                removedIdx.Add(n.Index ?? 0);
                if (perk is not null)
                    foreach (var p in RankChain(perk)) removedPerks.Add(p.FormKey);
            }
            else
            {
                keepCandidates.Add((n.Index ?? 0, perk));
            }
        }

        // Curation: the classifier decides what is MAO's domain; the human
        // decides which surviving perks are worth keeping (marth 2026-07-16:
        // vanilla survivors Snakeblood/Green Thumb KEEP — not duplicative of
        // MAO's Extended Synthesis/Pouch Expansion). Decisions persist next to
        // the patch so re-runs don't re-ask.
        var choices = choicesPath is not null && File.Exists(choicesPath)
            ? System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, bool>>(
                  File.ReadAllText(choicesPath)) ?? []
            : [];
        // No curation file (Synthesis) means non-interactive KEEP-ALL — never prompt.
        var interactive = choicesPath is not null &&
                          (!Console.IsInputRedirected ||
                           Environment.GetEnvironmentVariable("MAO_ASSUME_TTY") == "1");
        foreach (var (idx, perk) in keepCandidates)
        {
            var key = perk.FormKey.ToString();
            if (!choices.TryGetValue(key, out var keep))
            {
                var ranks = RankChain(perk).ToList();
                Console.WriteLine($"\nnon-craft perk in the tree: '{perk.Name}'" +
                                  $" ({ranks.Count} rank(s)) [{perk.FormKey.ModKey}]");
                foreach (var r in ranks)
                    if (r.Description?.String is { Length: > 0 } d)
                        Console.WriteLine($"  desc: {d}");
                foreach (var e in perk.Effects.OfType<IAPerkEntryPointEffectGetter>())
                    Console.WriteLine($"  effect: {e.EntryPoint}");
                foreach (var e in perk.Effects.OfType<IPerkAbilityEffectGetter>())
                    Console.WriteLine($"  ability: {(e.Ability.TryResolve(cache, out var sp) ? sp.Name?.String ?? sp.EditorID : "?")}");
                if (interactive)
                {
                    Console.Write("  keep it in the new tree? [Y/n]: ");
                    var a = Console.ReadLine()?.Trim().ToLowerInvariant();
                    keep = a is not ("n" or "no");
                    choices[key] = keep;
                }
                else
                {
                    // Fallback, not a decision: don't record it, so a later
                    // interactive run still asks.
                    keep = true;
                    Console.WriteLine("  (non-interactive: kept, not recorded)");
                }
            }
            Console.WriteLine($"  -> {(keep ? "KEEP" : "REMOVE")} '{perk.Name}'");
            if (keep)
            {
                keptPerkHeads.Add(perk);
            }
            else
            {
                removedIdx.Add(idx);
                foreach (var p in RankChain(perk)) removedPerks.Add(p.FormKey);
            }
        }
        if (choicesPath is not null && choices.Count > 0)
            File.WriteAllText(choicesPath, System.Text.Json.JsonSerializer.Serialize(
                choices, new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));

        var root = over.PerkTree.FirstOrDefault(n => (n.Index ?? 0) == 0)
                   ?? throw new InvalidOperationException("no root node in source tree");
        foreach (var n in over.PerkTree.Where(n => removedIdx.Contains(n.Index ?? 0)).ToList())
            over.PerkTree.Remove(n);
        foreach (var n in over.PerkTree)
            for (int i = n.ConnectionLineToIndices.Count - 1; i >= 0; i--)
                if (removedIdx.Contains(n.ConnectionLineToIndices[i]))
                    n.ConnectionLineToIndices.RemoveAt(i);

        // Kept nodes that lost every parent hang off the root again.
        var referenced = over.PerkTree.Where(n => (n.Index ?? 0) != 0)
            .SelectMany(n => n.ConnectionLineToIndices).ToHashSet();
        var orphans = over.PerkTree.Select(n => n.Index ?? 0)
            .Where(i => i != 0 && !referenced.Contains(i)).ToList();
        root.ConnectionLineToIndices.Clear();
        foreach (var o in orphans) root.ConnectionLineToIndices.Add(o);

        // Kept perks whose HasPerk prerequisite points at a removed perk get a
        // record override with just that dangling condition dropped.
        foreach (var head in keptPerkHeads)
        {
            foreach (var p in RankChain(head))
            {
                var dangling = p.Conditions.Where(c =>
                    c is IConditionFloatGetter cf &&
                    cf.Data is IHasPerkConditionDataGetter hp &&
                    removedPerks.Contains(hp.Perk.Link.FormKey)).ToList();
                if (dangling.Count == 0) continue;
                var po = patch.Perks.GetOrAddAsOverride(p);
                po.Conditions.RemoveAll(c =>
                    c is IConditionFloat cf && cf.Data is IHasPerkConditionDataGetter hp &&
                    removedPerks.Contains(hp.Perk.Link.FormKey));
                Console.WriteLine($"  perk override {p.EditorID}: dropped {dangling.Count} dangling HasPerk condition(s)");
            }
        }

        // MAO nodes get fresh indices and grid cells that don't collide with
        // anything kept. Layout principle (marth, inherited from MEO m36k/m36m):
        // CHOICES fan out in parallel from the hub at the SAME depth (gridY=1) —
        // never stacked one behind another (the constellation UI reads
        // same-column/increasing-gridY as a sequential chain). Flat cumulative
        // power goes SEQUENTIAL: Kit Calibration (5 ranks) is the spine; the two
        // sequential branches grow downward: Vanguard Coating -> Corrosive
        // Retention (coating line) and Field Extraction -> Pouch Expansion
        // (gathering line).
        //   fluid  vanguard apexStab      fieldExt  extSynth  capstone
        //   (x-3)  (x-2,1)  (x-1,1)       (x+1,1)   (x+2,1)   (x+3,1)
        //          corrosive(x-2,2)  calibration hub (x,0)  pouch (x+1,2)
        var occupied = over.PerkTree.Where(n => (n.Index ?? 0) != 0)
            .Select(n => (n.PerkGridX ?? 0, n.PerkGridY ?? 0)).ToHashSet();
        uint xBase = 3;  // leftmost reach is x-3; start clear of the grid edge
        (uint, uint)[] Cells(uint x) => [(x, 0u), (x - 3, 1u), (x - 2, 1u), (x - 2, 2u),
                                         (x - 1, 1u), (x + 1, 1u), (x + 1, 2u),
                                         (x + 2, 1u), (x + 3, 1u)];
        while (Cells(xBase).Any(c => occupied.Contains(c))) xBase++;
        uint nextIdx = over.PerkTree.Max(n => n.Index ?? 0) + 1;

        ActorValuePerkNode Node(uint idx, FormKey perk, uint gx, uint gy, params uint[] conns)
        {
            var n = new ActorValuePerkNode
            {
                Index = idx,
                PerkGridX = gx,
                PerkGridY = gy,
                HorizontalPosition = 0f,
                VerticalPosition = 0f,
                FNAM = new byte[] { 1, 0, 0, 0 },  // uint32 1, as on every vanilla perk node
            };
            n.AssociatedSkill.SetTo(av.FormKey);
            n.Perk.SetTo(perk);
            foreach (var c in conns) n.ConnectionLineToIndices.Add(c);
            return n;
        }

        uint iCal = nextIdx, iFlu = nextIdx + 1, iVan = nextIdx + 2, iCor = nextIdx + 3,
             iApx = nextIdx + 4, iFld = nextIdx + 5, iPch = nextIdx + 6,
             iSyn = nextIdx + 7, iCap = nextIdx + 8;
        over.PerkTree.Add(Node(iCal, calHead, xBase, 0,
                               iFlu, iVan, iApx, iFld, iSyn, iCap));  // hub fans to the choices
        over.PerkTree.Add(Node(iFlu, fluid,     xBase - 3, 1));           // choice
        over.PerkTree.Add(Node(iVan, vanguard,  xBase - 2, 1, iCor));     // coating line:
        over.PerkTree.Add(Node(iCor, corrosive, xBase - 2, 2));           // Vanguard -> Corrosive
        over.PerkTree.Add(Node(iApx, apexStab,  xBase - 1, 1));           // choice
        over.PerkTree.Add(Node(iFld, fieldExt,  xBase + 1, 1, iPch));     // gathering line:
        over.PerkTree.Add(Node(iPch, pouch,     xBase + 1, 2));           // FieldExtract -> Pouch
        over.PerkTree.Add(Node(iSyn, extSynth,  xBase + 2, 1));           // choice
        over.PerkTree.Add(Node(iCap, capstone,  xBase + 3, 1));           // master milestone (Alchemy 100 CTDA gates it)
        root.ConnectionLineToIndices.Add(iCal);

        Console.WriteLine($"tree: {removedIdx.Count} craft node(s) replaced, " +
                          $"{over.PerkTree.Count - 10} kept, MAO at x={xBase}, " +
                          $"{orphans.Count} kept orphan(s) reparented to root");
        return 0;
    }
}
