// Njordlinger Combat Gameplay Overhaul - the runesmith (PLAN.md phase 4 step 3, D22, Q28 default; config/runesmith.json).
//
// Records (appended after the soul stones, so nothing earlier moves): the NPC (traits from a vanilla levelled Nord, our
// name, a mage's robes, essential), his vendor faction, a buy/sell list (soul-gem goods), a stone chest in a new storage
// cell of our own, the quest "Words in Stone" (stages 10 and 100, one objective), its dialogue branch and five topics
// whose stage-setting lines carry tiny Papyrus fragments (papyrus/*.psc -> data/Scripts), a global the SKSE plugin
// sets once the player has learned a word of power, and ONE override: the Sleeping Giant Inn's interior cell, copied
// whole from the master (never hand-built - past mistake 12444), to hold his placed reference.
// The SKSE plugin opens the socketing window when his "Set my soul stones." line ends (TESTopicInfoEvent).
using System.Text.Json;
using System.Text.Json.Nodes;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;
using Noggog;

static class RunesmithBuild
{
    public static void Run(SkyrimMod mod, ModKey modKey, string repo, JsonNode cfg, JsonObject forms, Dictionary<string, FormKey> keywords)
    {
        var folders = (Environment.GetEnvironmentVariable("NJCGO_MASTERS") ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries);
        var masters = new List<ISkyrimModGetter>();
        foreach (var m in new[] { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "Dragonborn.esm" })
        {
            var path = folders.Select(f => Path.Combine(f, m)).FirstOrDefault(File.Exists);
            if (path != null) { masters.Add(SkyrimMod.CreateFromBinaryOverlay(path, SkyrimRelease.SkyrimSE)); }
        }
        var cache = masters.ToImmutableLinkCache();
        T ByEdid<T>(string edid) where T : class, IMajorRecordGetter
        {
            foreach (var m in Enumerable.Reverse(masters))
            {
                foreach (var r in m.EnumerateMajorRecords<T>()) { if (r.EditorID == edid) { return r; } }
            }
            throw new InvalidOperationException($"runesmith: {typeof(T).Name} {edid} is not in the masters");
        }
        string Hex(IMajorRecordGetter r) => $"0x{r.FormKey.ID:X3}";
        var stonesTable = forms["stones"] ?? throw new InvalidOperationException("runesmith: build the soul stones first");

        // ---- the NPC ------------------------------------------------------------------------------------------------
        var n = cfg["npc"]!;
        var learnedGlobal = new GlobalShort(mod) { EditorID = "NJCGO_RunesmithWordLearned", Data = 0 };
        mod.Globals.Add(learnedGlobal);

        var buySell = mod.FormLists.AddNew("NJCGO_RunesmithBuySell");
        buySell.Items.Add(new FormLink<IKeywordGetter>(keywords["VendorItemSoulGem"]));

        var chest = mod.Containers.AddNew("NJCGO_RunesmithChest");
        chest.Name = cfg["shopChest"]!["name"]!.GetValue<string>();
        chest.Items = new ExtendedList<ContainerEntry>();
        var vendorList = new FormKey(modKey, Convert.ToUInt32(stonesTable["vendor"]!.GetValue<string>(), 16));
        for (var i = 0; i < cfg["shopChest"]!["vendorStones"]!.GetValue<int>(); ++i)
        {
            chest.Items.Add(new ContainerEntry { Item = new ContainerItem { Item = new FormLink<IItemGetter>(vendorList), Count = 1 } });
        }
        chest.Flags = Container.Flag.Respawns;

        // a storage cell of our own for the chest (a new cell - nothing of the master is overridden for it)
        var storage = new Cell(mod) { EditorID = "NJCGO_RunesmithStorage", Name = "Runesmith storage", Flags = Cell.Flag.IsInteriorCell };
        var chestRef = new PlacedObject(mod) { Base = chest.ToNullableLink(), Placement = new Placement { Position = new P3Float(0, 0, 0), Rotation = new P3Float(0, 0, 0) } };
        chestRef.MajorRecordFlagsRaw |= 0x400;   // persistent
        storage.Persistent.Add(chestRef);
        AddInteriorCell(mod, storage);

        var faction = mod.Factions.AddNew("NJCGO_RunesmithFaction");
        faction.Name = "Runesmith";
        faction.Flags = Faction.FactionFlag.Vendor | Faction.FactionFlag.HiddenFromPC;
        faction.VendorBuySellList.SetTo(buySell);
        faction.MerchantContainer.SetTo(chestRef);
        faction.VendorValues = new VendorValues { StartHour = 0, EndHour = 24, Radius = 0 };

        var npc = mod.Npcs.AddNew("NJCGO_Runesmith");
        npc.Name = n["name"]!.GetValue<string>();
        npc.ShortName = n["shortName"]!.GetValue<string>();
        npc.Race.SetTo(ByEdid<IRaceGetter>(n["race"]!.GetValue<string>()));
        npc.Class.SetTo(ByEdid<IClassGetter>(n["class"]!.GetValue<string>()));
        npc.DefaultOutfit.SetTo(ByEdid<IOutfitGetter>(n["outfit"]!.GetValue<string>()));
        npc.Template.SetTo(ByEdid<ILeveledNpcGetter>(n["template"]!.GetValue<string>()));
        npc.Configuration.TemplateFlags = NpcConfiguration.TemplateFlag.Traits;
        npc.Configuration.Flags = NpcConfiguration.Flag.Unique | NpcConfiguration.Flag.Essential;
        npc.Configuration.Level = new NpcLevel { Level = n["level"]!.GetValue<short>() };
        npc.Factions.Add(new RankPlacement { Faction = faction.ToLink(), Rank = 0 });
        npc.AIData.Confidence = Confidence.Average;
        npc.AIData.Aggression = Aggression.Unaggressive;
        npc.AIData.Responsibility = Responsibility.NoCrime;
        // Fable 14:25: his AI must keep him where the plugin puts him (the inn), never pull him back to the storage cell
        // he is placed in - the game's own "sandbox around where I am now" package, never one by editor location.
        npc.Packages.Add(ByEdid<IPackageGetter>("DefaultSandboxCurrentLocation256").ToLink());

        // his place: the inn's interior cell, overridden as a whole copy of the master's record
        var home = cfg["home"]!;
        var cellGetter = ByEdid<ICellGetter>(home["cell"]!.GetValue<string>());
        // Q67 (Fable 14:20): the inn's CELL record is NOT overridden (inn overhauls edit it). He is placed in our storage
        // cell, and the SKSE plugin moves him to this spot the first time the player is in the inn (runesmith.json).
        var spot = FindSpot(cellGetter, cache, home["near"]!.AsArray().Select(x => x!.GetValue<string>()).ToArray());
        var off = home["offset"]!.AsArray().Select(x => x!.GetValue<float>()).ToArray();
        var npcRef = new PlacedNpc(mod)
        {
            Base = npc.ToNullableLink(),
            Placement = new Placement { Position = new P3Float(0, 64, 0), Rotation = new P3Float(0, 0, 0) }
        };
        npcRef.MajorRecordFlagsRaw |= 0x400;
        storage.Persistent.Add(npcRef);

        // ---- the quest and its dialogue -------------------------------------------------------------------------------
        var q = cfg["quest"]!;
        var quest = mod.Quests.AddNew("NJCGO_RunesmithQuest");
        quest.Name = q["name"]!.GetValue<string>();
        quest.Flags = Quest.Flag.StartGameEnabled | Quest.Flag.RunOnce;
        quest.Priority = 50;
        quest.Type = Quest.TypeEnum.SideQuest;
        foreach (var (stage, text) in q["stages"]!.AsObject())
        {
            var st = new QuestStage { Index = ushort.Parse(stage) };
            var entry = new QuestLogEntry { Entry = text!.GetValue<string>() };
            if (stage == "100") { entry.Flags = QuestLogEntry.Flag.CompleteQuest; }
            st.LogEntries.Add(entry);
            quest.Stages.Add(st);
        }
        quest.Objectives.Add(new QuestObjective { Index = 10, DisplayText = q["objective"]!.GetValue<string>() });

        Condition StageIs(string a_spec)
        {
            var op = CompareOperator.EqualTo;
            var v = a_spec;
            if (a_spec.StartsWith(">=")) { op = CompareOperator.GreaterThanOrEqualTo; v = a_spec[2..]; }
            else if (a_spec.StartsWith("<")) { op = CompareOperator.LessThan; v = a_spec[1..]; }
            var d = new GetStageConditionData();
            d.Quest.Link.SetTo(quest.FormKey);
            return new ConditionFloat { CompareOperator = op, ComparisonValue = float.Parse(v), Data = d };
        }
        Condition SpeakerIsHim()
        {
            var d = new GetIsIDConditionData();
            d.Object.Link.SetTo(npc.FormKey);
            return new ConditionFloat { CompareOperator = CompareOperator.EqualTo, ComparisonValue = 1, Data = d };
        }
        Condition Learned(int a_v)
        {
            var d = new GetGlobalValueConditionData();
            d.Global.Link.SetTo(learnedGlobal.FormKey);
            return new ConditionFloat { CompareOperator = CompareOperator.EqualTo, ComparisonValue = a_v, Data = d };
        }

        var topicForms = new JsonObject();
        DialogTopic? first = null;
        foreach (var t in cfg["topics"]!.AsArray())
        {
            var id = t!["id"]!.GetValue<string>();
            var topic = mod.DialogTopics.AddNew("NJCGO_Runesmith" + id);
            topic.Name = t["prompt"]!.GetValue<string>();
            topic.Quest.SetTo(quest);
            // one top-level branch per topic, starting at it, as every vanilla player topic is built
            var branch = mod.DialogBranches.AddNew("NJCGO_RunesmithBranch" + id);
            branch.Quest.SetTo(quest);
            { var cp = branch.GetType().GetProperty("Category")!; cp.SetValue(branch, Enum.Parse(Nullable.GetUnderlyingType(cp.PropertyType) ?? cp.PropertyType, "Player")); }
            branch.Flags = DialogBranch.Flag.TopLevel;
            branch.StartingTopic.SetTo(topic);
            topic.Branch.SetTo(branch);
            topic.Category = DialogTopic.CategoryEnum.Topic;
            topic.Subtype = DialogTopic.SubtypeEnum.Custom;
            topic.SubtypeName = "CUST";   // SNAM: the game reads the subtype from it (blank in the df8b build)
            topic.Priority = 50;
            first ??= topic;
            var info = new DialogResponses(mod.GetNextFormKey(), SkyrimRelease.SkyrimSE);
            info.Flags = new DialogResponseFlags();
            info.FavorLevel = FavorLevel.None;
            var k = 1;
            foreach (var line in t["say"]!.AsArray())
            {
                info.Responses.Add(new DialogResponse { Text = line!.GetValue<string>(), ResponseNumber = (byte)k++, Emotion = Emotion.Neutral, EmotionValue = 50 });
            }
            info.Conditions.Add(SpeakerIsHim());
            info.Conditions.Add(StageIs(t["stage"]!.GetValue<string>()));
            if (t["learned"] is JsonNode ln) { info.Conditions.Add(Learned(ln.GetValue<int>())); }
            if (t["script"] is JsonNode sc)
            {
                var name = sc.GetValue<string>();
                info.VirtualMachineAdapter = new DialogResponsesAdapter
                {
                    Scripts = new ExtendedList<ScriptEntry> { new ScriptEntry { Name = name } },
                    ScriptFragments = new ScriptFragments { FileName = name, OnEnd = new ScriptFragment { ScriptName = name, FragmentName = "Fragment_0" } }
                };
            }
            topic.Responses.Add(info);
            topicForms[id] = new JsonObject { ["topic"] = Hex(topic), ["info"] = Hex(info) };
        }

        forms["runesmith"] = new JsonObject
        {
            ["npc"] = Hex(npc), ["npcRef"] = Hex(npcRef), ["quest"] = Hex(quest), ["learnedGlobal"] = Hex(learnedGlobal),
            ["faction"] = Hex(faction), ["chest"] = Hex(chest), ["chestRef"] = Hex(chestRef), ["topics"] = topicForms,
            ["home"] = $"{cellGetter.EditorID} at {spot.pos.X:F0},{spot.pos.Y:F0},{spot.pos.Z:F0} (near {spot.what})",
            ["homeCell"] = $"{cellGetter.FormKey.ModKey.FileName}|0x{cellGetter.FormKey.ID:X}",
            ["homePos"] = new JsonArray(spot.pos.X + off[0], spot.pos.Y + off[1], spot.pos.Z + off[2]),
            ["homeAngleZ"] = spot.rot.Z
        };
        var outDir = Path.Combine(repo, "data", "SKSE", "Plugins", "NjordlingerCombatGameplayOverhaul");
        var table = JsonNode.Parse(forms["runesmith"]!.ToJsonString())!.AsObject();
        table["schema"] = "NjordlingerCGO runesmith 1";
        table["note"] = "GENERATED by tools/PluginBuild from config/runesmith.json - do not edit; 0x... = NjordlingerCombatGameplayOverhaul.esp";
        table["socketInfo"] = topicForms["Socket"]!["info"]!.GetValue<string>();
        table["serviceRadius"] = 1500;
        File.WriteAllText(Path.Combine(outDir, "runesmith.json"), table.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
        Console.WriteLine($"runesmith: {npc.Name} in {cellGetter.EditorID} near {spot.what}; quest {quest.Name}; {topicForms.Count} topics");
    }

    // A spot inside the cell: the first reference whose base's editor ID contains one of a_near (in that order).
    static (P3Float pos, P3Float rot, string what) FindSpot(ICellGetter a_cell, ILinkCache a_cache, string[] a_near)
    {
        var refs = a_cell.Persistent.Concat(a_cell.Temporary).OfType<IPlacedObjectGetter>().ToList();
        foreach (var want in a_near)
        {
            foreach (var r in refs)
            {
                if (r.Placement == null) { continue; }
                if (!a_cache.TryResolve(r.Base.FormKey, out var b) || b.EditorID == null) { continue; }
                if (b.EditorID.Contains(want, StringComparison.OrdinalIgnoreCase)) { return (r.Placement.Position, r.Placement.Rotation, b.EditorID); }
            }
        }
        throw new InvalidOperationException($"runesmith: no reference near which to stand in {a_cell.EditorID}");
    }

    // A new interior cell goes into the block and sub-block the engine files it under by its form ID.
    static void AddInteriorCell(SkyrimMod mod, Cell cell)
    {
        var id = (int)cell.FormKey.ID;
        var blockNo = id % 10;
        var subNo = (id / 10) % 10;
        var block = mod.Cells.Records.FirstOrDefault(b => b.BlockNumber == blockNo);
        if (block == null) { block = new CellBlock { BlockNumber = blockNo, GroupType = GroupTypeEnum.InteriorCellBlock }; mod.Cells.Records.Add(block); }
        var sub = block.SubBlocks.FirstOrDefault(s => s.BlockNumber == subNo);
        if (sub == null) { sub = new CellSubBlock { BlockNumber = subNo, GroupType = GroupTypeEnum.InteriorCellSubBlock }; block.SubBlocks.Add(sub); }
        sub.Cells.Add(cell);
    }
}
