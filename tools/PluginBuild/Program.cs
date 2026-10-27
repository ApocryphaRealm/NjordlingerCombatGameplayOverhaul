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
var helperGlobals = new Dictionary<string, GlobalShort>();
foreach (var g in content["helpers"]?["globals"]?.AsArray() ?? new JsonArray())
{
    var name = g!.GetValue<string>();
    var glob = new GlobalShort(mod) { EditorID = name, Data = 0 };
    mod.Globals.Add(glob);
    helperGlobals[name] = glob;
}
// Phase 3 (gear): the plugin's own keywords by editor ID, and the keyword '@unique' stands for while a unique is built.
var ownKeywords = new Dictionary<string, FormKey>();
var races = new Dictionary<string, FormKey>();   // gear-content.json "races" (Skyrim.esm), for GetIsRace
string? currentUniqueKeyword = null;
var dllDone = new List<string>();
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
        if (name == "@unique") { name = currentUniqueKeyword ?? throw new InvalidOperationException($"{owner}: @unique used outside a unique"); }
        if (ownKeywords.TryGetValue(name, out var own)) { return new FormLink<IKeywordGetter>(own); }
        if (!keywords.TryGetValue(name, out var fk)) { throw new InvalidOperationException($"{owner}: keyword {name} is not in class-content.json's or gear-content.json's keyword list"); }
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
        "GetLightLevel" => new GetLightLevelConditionData(),
        "GetIsRace" => SetRace(new GetIsRaceConditionData(), races[c["race"]!.GetValue<string>()]),
        "GetGlobalValue" => SetGlobal(new GetGlobalValueConditionData(), helperGlobals[c["global"]!.GetValue<string>()]),
        // phase 4 (soul stones)
        "GetActorValue" => new GetActorValueConditionData { ActorValue = Enum.Parse<ActorValue>(c["av"]!.GetValue<string>()) },
        "IsRaining" => new IsRainingConditionData(),
        "IsSnowing" => new IsSnowingConditionData(),
        "IsPleasant" => new IsPleasantConditionData(),
        _ => throw new InvalidOperationException($"{owner}: condition {f} is not supported by the plugin build")
    };
    data.RunOnType = Condition.RunOnType.Subject;
    var cond = new ConditionFloat { CompareOperator = Op(c["op"]?.GetValue<string>()), ComparisonValue = v, Data = data };
    if (c["or"]?.GetValue<bool>() == true) { cond.Flags |= Condition.Flag.OR; }
    return cond;
}

// Sets the global parameter of GetGlobalValue, whichever property name this Mutagen version gives it.
ConditionData SetGlobal(ConditionData d, GlobalShort g)
{
    var prop = d.GetType().GetProperties().First(p => p.Name.Contains("Global") && p.CanWrite);
    var current = prop.GetValue(d);
    if (current is IFormLinkOrIndex<IGlobalGetter> fli) { fli.Link.SetTo(g.FormKey); }
    else if (current is IFormLink<IGlobalGetter> fl) { fl.SetTo(g.FormKey); }
    else { throw new InvalidOperationException("cannot set the global of " + d.GetType().Name); }
    return d;
}

