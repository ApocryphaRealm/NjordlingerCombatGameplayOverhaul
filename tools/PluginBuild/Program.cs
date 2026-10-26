// Njordlinger Combat Gameplay Overhaul - the plugin build (PLAN.md phase 2 steps 2 and 5): writes
// NjordlingerCombatGameplayOverhaul.esp (ESL-flagged, form version 44, master Skyrim.esm only) from config/classes.json and
// config/class-content.json, and generated/plugin-forms.json with the local form ID of every record, which
// tools/build-classes.py puts into the shipped classes.json for the DLL.
//
//   dotnet run --project tools/PluginBuild -- <repo folder>
//
// Records: three group globals (NJCGO_GroupWarrior / Magic / Covert, short, 1 = open), one global and one keyword per
// class (NJCGO_Class_<id>), and a PERK for every class ability, title ability and rank perk (CLASS-CONTENT.md sections 2
// and 3): NJCGO_Ab_<id>, NJCGO_Title_<id>, NJCGO_Rank_<class>_<A1..B3>. A perk's constant effects ride a hidden ability
// spell (NJCGO_AbSp_*, one Value Modifier magic effect per effect, each carrying the class keyword where there is one);
// its entry points are perk entry points with their conditions. No scripts; nothing in the game is overridden.
using System.Text.Json;
using System.Text.Json.Nodes;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;
using Noggog;

var repo = args.Length > 0 ? args[0] : Directory.GetCurrentDirectory();
var classes = JsonNode.Parse(File.ReadAllText(Path.Combine(repo, "config", "classes.json")))!;
var content = JsonNode.Parse(File.ReadAllText(Path.Combine(repo, "config", "class-content.json")))!;

var modKey = ModKey.FromFileName("NjordlingerCombatGameplayOverhaul.esp");
var skyrim = ModKey.FromFileName("Skyrim.esm");
var mod = new SkyrimMod(modKey, SkyrimRelease.SkyrimSE);
mod.ModHeader.Flags |= SkyrimModHeader.HeaderFlag.Small;
mod.ModHeader.Author = "ApocryphaRealm";
mod.ModHeader.Description = "Njordlinger Combat Gameplay Overhaul - class globals, keywords, abilities and rank perks.";

var keywords = new Dictionary<string, FormKey>();
foreach (var (name, id) in content["keywords"]!.AsObject())
{
    keywords[name] = new FormKey(skyrim, Convert.ToUInt32(id!.GetValue<string>(), 16));
}

string Hex(IMajorRecordGetter r) => $"0x{r.FormKey.ID:X3}";
var stubs = new List<string>();
var approx = new List<string>();
int entryPoints = 0, constantEffects = 0;

CompareOperator Op(string? op) => op switch
{
    null or "==" => CompareOperator.EqualTo,
    "!=" => CompareOperator.NotEqualTo,
    ">" => CompareOperator.GreaterThan,
    ">=" => CompareOperator.GreaterThanOrEqualTo,
    "<" => CompareOperator.LessThan,
    "<=" => CompareOperator.LessThanOrEqualTo,
    _ => throw new InvalidOperationException("unknown operator " + op)
};

