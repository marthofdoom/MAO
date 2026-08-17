// MAO shared quest-reference extractor. Shared with MAO.Synthesis via
// <Compile Include> (the Commands.Ladders.cs pattern) so both patchers emit
// byte-identical quest data for the same load order.
//
// THE PROBLEM (validated via quest-scan/quest-dump): "collect N of ordinary
// ingredient X" quests (FreeformRiften10 "Stoking the Flames": 10 fire salts)
// do NOT quest-flag the item — IsQuestObject() is false, so the DLL's runtime
// quest-item guard can't see them. The quest reads the player's count with a
// GetItemCount CONDITION instead, and MAO's conversion sink destroys the item
// on pickup, soft-locking the quest. The item form appears ONLY in conditions:
// on dialogue INFOs owned by the quest (topic.Quest — FFRiften10DoneBranchTopic
// GetItemCount FireSalts >= 10), on quest aliases, and on quest
// DialogConditions. So the clean statically-readable edge is
//   item -> QUEST (via condition ownership), not item -> objective.
//
// THE SPLIT (mirrors the ladder feature): the PATCHER reads the whole load
// order offline and emits { quest -> [ingredient/potion forms it references] }
// into mao_tiers.json ("questRefs"); the DLL builds the reverse index and, at
// conversion time, keeps the item whenever ANY referencing quest currently
// SHOWS IN THE PLAYER'S JOURNAL (enabled + an objective in the kDisplayed
// state). DELIBERATELY NO framework filtering here: always-running dialogue
// frameworks (follower mods, crafting vendors) also reference these items in
// barter lines, but they never display a journal objective — the RUNTIME
// journal predicate is the discriminator, so patch-time emission must be
// complete, not precise. (quest-scan's "PRECISE" heuristic stays a diagnostic
// only.)
//
// Conditions read: GetItemCount and GetIsID naming an Ingredient or a
// non-food Ingestible — the same convertible set the DLL's ContainerSink
// destroys. A GetItemCount on a FORM LIST is expanded one level into its
// convertible members (radiant "bring any of..." quests).

using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Skyrim;

static partial class Commands
{
    public static Dictionary<FormKey, HashSet<FormKey>> CollectQuestItemRefs(
        LoadOrder<IModListingGetter<ISkyrimModGetter>> lo, ILinkCache cache)
    {
        // The forms MAO would convert: ingredients + non-food ingestibles
        // (mirrors ContainerSink: IngredientItem branch + !IsFood AlchemyItem
        // branch; MAO's own flask forms never appear in third-party conditions).
        var alch = new HashSet<FormKey>();
        foreach (var i in lo.PriorityOrder.Ingredient().WinningOverrides()) alch.Add(i.FormKey);
        foreach (var p in lo.PriorityOrder.Ingestible().WinningOverrides())
            if (!p.Flags.HasFlag(Ingestible.Flag.FoodItem)) alch.Add(p.FormKey);

        var byQuest = new Dictionary<FormKey, HashSet<FormKey>>();
        void Note(FormKey quest, FormKey item)
        {
            if (quest.IsNull || item.IsNull) return;
            if (alch.Contains(item))
            {
                if (!byQuest.TryGetValue(quest, out var s)) byQuest[quest] = s = new();
                s.Add(item);
                return;
            }
            // GetItemCount accepts a FormList ("bring any Daedra heart-tier
            // reagent"): expand ONE level into its convertible members. No
            // recursion — a nested list of lists is exotic enough to skip, and
            // an unguarded walk is the MEO dangling-link crash class.
            if (cache.TryResolve<IFormListGetter>(item, out var fl))
                foreach (var entry in fl.Items)
                    if (alch.Contains(entry.FormKey))
                    {
                        if (!byQuest.TryGetValue(quest, out var s)) byQuest[quest] = s = new();
                        s.Add(entry.FormKey);
                    }
        }

        void Scan(IEnumerable<IConditionGetter> conds, FormKey quest)
        {
            foreach (var c in conds)
            {
                switch (c.Data)
                {
                    case IGetItemCountConditionDataGetter g:
                        Note(quest, g.ItemOrList.Link.FormKey);
                        break;
                    case IGetIsIDConditionDataGetter g:
                        Note(quest, g.Object.Link.FormKey);
                        break;
                }
            }
        }

        foreach (var q in lo.PriorityOrder.Quest().WinningOverrides())
        {
            foreach (var a in q.Aliases) Scan(a.Conditions, q.FormKey);
            Scan(q.DialogConditions, q.FormKey);
        }
        // Dialogue INFO conditions, attributed to the topic's OWNING quest
        // (FFRiften10DoneBranchTopic.Quest == FreeformRiften10 — confirmed via
        // quest-dump). Winning topic overrides, the Commands.QuestScan.cs
        // pattern.
        foreach (var topic in lo.PriorityOrder.DialogTopic().WinningOverrides())
        {
            var qk = topic.Quest.FormKey;
            if (qk.IsNull) continue;
            foreach (var resp in topic.Responses) Scan(resp.Conditions, qk);
        }
        return byQuest;
    }
}
