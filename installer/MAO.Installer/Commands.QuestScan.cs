using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;

// VALIDATION: how does the load order encode "a quest wants ordinary ingredient
// X" (e.g. collect fire salts)? MAO can't see this at runtime — the item isn't
// quest-FLAGGED; the quest checks GetItemCount(X) in a condition. This scans the
// load order for those conditions to prove the mechanism is READABLE offline and
// to measure how many quests/items a static scan would flag (false-positive
// risk). Scans alias conditions, quest DialogConditions, and dialogue INFO
// conditions attributed to their owning quest.
static partial class Commands
{
    public static int QuestScan(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string needle)
    {
        // The forms MAO would convert: ingredients + non-food potions.
        var alch = new HashSet<FormKey>();
        foreach (var i in lo.PriorityOrder.Ingredient().WinningOverrides()) alch.Add(i.FormKey);
        foreach (var p in lo.PriorityOrder.Ingestible().WinningOverrides())
            if (!p.Flags.HasFlag(Ingestible.Flag.FoodItem)) alch.Add(p.FormKey);

        string Name(FormKey fk) =>
            cache.TryResolve<ISkyrimMajorRecordGetter>(fk, out var r) ? (r.EditorID ?? fk.ToString()) : fk.ToString();

        var byQuest = new Dictionary<FormKey, HashSet<FormKey>>();
        var srcOf = new Dictionary<FormKey, string>();
        void Note(FormKey quest, FormKey item, string src)
        {
            if (quest.IsNull || !alch.Contains(item)) return;
            if (!byQuest.TryGetValue(quest, out var s)) byQuest[quest] = s = new();
            s.Add(item);
            srcOf[quest] = src;
        }

        void Scan(System.Collections.Generic.IEnumerable<IConditionGetter> conds, FormKey quest, string src)
        {
            foreach (var c in conds)
            {
                switch (c.Data)
                {
                    case IGetItemCountConditionDataGetter g:
                        Note(quest, g.ItemOrList.Link.FormKey, src);
                        break;
                    case IGetIsIDConditionDataGetter g:
                        Note(quest, g.Object.Link.FormKey, src);
                        break;
                }
            }
        }

        int aliasHits = 0, questHits = 0, infoHits = 0;
        foreach (var q in lo.PriorityOrder.Quest().WinningOverrides())
        {
            int before = byQuest.TryGetValue(q.FormKey, out var s0) ? s0.Count : 0;
            foreach (var a in q.Aliases) Scan(a.Conditions, q.FormKey, "alias");
            aliasHits += (byQuest.TryGetValue(q.FormKey, out var s1) ? s1.Count : 0) - before;
            before = byQuest.TryGetValue(q.FormKey, out var s2) ? s2.Count : 0;
            Scan(q.DialogConditions, q.FormKey, "questcond");
            questHits += (byQuest.TryGetValue(q.FormKey, out var s3) ? s3.Count : 0) - before;
        }
        foreach (var topic in lo.PriorityOrder.DialogTopic().WinningOverrides())
        {
            var qk = topic.Quest.FormKey;
            if (qk.IsNull) continue;
            int before = byQuest.TryGetValue(qk, out var s0) ? s0.Count : 0;
            foreach (var resp in topic.Responses) Scan(resp.Conditions, qk, "dialogue");
            infoHits += (byQuest.TryGetValue(qk, out var s1) ? s1.Count : 0) - before;
        }

        // Precision filter: a "collect" quest is not START-GAME-ENABLED (those
        // are always-running frameworks: followers, crafting vendors) and HAS at
        // least one objective. Barter/service dialogue lives on always-running
        // quests, so this strips the worst false positives.
        var qMeta = new Dictionary<FormKey, (bool sge, int obj)>();
        foreach (var q in lo.PriorityOrder.Quest().WinningOverrides())
            qMeta[q.FormKey] = (q.Flags.HasFlag(Quest.Flag.StartGameEnabled), q.Objectives.Count);
        bool Precise(FormKey qk) => qMeta.TryGetValue(qk, out var m) && !m.sge && m.obj > 0;
        var preciseForms = new HashSet<FormKey>();
        int preciseQuests = 0;
        foreach (var kv in byQuest)
            if (Precise(kv.Key)) { preciseQuests++; foreach (var f in kv.Value) preciseForms.Add(f); }

        int totalItems = byQuest.Values.Sum(s => s.Count);
        Console.WriteLine($"{byQuest.Count} quests reference {totalItems} ingredient/potion slot(s) via item conditions");
        Console.WriteLine($"  by source (first-seen per quest): alias={aliasHits} questcond={questHits} dialogue={infoHits}");
        // Distribution: how many DISTINCT ingredients would ever be flagged, and
        // the worst offenders (a quest flagging a common item = false-positive pain).
        var perItem = new Dictionary<FormKey, int>();
        foreach (var s in byQuest.Values)
            foreach (var f in s) perItem[f] = perItem.GetValueOrDefault(f) + 1;
        Console.WriteLine($"  {perItem.Count} distinct ingredient/potion forms flagged across all quests");
        Console.WriteLine($"  PRECISE (not start-game-enabled + has objectives): {preciseQuests} quests -> {preciseForms.Count} distinct forms");

        if (needle != "")
        {
            Console.WriteLine($"\n--- quests referencing '{needle}' ---");
            foreach (var kv in byQuest)
            {
                var hits = kv.Value.Where(f => Name(f).Contains(needle, StringComparison.OrdinalIgnoreCase)).ToList();
                if (hits.Count == 0) continue;
                var qn = cache.TryResolve<IQuestGetter>(kv.Key, out var q)
                    ? $"{q.EditorID} '{q.Name?.String}'" : kv.Key.ToString();
                Console.WriteLine($"  [{srcOf.GetValueOrDefault(kv.Key)}] {qn}: {string.Join(", ", hits.Select(Name))}");
            }
        }
        return 0;
    }