Condition MakeCondition(JsonNode c, string owner)
{
    var f = c["f"]!.GetValue<string>();
    var v = c["v"]?.GetValue<float>() ?? 1.0f;
    IFormLinkGetter<IKeywordGetter> Kw()
    {
        var name = c["kw"]!.GetValue<string>();
        if (!keywords.TryGetValue(name, out var fk)) { throw new InvalidOperationException($"{owner}: keyword {name} is not in class-content.json's keyword list"); }
        return new FormLink<IKeywordGetter>(fk);
    }
    ConditionData data = f switch
    {
        "IsSneaking" => new IsSneakingConditionData(),
        "IsPowerAttacking" => new IsPowerAttackingConditionData(),
        "IsBlocking" => new IsBlockingConditionData(),
        "IsDualCasting" => new IsDualCastingConditionData(),
        "IsSprinting" => new IsSprintingConditionData(),
        "IsWeaponOut" => new IsWeaponOutConditionData(),
        "IsStaggered" => new IsStaggeredConditionData(),
        "IsFleeing" => new IsFleeingConditionData(),
        "GetKnockedState" => new GetKnockedStateConditionData(),
        "GetHealthPercentage" => new GetHealthPercentageConditionData(),
        "GetRandomPercent" => new GetRandomPercentConditionData(),
        "GetEquippedItemType" => SetHand(new GetEquippedItemTypeConditionData(), c["hand"]!.GetValue<string>() == "left" ? CastSource.Left : CastSource.Right),
        "HasKeyword" => MakeKeywordData(new HasKeywordConditionData(), Kw()),
        "WornHasKeyword" => MakeKeywordData(new WornHasKeywordConditionData(), Kw()),
        "HasMagicEffectKeyword" => MakeKeywordData(new HasMagicEffectKeywordConditionData(), Kw()),
        "SpellHasKeyword" => MakeKeywordData(new EPMagic_SpellHasKeywordConditionData(), Kw()),
        "EPMagic_SpellHasSkill" => new EPMagic_SpellHasSkillConditionData { ActorValue = Enum.Parse<ActorValue>(c["skill"]!.GetValue<string>()) },
        _ => throw new InvalidOperationException($"{owner}: condition {f} is not supported by the plugin build")
    };
    data.RunOnType = Condition.RunOnType.Subject;
    var cond = new ConditionFloat { CompareOperator = Op(c["op"]?.GetValue<string>()), ComparisonValue = v, Data = data };
    if (c["or"]?.GetValue<bool>() == true) { cond.Flags |= Condition.Flag.OR; }
    return cond;
}

// Sets the hand (CastSource) parameter of GetEquippedItemType, whichever property name this Mutagen version gives it.
ConditionData SetHand(ConditionData d, CastSource hand)
{
    var prop = d.GetType().GetProperties().FirstOrDefault(p => p.PropertyType == typeof(CastSource) && p.CanWrite)
               ?? throw new InvalidOperationException("no CastSource parameter on " + d.GetType().Name + ": " + string.Join(",", d.GetType().GetProperties().Select(p => p.Name + ":" + p.PropertyType.Name)));
    prop.SetValue(d, hand);
    return d;
}

// Sets the keyword parameter of a keyword condition, whichever property name this Mutagen version gives it.
ConditionData MakeKeywordData(ConditionData d, IFormLinkGetter<IKeywordGetter> kw)
{
    var prop = d.GetType().GetProperties().First(p => p.Name.Contains("Keyword") && p.CanWrite);
    var current = prop.GetValue(d);
    if (current is IFormLinkOrIndex<IKeywordGetter> fli) { fli.Link.SetTo(kw.FormKey); }
    else if (current is IFormLink<IKeywordGetter> fl) { fl.SetTo(kw.FormKey); }
    else { throw new InvalidOperationException("cannot set the keyword of " + d.GetType().Name); }
    return d;
}