// Sets the race parameter of GetIsRace, whichever link type this Mutagen version gives it.
ConditionData SetRace(ConditionData d, FormKey fk)
{
    var prop = d.GetType().GetProperties().First(p => p.Name.Contains("Race") && p.CanWrite);
    var current = prop.GetValue(d);
    if (current is IFormLinkOrIndex<IRaceGetter> a) { a.Link.SetTo(fk); }
    else if (current is IFormLink<IRaceGetter> b) { b.SetTo(fk); }
    else { throw new InvalidOperationException("cannot set the race of " + d.GetType().Name + " (" + current?.GetType().FullName + ")"); }
    return d;
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
    if (def["dll"] is JsonNode dl) { dllDone.Add($"{editorId}: {dl.GetValue<string>()}"); }
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

var helperForms = new JsonObject();
foreach (var (name, g) in helperGlobals) { helperForms[name] = Hex(g); }
foreach (var (id, def) in content["helpers"]?["spells"]?.AsObject() ?? new JsonObject())
{
    var isAbility = def!["ability"]?.GetValue<bool>() == true;
    var spell = mod.Spells.AddNew("NJCGO_FxSp_" + id);
    spell.Name = def["name"]!.GetValue<string>();
    spell.Type = isAbility ? SpellType.Ability : SpellType.Spell;
    spell.CastType = isAbility ? CastType.ConstantEffect : CastType.FireAndForget;
    spell.TargetType = isAbility ? TargetType.Self : TargetType.TargetActor;
    spell.Description = "";
    var k = 0;
    foreach (var e in def["effects"]!.AsArray())
    {
        var arch = Enum.Parse<MagicEffectArchetype.TypeEnum>(e!["archetype"]!.GetValue<string>());
        var mgef = mod.MagicEffects.AddNew($"NJCGO_FxEf_{id}_{k++}");
        mgef.Name = spell.Name.String;
        mgef.Archetype = new MagicEffectArchetype(arch);
        if (e["av"] is JsonNode avn) { mgef.Archetype.ActorValue = Enum.Parse<ActorValue>(avn.GetValue<string>()); }
        var duration = e["duration"]?.GetValue<int>() ?? 0;
        mgef.CastType = isAbility ? CastType.ConstantEffect : CastType.FireAndForget;
        mgef.TargetType = isAbility ? TargetType.Self : TargetType.TargetActor;
        mgef.Flags = MagicEffect.Flag.NoArea | MagicEffect.Flag.Recover;
        if (duration == 0 && !isAbility) { mgef.Flags = MagicEffect.Flag.NoArea | MagicEffect.Flag.NoDuration; }
        if (e["detrimental"]?.GetValue<bool>() == true) { mgef.Flags |= MagicEffect.Flag.Detrimental; }
        if (e["hostile"]?.GetValue<bool>() == true) { mgef.Flags |= MagicEffect.Flag.Hostile; }
        mgef.MagicSkill = ActorValue.None;
        mgef.ResistValue = ActorValue.None;
        mgef.SecondActorValue = ActorValue.None;
        spell.Effects.Add(new Effect { BaseEffect = mgef.ToNullableLink(), Data = new EffectData { Magnitude = e["magnitude"]!.GetValue<float>(), Area = 0, Duration = duration } });
    }
    helperForms[id] = Hex(spell);
}
forms["helpers"] = helperForms;

// ---- Phase 3: Gear (PLAN.md D14 / D15, config/gear-content.json, GEAR-CONTENT.md) ---------------------------------------
// Built AFTER every class record, so no form ID of phase 2 moves. Records: two module globals (NJCGO_Gear_SetsOn /
// _UniquesOn, the Gear tab's switches), the five set keywords (the editor IDs KID's set rules already use, so KID finds
// these instead of creating its own), NJCGO_Tier_Unique (the tier rules' unique keyword, for the same reason) and
// NJCGO_NoEnchant, one keyword per authored unique; per set two ability spells (2 and 4 pieces - the SKSE plugin adds and
// removes them by piece count) and one perk of entry points (SPID, every actor); per unique one perk of entry points
// carrying one ability spell (SPID, every actor). Also writes data/NjordlingerCGO_Gear_DISTR.ini and
// data/NjordlingerCGO_Uniques_KID.ini.
var gearPath = Path.Combine(repo, "config", "gear-content.json");
if (File.Exists(gearPath))
{
    var gear = JsonNode.Parse(File.ReadAllText(gearPath))!;
    var setsCfg = JsonNode.Parse(File.ReadAllText(Path.Combine(repo, "config", "sets.json")), null, new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true })!.AsArray();
    foreach (var (name, id) in gear["keywords"]!.AsObject())
    {
        keywords[name] = new FormKey(skyrim, Convert.ToUInt32(id!.GetValue<string>(), 16));
    }
    foreach (var (name, id) in gear["races"]?.AsObject() ?? new JsonObject())
    {
        races[name] = new FormKey(skyrim, Convert.ToUInt32(id!.GetValue<string>(), 16));
    }
    var setsOn = new GlobalShort(mod) { EditorID = "NJCGO_Gear_SetsOn", Data = 1 };
    mod.Globals.Add(setsOn);
    var uniquesOn = new GlobalShort(mod) { EditorID = "NJCGO_Gear_UniquesOn", Data = 1 };
    mod.Globals.Add(uniquesOn);
    var tierUnique = mod.Keywords.AddNew("NJCGO_Tier_Unique");
    var noEnchant = mod.Keywords.AddNew("NJCGO_NoEnchant");
    ownKeywords["NJCGO_Tier_Unique"] = tierUnique.FormKey;
    ownKeywords["NJCGO_NoEnchant"] = noEnchant.FormKey;

    // The spell parameter of HasSpell, whichever link type this Mutagen version gives it.
    ConditionData SetSpell(ConditionData d, FormKey fk)
    {
        var prop = d.GetType().GetProperties().First(p => p.Name.Contains("Spell") && p.CanWrite);
        var current = prop.GetValue(d);
        if (current is IFormLinkOrIndex<ISpellRecordGetter> a) { a.Link.SetTo(fk); }
        else if (current is IFormLink<ISpellRecordGetter> b) { b.SetTo(fk); }
        else if (current is IFormLinkOrIndex<ISpellGetter> c) { c.Link.SetTo(fk); }
        else if (current is IFormLink<ISpellGetter> e) { e.SetTo(fk); }
        else { throw new InvalidOperationException("cannot set the spell of " + d.GetType().Name + " (" + current?.GetType().FullName + ")"); }
        return d;
    }
    Condition Gate(ConditionData d) { d.RunOnType = Condition.RunOnType.Subject; return new ConditionFloat { CompareOperator = CompareOperator.EqualTo, ComparisonValue = 1.0f, Data = d }; }
    Condition GlobalOn(GlobalShort g) => Gate(SetGlobal(new GetGlobalValueConditionData(), g));
    Condition Worn(FormKey kw) => Gate(MakeKeywordData(new WornHasKeywordConditionData(), new FormLink<IKeywordGetter>(kw)));
    Condition HasSpellOf(Spell s) => Gate(SetSpell(new HasSpellConditionData(), s.FormKey));

    int gearEntries = 0, gearConstants = 0;
    // One visible ability (no HideInUI: it shows in Active Effects under its name) of the constant effects in a_effects.
    Spell GearAbility(string editorId, string name, string description, JsonArray a_effects, Func<List<Condition>> gate)
    {
        var spell = MakeAbilitySpell(editorId, name);
        spell.Description = description;
        var n = 0;
        foreach (var e in a_effects.Where(e => e!["av"] != null))
        {
            var mgef = mod.MagicEffects.AddNew($"{editorId.Replace("NJCGO_", "NJCGO_GFx_")}_{n++}");
            mgef.Name = name;
            mgef.Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.ValueModifier) { ActorValue = Enum.Parse<ActorValue>(e!["av"]!.GetValue<string>()) };
            mgef.CastType = CastType.ConstantEffect;
            mgef.TargetType = TargetType.Self;
            mgef.Flags = MagicEffect.Flag.Recover | MagicEffect.Flag.NoArea | MagicEffect.Flag.NoDuration;
            mgef.MagicSkill = ActorValue.None;
            mgef.ResistValue = ActorValue.None;
            mgef.SecondActorValue = ActorValue.None;
            var effect = new Effect { BaseEffect = mgef.ToNullableLink(), Data = new EffectData { Magnitude = e["add"]!.GetValue<float>(), Area = 0, Duration = 0 } };
            foreach (var g in gate()) { effect.Conditions.Add(g); }
            if (e["if"] is JsonArray conds) { foreach (var c in conds) { effect.Conditions.Add(MakeCondition(c!, editorId)); } }
            spell.Effects.Add(effect);
            ++gearConstants;
        }
        if (n == 0) { throw new InvalidOperationException($"{editorId}: a gear ability needs at least one constant (av) effect - it is what the player sees"); }
        return spell;
    }
    // The entry points of a_effects onto a_perk; tab 0 (the perk owner) gets the gate conditions first.
    void GearEntries(Perk perk, JsonArray a_effects, Func<List<Condition>> gate, string owner)
    {
        foreach (var e in a_effects.Where(e => e!["ep"] != null))
        {
            var ep = Enum.Parse<APerkEntryPointEffect.EntryType>(e!["ep"]!.GetValue<string>());
            var mv = new PerkEntryPointModifyValue
            {
                EntryPoint = ep,
                Modification = e["add"] != null ? PerkEntryPointModifyValue.ModificationType.Add : PerkEntryPointModifyValue.ModificationType.Multiply,
                Value = e["add"]?.GetValue<float>() ?? e["mult"]!.GetValue<float>(),
                PerkConditionTabCount = TabCount(ep),
            };
            var byTab = new SortedDictionary<int, List<Condition>> { [0] = gate() };
            if (e["if"] is JsonArray conds)
            {
                foreach (var c in conds)
                {
                    var tab = c!["tab"]?.GetValue<int>() ?? 0;
                    if (!byTab.TryGetValue(tab, out var list)) { byTab[tab] = list = new List<Condition>(); }
                    list.Add(MakeCondition(c, owner));
                }
            }
            foreach (var (tab, list) in byTab)
            {
                var pc = new PerkCondition { RunOnTabIndex = (byte)tab };
                foreach (var c in list) { pc.Conditions.Add(c); }
                mv.Conditions.Add(pc);
            }
            perk.Effects.Add(mv);
            ++gearEntries;
        }
    }
    Perk GearPerk(string editorId, string name, string description)
    {
        var perk = mod.Perks.AddNew(editorId);
        perk.Name = name;
        perk.Description = description;
        perk.Playable = false;
        perk.Hidden = true;
        perk.NumRanks = 1;
        return perk;
    }
    string Distr(IMajorRecordGetter r) => $"0x{r.FormKey.ID:X}~{modKey.FileName}";

    var gearForms = new JsonObject
    {
        ["globals"] = new JsonObject { ["setsOn"] = Hex(setsOn), ["uniquesOn"] = Hex(uniquesOn) },
        ["keywords"] = new JsonObject { ["tierUnique"] = Hex(tierUnique), ["noEnchant"] = Hex(noEnchant) },
    };
    var distr = new List<string>
    {
        "; Njordlinger Combat Gameplay Overhaul - gear effects on actors (GENERATED by tools\\PluginBuild from config\\gear-content.json).",
        "; PLAN.md D14 / D15 / D16: every actor carries the perks; their conditions (the worn keyword, the module switch, and for a",
        "; set the 2- or 4-piece ability the SKSE plugin adds by piece count) decide when an effect applies.",
        ""
    };
    var setForms = new JsonArray();
    foreach (var s in setsCfg)
    {
        var setName = s!["name"]!.GetValue<string>();
        var kwId = s["keyword"]!.GetValue<string>();
        var def = gear["sets"]?[setName] ?? throw new InvalidOperationException($"gear-content.json has no bonuses for the set {setName}");
        var kw = mod.Keywords.AddNew(kwId);
        ownKeywords[kwId] = kw.FormKey;
        var stem = "NJCGO_Set" + new string(setName.Where(char.IsLetterOrDigit).ToArray());
        var ab2 = GearAbility(stem + "_2", def["2"]!["name"]!.GetValue<string>(), def["2"]!["description"]!.GetValue<string>(), def["2"]!["effects"]!.AsArray(), () => new() { GlobalOn(setsOn), Worn(kw.FormKey) });
        var ab4 = GearAbility(stem + "_4", def["4"]!["name"]!.GetValue<string>(), def["4"]!["description"]!.GetValue<string>(), def["4"]!["effects"]!.AsArray(), () => new() { GlobalOn(setsOn), Worn(kw.FormKey) });
        var perk = GearPerk(stem + "_Perk", setName, $"{def["2"]!["description"]!.GetValue<string>()} {def["4"]!["description"]!.GetValue<string>()}");
        GearEntries(perk, def["2"]!["effects"]!.AsArray(), () => new() { GlobalOn(setsOn), Worn(kw.FormKey), HasSpellOf(ab2) }, stem + "_2");
        GearEntries(perk, def["4"]!["effects"]!.AsArray(), () => new() { GlobalOn(setsOn), Worn(kw.FormKey), HasSpellOf(ab4) }, stem + "_4");
        var entry = new JsonObject { ["name"] = setName, ["keyword"] = Hex(kw), ["ability2"] = Hex(ab2), ["ability4"] = Hex(ab4) };
        if (perk.Effects.Count > 0)
        {
            entry["perk"] = Hex(perk);
            distr.Add($"; {setName} set ({def["trigger"]?.GetValue<string>() ?? ""}) - entry points at 2 and 4 pieces");
            distr.Add($"Perk = {Distr(perk)}");
        }
        else { mod.Perks.Remove(perk.FormKey); }
        setForms.Add(entry);
    }
    gearForms["sets"] = setForms;

    var kid = new List<string>
    {
        "; Njordlinger Combat Gameplay Overhaul - unique items (GENERATED by tools\\PluginBuild from config\\gear-content.json).",
        "; PLAN.md D15: every unique-tier item cannot be enchanted (NJCGO_NoEnchant and the game's own MagicDisallowEnchanting,",
        "; given to every item KID's tier rules mark NJCGO_Tier_Unique - the SKSE plugin repeats this at load for any item KID",
        "; missed); each authored unique gets its own keyword, which its SPID perk's conditions read.",
        "",
        "Keyword = NJCGO_NoEnchant|Weapon|NJCGO_Tier_Unique",
        "Keyword = NJCGO_NoEnchant|Armor|NJCGO_Tier_Unique",
        "Keyword = MagicDisallowEnchanting|Weapon|NJCGO_Tier_Unique",
        "Keyword = MagicDisallowEnchanting|Armor|NJCGO_Tier_Unique",
        ""
    };
    distr.Add("");
    var uniqueForms = new JsonArray();
    foreach (var u in gear["uniques"]!.AsArray())
    {
        var uid = u!["id"]!.GetValue<string>();
        var uname = u["name"]!.GetValue<string>();
        var udesc = u["description"]!.GetValue<string>();
        var kw = mod.Keywords.AddNew("NJCGO_Unique_" + uid);
        ownKeywords[kw.EditorID!] = kw.FormKey;
        currentUniqueKeyword = kw.EditorID;
        // 2026-10-26 (retest 70e6): the SKSE plugin adds and removes a unique's ability by what the actor has equipped, like
        // the set abilities, so the ability carries only the module switch (the perk keeps its worn-keyword entry points).
        var ab = GearAbility("NJCGO_UniAb_" + uid, uname, udesc, u["effects"]!.AsArray(), () => new() { GlobalOn(uniquesOn) });
        var perk = GearPerk("NJCGO_UniPerk_" + uid, uname, udesc);
        GearEntries(perk, u["effects"]!.AsArray(), () => new() { GlobalOn(uniquesOn), Worn(kw.FormKey) }, "NJCGO_UniPerk_" + uid);
        if (perk.Effects.Count == 0) { perk.Description = udesc; }   // a unique with no entry point keeps an empty perk (SPID line stays harmless)
        currentUniqueKeyword = null;
        if (u["dll"] is JsonNode udll) { dllDone.Add($"NJCGO_UniPerk_{uid}: {udll.GetValue<string>()}"); }
        var refs = u["forms"]!.AsArray().Select(f =>
        {
            var parts = f!.GetValue<string>().Split('|');
            return $"0x{Convert.ToUInt32(parts[1], 16):X}~{parts[0]}";
        });
        kid.Add($"; {uname}");
        kid.Add($"Keyword = NJCGO_Unique_{uid}|{u["type"]!.GetValue<string>()}|{string.Join(",", refs)}");
        distr.Add($"; {uname}");
        distr.Add($"Perk = {Distr(perk)}");
        uniqueForms.Add(new JsonObject { ["id"] = uid, ["name"] = uname, ["keyword"] = Hex(kw), ["perk"] = Hex(perk), ["ability"] = Hex(ab) });
    }
    gearForms["uniques"] = uniqueForms;
    // Gear helper spells the SKSE plugin casts (Spellbreaker's silence), built last so no earlier form ID moves.
    var gearHelpers = new JsonObject();
    foreach (var (hid, def) in gear["helpers"]?.AsObject() ?? new JsonObject())
    {
        var spell = mod.Spells.AddNew("NJCGO_GearFx_" + char.ToUpper(hid[0]) + hid[1..]);
        spell.Name = def!["name"]!.GetValue<string>();
        spell.Type = SpellType.Spell;
        spell.CastType = CastType.FireAndForget;
        spell.TargetType = TargetType.TargetActor;
        spell.Description = "";
        var k = 0;
        foreach (var e in def["effects"]!.AsArray())
        {
            var mgef = mod.MagicEffects.AddNew($"NJCGO_GearFxEf_{hid}_{k++}");
            mgef.Name = spell.Name.String;
            mgef.Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.ValueModifier) { ActorValue = Enum.Parse<ActorValue>(e!["av"]!.GetValue<string>()) };
            var duration = e["duration"]?.GetValue<int>() ?? 0;
            mgef.CastType = CastType.FireAndForget;
            mgef.TargetType = TargetType.TargetActor;
            mgef.Flags = MagicEffect.Flag.NoArea | MagicEffect.Flag.Detrimental | MagicEffect.Flag.Hostile | (duration == 0 ? MagicEffect.Flag.NoDuration : MagicEffect.Flag.Recover);
            mgef.MagicSkill = ActorValue.None;
            mgef.ResistValue = ActorValue.None;
            mgef.SecondActorValue = ActorValue.None;
            spell.Effects.Add(new Effect { BaseEffect = mgef.ToNullableLink(), Data = new EffectData { Magnitude = e["magnitude"]!.GetValue<float>(), Area = 0, Duration = duration } });
        }
        gearHelpers[hid] = Hex(spell);
    }
    gearForms["helpers"] = gearHelpers;
    forms["gear"] = gearForms;
    File.WriteAllText(Path.Combine(repo, "data", "NjordlingerCGO_Gear_DISTR.ini"), string.Join("\r\n", distr) + "\r\n");
    File.WriteAllText(Path.Combine(repo, "data", "NjordlingerCGO_Uniques_KID.ini"), string.Join("\r\n", kid) + "\r\n");
    Console.WriteLine($"gear: {setForms.Count} sets, {uniqueForms.Count} uniques, {gearEntries} entry points, {gearConstants} constant effects; wrote NjordlingerCGO_Gear_DISTR.ini and NjordlingerCGO_Uniques_KID.ini");
}