    // Deep-dump one quest by name/EDID: plugin, flags, objectives, aliases (with
    // any item form they reference), and item conditions across its dialogue.
    public static int QuestDump(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache, string needle)
    {
        string Name(FormKey fk) =>
            cache.TryResolve<ISkyrimMajorRecordGetter>(fk, out var r) ? (r.EditorID ?? r.FormKey.ToString()) : fk.ToString();
        bool Match(IQuestGetter q) =>
            (q.EditorID ?? "").Contains(needle, StringComparison.OrdinalIgnoreCase) ||
            (q.Name?.String ?? "").Contains(needle, StringComparison.OrdinalIgnoreCase);

        foreach (var q in lo.PriorityOrder.Quest().WinningOverrides())
        {
            if (!Match(q)) continue;
            Console.WriteLine($"QUEST {q.EditorID} '{q.Name?.String}' [{q.FormKey}]");
            Console.WriteLine($"  flags={q.Flags}  type={q.Type}  objectives={q.Objectives.Count}  aliases={q.Aliases.Count}");
            foreach (var o in q.Objectives)
                Console.WriteLine($"  OBJ #{o.Index} '{o.DisplayText}' targets={o.Targets?.Count ?? 0}");
            foreach (var a in q.Aliases)
            {
                // What form does this alias reference (created object / forced ref /
                // find-matching keyword) + any item conditions?
                var items = new List<string>();
                foreach (var c in a.Conditions)
                    if (c.Data is IGetItemCountConditionDataGetter g) items.Add("GetItemCount " + Name(g.ItemOrList.Link.FormKey));
                    else if (c.Data is IGetIsIDConditionDataGetter g2) items.Add("GetIsID " + Name(g2.Object.Link.FormKey));
                Console.WriteLine($"  ALIAS #{a.ID} '{a.Name}' conds=[{string.Join(", ", items)}]");
            }
            // Dialogue owned by this quest, item conditions
            foreach (var topic in lo.PriorityOrder.DialogTopic().WinningOverrides())
            {
                if (topic.Quest.FormKey != q.FormKey) continue;
                foreach (var resp in topic.Responses)
                    foreach (var c in resp.Conditions)
                        if (c.Data is IGetItemCountConditionDataGetter g)
                            Console.WriteLine($"  DIALOGUE {topic.EditorID}: GetItemCount {Name(g.ItemOrList.Link.FormKey)} {(c is IConditionFloatGetter cf ? cf.CompareOperator + " " + cf.ComparisonValue : "")}");
            }
        }
        return 0;
    }
}