// One PERK for an ability / title / rank perk definition. a_keyword: the class keyword carried by its magic effects.
BGSPerkBuild_Result BGSPerkBuild(string editorId, string name, string description, JsonNode def, Keyword? keyword)
{
    var perk = mod.Perks.AddNew(editorId);
    perk.Name = name;
    perk.Description = description;
    perk.Playable = false;
    perk.Hidden = true;
    perk.NumRanks = 1;
    if (def["stub"] is JsonNode st) { stubs.Add($"{editorId}: {st.GetValue<string>()}"); }
    if (def["approx"] is JsonNode ap) { approx.Add($"{editorId}: {ap.GetValue<string>()}"); }
    Spell? spell = null;
    var n = 0;
    foreach (var e in def["effects"]!.AsArray())
    {
        if (e!["av"] is JsonNode avNode)
        {
            spell ??= MakeAbilitySpell(editorId.Replace("NJCGO_", "NJCGO_AbSp_"), name);
            var mgef = mod.MagicEffects.AddNew($"{editorId.Replace("NJCGO_", "NJCGO_Fx_")}_{n++}");
            mgef.Name = name;
            mgef.Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.ValueModifier) { ActorValue = Enum.Parse<ActorValue>(avNode.GetValue<string>()) };
            mgef.CastType = CastType.ConstantEffect;
            mgef.TargetType = TargetType.Self;
            mgef.Flags = MagicEffect.Flag.Recover | MagicEffect.Flag.NoArea | MagicEffect.Flag.NoDuration | MagicEffect.Flag.HideInUI;
            mgef.MagicSkill = ActorValue.None;
            mgef.ResistValue = ActorValue.None;
            mgef.SecondActorValue = ActorValue.None;
            if (keyword != null) { mgef.Keywords = new ExtendedList<IFormLinkGetter<IKeywordGetter>> { keyword.ToLink() }; }
            var effect = new Effect { BaseEffect = mgef.ToNullableLink(), Data = new EffectData { Magnitude = e["add"]!.GetValue<float>(), Area = 0, Duration = 0 } };
            if (e["if"] is JsonArray conds) { foreach (var c in conds) { effect.Conditions.Add(MakeCondition(c!, editorId)); } }
            spell.Effects.Add(effect);
            ++constantEffects;
        }
        else if (e["ep"] is JsonNode epNode)
        {
            var ep = Enum.Parse<APerkEntryPointEffect.EntryType>(epNode.GetValue<string>());
            var mv = new PerkEntryPointModifyValue
            {
                EntryPoint = ep,
                Modification = e["add"] != null ? PerkEntryPointModifyValue.ModificationType.Add : PerkEntryPointModifyValue.ModificationType.Multiply,
                Value = e["add"]?.GetValue<float>() ?? e["mult"]!.GetValue<float>(),
                PerkConditionTabCount = TabCount(ep),
            };
            if (e["if"] is JsonArray conds)
            {
                foreach (var group in conds.GroupBy(c => c!["tab"]?.GetValue<int>() ?? 0))
                {
                    var pc = new PerkCondition { RunOnTabIndex = (byte)group.Key };
                    foreach (var c in group) { pc.Conditions.Add(MakeCondition(c!, editorId)); }
                    mv.Conditions.Add(pc);
                }
            }
            perk.Effects.Add(mv);
            ++entryPoints;
        }
    }
    if (spell != null) { perk.Effects.Add(new PerkAbilityEffect { Ability = spell.ToLink() }); }
    return new BGSPerkBuild_Result(perk);
}

Spell MakeAbilitySpell(string editorId, string name)
{
    var spell = mod.Spells.AddNew(editorId);
    spell.Name = name;
    spell.Type = SpellType.Ability;
    spell.CastType = CastType.ConstantEffect;
    spell.TargetType = TargetType.Self;
    spell.Description = "";
    return spell;
}

// The number of condition tabs each entry point has in the game (the perk's tabs: owner, then the entry point's own).
byte TabCount(APerkEntryPointEffect.EntryType ep) => ep switch
{
    APerkEntryPointEffect.EntryType.ModAttackDamage or APerkEntryPointEffect.EntryType.ModPowerAttackDamage or
    APerkEntryPointEffect.EntryType.ModSneakAttackMult or APerkEntryPointEffect.EntryType.ModIncomingDamage or
    APerkEntryPointEffect.EntryType.ModTargetStagger or APerkEntryPointEffect.EntryType.ModIncomingSpellMagnitude => 3,
    APerkEntryPointEffect.EntryType.ModSpellCost or APerkEntryPointEffect.EntryType.ModSpellMagnitude or
    APerkEntryPointEffect.EntryType.ModSpellDuration or APerkEntryPointEffect.EntryType.ModArmorRating or
    APerkEntryPointEffect.EntryType.ModPowerAttackStamina or APerkEntryPointEffect.EntryType.ModIncomingStagger or
    APerkEntryPointEffect.EntryType.ModLockpickSweetSpot or APerkEntryPointEffect.EntryType.ModBribeAmount or
    APerkEntryPointEffect.EntryType.ModPoisonDoseCount => 2,
    APerkEntryPointEffect.EntryType.ModPickpocketChance => 3,
    _ => 1
};