// ---- Phase 4: Soul Stones (PLAN.md D17-D22, config/stones.json, WORD-TABLE.md) ------------------------------------------
// Built AFTER every phase 2 and 3 record, so no earlier form ID moves. Needs the game's masters to copy Shout visuals,
// resolve vanilla keywords, spells, soul gems and loot lists: NJCGO_MASTERS = folders holding Skyrim.esm, Dawnguard.esm
// and Dragonborn.esm (separated by ';'). Writes data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/stones.json (the SKSE
// plugin's table), data/SKSE/Plugins/SkyPatcher/leveledList/NjordlingerCGO/SoulStones.ini (loot and merchants) and
// data/SKSE/Plugins/InventoryInjector/NjordlingerCGO_StoneIcons.json (soul-gem icon in the tier colour).
var stonesPath = Path.Combine(repo, "config", "stones.json");
var schemePath = Path.Combine(repo, "data", "SKSE", "Plugins", "NjordlingerCombatGameplayOverhaul", "stone-scheme.json");
if (File.Exists(stonesPath))
{
    // Phase 4 step 3: the runesmith, his quest and dialogue (config/runesmith.json), after every stone record and before
    // the soft-linked phrases (StoneBuild calls it at that point, so no earlier form ID moves).
    var runesmithPath = Path.Combine(repo, "config", "runesmith.json");
    StoneBuild.Run(mod, modKey, repo, JsonNode.Parse(File.ReadAllText(stonesPath))!, JsonNode.Parse(File.ReadAllText(schemePath))!,
        keywords, forms, stubs, approx, dllDone, MakeCondition, TabCount,
        () => { if (File.Exists(runesmithPath)) { RunesmithBuild.Run(mod, modKey, repo, JsonNode.Parse(File.ReadAllText(runesmithPath))!, forms, keywords); } },
        () => RunesmithBuild.RunLate(forms));
}

