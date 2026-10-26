// Njordlinger Combat Gameplay Overhaul - the plugin build (PLAN.md phase 2 step 2): writes
// NjordlingerCombatGameplayOverhaul.esp (ESL-flagged, form version 44, no masters) from config/classes.json and
// config/class-abilities.json, and generated/plugin-forms.json with the local form ID of every record, which
// tools/build-classes.py puts into the shipped classes.json for the DLL.
//
//   dotnet run --project tools/PluginBuild -- <repo folder>
//
// Records: three group globals (NJCGO_GroupWarrior / Magic / Covert, short, 1 = open), one global per class
// (NJCGO_Class_<id>, short, 1 = this character has been that class), one keyword per class (NJCGO_Class_<id>), and for
// each class whose ability has effects a constant self ability (NJCGO_Ab_<id>) with one Value Modifier magic effect
// per effect (NJCGO_AbFx_<id>_<n>), each effect carrying the class keyword. Nothing in the game is overridden.
using System.Text.Json;
using System.Text.Json.Nodes;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;

var repo = args.Length > 0 ? args[0] : Directory.GetCurrentDirectory();
var classes = JsonNode.Parse(File.ReadAllText(Path.Combine(repo, "config", "classes.json")))!;
var abilities = JsonNode.Parse(File.ReadAllText(Path.Combine(repo, "config", "class-abilities.json")))!["abilities"]!.AsObject();

var modKey = ModKey.FromFileName("NjordlingerCombatGameplayOverhaul.esp");
var mod = new SkyrimMod(modKey, SkyrimRelease.SkyrimSE);
mod.ModHeader.Flags |= SkyrimModHeader.HeaderFlag.Small;
mod.ModHeader.Author = "ApocryphaRealm";
mod.ModHeader.Description = "Njordlinger Combat Gameplay Overhaul - class globals, keywords and abilities.";

string Hex(IMajorRecordGetter r) => $"0x{r.FormKey.ID:X3}";
var forms = new JsonObject();
var groupForms = new JsonObject();
foreach (var g in new[] { "warrior", "magic", "covert" })
{
    var glob = new GlobalShort(mod) { EditorID = "NJCGO_Group" + char.ToUpper(g[0]) + g[1..], Data = 1 };
    mod.Globals.Add(glob);
    groupForms[g] = Hex(glob);
}
forms["groupGlobals"] = groupForms;

var classForms = new JsonObject();
var stubs = new List<string>();
foreach (var c in classes["classes"]!.AsArray())
{
    var id = c!["id"]!.GetValue<string>();
    var name = c["name"]!.GetValue<string>();
    var entry = new JsonObject();
    var glob = new GlobalShort(mod) { EditorID = "NJCGO_Class_" + id, Data = 0 };
    mod.Globals.Add(glob);
    entry["global"] = Hex(glob);
    var kw = mod.Keywords.AddNew("NJCGO_Class_" + id);
    entry["keyword"] = Hex(kw);

    if (abilities[id] is JsonObject ab && ab["effects"] is JsonArray fx && fx.Count > 0)
    {
        var spell = mod.Spells.AddNew("NJCGO_Ab_" + id);
        spell.Name = ab["name"]!.GetValue<string>();
        spell.Type = SpellType.Ability;
        spell.CastType = CastType.ConstantEffect;
        spell.TargetType = TargetType.Self;
        spell.Description = $"The {name} class ability.";
        for (var n = 0; n < fx.Count; ++n)
        {
            var av = Enum.Parse<ActorValue>(fx[n]!["av"]!.GetValue<string>());
            var magnitude = fx[n]!["magnitude"]!.GetValue<float>();
            var mgef = mod.MagicEffects.AddNew($"NJCGO_AbFx_{id}_{n}");
            mgef.Name = spell.Name.String;
            mgef.Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.ValueModifier) { ActorValue = av };
            mgef.CastType = CastType.ConstantEffect;
            mgef.TargetType = TargetType.Self;
            mgef.Flags = MagicEffect.Flag.Recover | MagicEffect.Flag.NoArea | MagicEffect.Flag.NoDuration;
            mgef.MagicSkill = ActorValue.None;
            mgef.ResistValue = ActorValue.None;
            mgef.SecondActorValue = ActorValue.None;
            mgef.Keywords = new Noggog.ExtendedList<IFormLinkGetter<IKeywordGetter>> { kw.ToLink() };
            spell.Effects.Add(new Effect { BaseEffect = mgef.ToNullableLink(), Data = new EffectData { Magnitude = magnitude, Area = 0, Duration = 0 } });
        }
        entry["ability"] = Hex(spell);
    }
    else
    {
        stubs.Add($"{id}: {(abilities[id] is JsonObject s && s["stub"] is JsonNode st ? st.GetValue<string>() : "no ability defined")}");
    }
    classForms[id] = entry;
}
forms["classes"] = classForms;
forms["stubs"] = new JsonArray(stubs.Select(s => (JsonNode)JsonValue.Create(s)!).ToArray());

var esp = Path.Combine(repo, "data", "NjordlingerCombatGameplayOverhaul.esp");
mod.WriteToBinary(esp);
Directory.CreateDirectory(Path.Combine(repo, "generated"));
File.WriteAllText(Path.Combine(repo, "generated", "plugin-forms.json"), forms.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
Console.WriteLine($"wrote {esp}: {mod.Globals.Count} globals, {mod.Keywords.Count} keywords, {mod.Spells.Count} abilities, {mod.MagicEffects.Count} magic effects");
foreach (var s in stubs) { Console.WriteLine("  stub - " + s); }