var forms = new JsonObject();
var groupForms = new JsonObject();
foreach (var g in new[] { "warrior", "magic", "covert" })
{
    var glob = new GlobalShort(mod) { EditorID = "NJCGO_Group" + char.ToUpper(g[0]) + g[1..], Data = 1 };
    mod.Globals.Add(glob);
    groupForms[g] = Hex(glob);
}
forms["groupGlobals"] = groupForms;

var abilities = content["abilities"]!.AsObject();
var ranks = content["ranks"]!.AsObject();
var classForms = new JsonObject();
foreach (var c in classes["classes"]!.AsArray())
{
    var id = c!["id"]!.GetValue<string>();
    var entry = new JsonObject();
    var glob = new GlobalShort(mod) { EditorID = "NJCGO_Class_" + id, Data = 0 };
    mod.Globals.Add(glob);
    entry["global"] = Hex(glob);
    var kw = mod.Keywords.AddNew("NJCGO_Class_" + id);
    entry["keyword"] = Hex(kw);
    if (abilities[id] is JsonNode ab)
    {
        var r = BGSPerkBuild("NJCGO_Ab_" + id, ab["name"]!.GetValue<string>(), c["description"]!.GetValue<string>(), ab, kw);
        entry["ability"] = Hex(r.Perk);
    }
    else { stubs.Add($"{id}: no ability in class-content.json"); }
    if (ranks[id] is JsonObject rk)
    {
        var rankForms = new JsonObject();
        foreach (var slot in new[] { "A1", "B1", "A2", "B2", "A3", "B3" })
        {
            var def = rk[slot] ?? throw new InvalidOperationException($"{id}: rank perk {slot} is missing");
            var r = BGSPerkBuild($"NJCGO_Rank_{id}_{slot}", def["name"]!.GetValue<string>(), def["description"]!.GetValue<string>(), def, kw);
            rankForms[slot] = Hex(r.Perk);
        }
        entry["ranks"] = rankForms;
    }
    classForms[id] = entry;
}
forms["classes"] = classForms;

var titleForms = new JsonObject();
foreach (var t in classes["titles"]!.AsArray())
{
    var id = t!["id"]!.GetValue<string>();
    if (abilities[id] is JsonNode ab)
    {
        var r = BGSPerkBuild("NJCGO_Title_" + id, ab["name"]!.GetValue<string>(), t["description"]!.GetValue<string>(), ab, null);
        titleForms[id] = Hex(r.Perk);
    }
}
forms["titles"] = titleForms;
forms["stubs"] = new JsonArray(stubs.Select(s => (JsonNode)JsonValue.Create(s)!).ToArray());
forms["approximations"] = new JsonArray(approx.Select(s => (JsonNode)JsonValue.Create(s)!).ToArray());

var esp = Path.Combine(repo, "data", "NjordlingerCombatGameplayOverhaul.esp");
mod.WriteToBinary(esp);
Directory.CreateDirectory(Path.Combine(repo, "generated"));
File.WriteAllText(Path.Combine(repo, "generated", "plugin-forms.json"), forms.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
Console.WriteLine($"wrote {esp}: {mod.Globals.Count} globals, {mod.Keywords.Count} keywords, {mod.Perks.Count} perks ({entryPoints} entry points), {mod.Spells.Count} ability spells ({constantEffects} constant effects)");
Console.WriteLine($"{stubs.Count} stubs, {approx.Count} approximations - generated/plugin-forms.json lists them");

record BGSPerkBuild_Result(Perk Perk);