forms["dll"] =new JsonArray(dllDone.Select(s => (JsonNode)JsonValue.Create(s)!).ToArray());
forms["stubs"] = new JsonArray(stubs.Select(s => (JsonNode)JsonValue.Create(s)!).ToArray());
forms["approximations"] = new JsonArray(approx.Select(s => (JsonNode)JsonValue.Create(s)!).ToArray());

var esp = Path.Combine(repo, "data", "NjordlingerCombatGameplayOverhaul.esp");
mod.WriteToBinary(esp);
// The SEQ file (combined run 68e5: Thorvar offered none of his topics): a start-game-enabled quest with dialogue needs
// its form ID in Data\SEQ\<plugin>.seq, or its dialogue never loads. The ID is file-relative: the plugin's own index is
// the number of its masters.
{
    using var written = SkyrimMod.CreateFromBinaryOverlay(esp, SkyrimRelease.SkyrimSE);
    var masterCount = (uint)written.ModHeader.MasterReferences.Count;
    var seqIds = written.Quests.Records.Where(q => q.FormKey.ModKey == modKey && q.Flags.HasFlag(Quest.Flag.StartGameEnabled)).Select(q => (masterCount << 24) | q.FormKey.ID).ToList();
    var seqDir = Path.Combine(repo, "data", "SEQ");
    if (seqIds.Count > 0)
    {
        Directory.CreateDirectory(seqDir);
        using var fs = File.Create(Path.Combine(seqDir, Path.ChangeExtension(modKey.FileName, ".seq")));
        foreach (var id in seqIds) { fs.Write(BitConverter.GetBytes(id)); }
        Console.WriteLine($"wrote SEQ\\{Path.ChangeExtension(modKey.FileName, ".seq")}: {seqIds.Count} start-game-enabled quest(s)");
    }
}
Directory.CreateDirectory(Path.Combine(repo, "generated"));
File.WriteAllText(Path.Combine(repo, "generated", "plugin-forms.json"), forms.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
Console.WriteLine($"wrote {esp}: {mod.Globals.Count} globals, {mod.Keywords.Count} keywords, {mod.Perks.Count} perks ({entryPoints} entry points), {mod.Spells.Count} ability spells ({constantEffects} constant effects)");
Console.WriteLine($"{stubs.Count} stubs, {approx.Count} approximations, {dllDone.Count} effects done by the SKSE plugin - generated/plugin-forms.json lists them");

record BGSPerkBuild_Result(Perk Perk);
