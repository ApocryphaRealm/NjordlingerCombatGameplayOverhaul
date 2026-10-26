// Njordlinger Combat Gameplay Overhaul - the phase 1 build step (data layer, no game code).
//
// Reads a staged load order (the Synthesis run-patcher command line: --DataFolderPath, --LoadOrderFilePath) and writes
// the mod's generated data files into the folder named by NJCGO_OUT, from the build inputs in NJCGO_CONFIG:
//
//   NjordlingerCGO_Tiers_KID.ini   - one tier keyword on every playable weapon and armour (PLAN.md D13):
//        common / magic / epic / legendary by the SMITHING LEVEL of the perk its crafting recipe requires
//        (config tiers.json "bands"), unique for the named uniques, a material fallback for items with no recipe;
//        an enchanted variant takes its template's tier (D10). Items added by the mods awaiting the owner's
//        keep-or-remove mark (EQUIPMENT-AUDIT.md section 0, Q47) are left out (D24, phase 5).
//   NjordlingerCGO_Sets_KID.ini    - set-membership keywords for the first five unique sets (sets.json).
//   SkyPatcher\npc\...\LoreTierLevels.ini - NPC levels by lore tier (npc-tiers.json, D11):
//        a PC-level-mult NPC is clamped to its tier's band (calcLevelMin / calcLevelMax); a fixed-level NPC above the
//        band's top is set to the top (level). Unique NPCs are never touched.
//   build-report.txt               - every decision, with counts, so a reader can audit the output.
//
// The patch the pipeline writes (--OutputPath) is empty and is thrown away: this tool only reads records.
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;
using Mutagen.Bethesda.Synthesis;

namespace TierBuild
{
    public class TierConfig
    {
        public string[] Tiers { get; set; } = { "Common", "Magic", "Epic", "Legendary", "Unique" };
        public string KeywordPrefix { get; set; } = "NJCGO_Tier_";
        public Dictionary<string, int> PerkLevelAliases { get; set; } = new();   // perk EditorID -> smithing level
        public string[] IgnorePerks { get; set; } = Array.Empty<string>();
        public List<Band> Bands { get; set; } = new();                            // smithing level (inclusive max) -> tier
        public string[] CraftBenches { get; set; } = Array.Empty<string>();
        public string[] TemperBenches { get; set; } = Array.Empty<string>();
        public string[] UniqueKeywords { get; set; } = Array.Empty<string>();
        public string UniqueIncludeRegex { get; set; } = "";
        public string UniqueExcludeRegex { get; set; } = "";
        public string[] UniqueSourcePlugins { get; set; } = Array.Empty<string>();   // a plugin whose every weapon/armour is an artefact
        public string[] ExcludedPlugins { get; set; } = Array.Empty<string>();
        public int ItemsPerLine { get; set; } = 60;
        public bool SetPiecesAreUnique { get; set; } = false;   // sets.json pieces -> unique tier (Q58)
    }
    public class Band { public int MaxLevel { get; set; } public string Tier { get; set; } = ""; }
    public class SetDef { public string Name { get; set; } = ""; public string Keyword { get; set; } = ""; public string[] EditorIdPrefixes { get; set; } = Array.Empty<string>(); public string? NameRegex { get; set; } public string Status { get; set; } = ""; }
    public class NpcTier { public string Name { get; set; } = ""; public string[] Factions { get; set; } = Array.Empty<string>(); public string[] Races { get; set; } = Array.Empty<string>(); public int Min { get; set; } public int Max { get; set; } public string Note { get; set; } = ""; }
    public class NpcConfig { public string OutputSubfolder { get; set; } = "NjordlingerCGO"; public List<NpcTier> Tiers { get; set; } = new(); }

    public static class Program
    {
        static string Cfg = "", Out = "";
        static readonly StringBuilder Report = new();
        static void R(string s) { Report.AppendLine(s); Console.WriteLine(s); }

        public static async Task<int> Main(string[] args)
        {
            Cfg = Environment.GetEnvironmentVariable("NJCGO_CONFIG") ?? "";
            Out = Environment.GetEnvironmentVariable("NJCGO_OUT") ?? "";
            if (Cfg == "" || Out == "" || !Directory.Exists(Cfg)) { Console.Error.WriteLine("set NJCGO_CONFIG (existing folder) and NJCGO_OUT"); return 2; }
            Directory.CreateDirectory(Out);
            return await SynthesisPipeline.Instance
                .AddPatch<ISkyrimMod, ISkyrimModGetter>(Run)
                .SetTypicalOpen(GameRelease.SkyrimSE, "NjcgoTierBuild.esp")
                .Run(args);
        }

        static T Load<T>(string file) where T : new()
        {
            var p = Path.Combine(Cfg, file);
            if (!File.Exists(p)) throw new FileNotFoundException("build input missing", p);
            return JsonSerializer.Deserialize<T>(File.ReadAllText(p), new JsonSerializerOptions { PropertyNameCaseInsensitive = true, ReadCommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true }) ?? new T();
        }

        static string KidId(FormKey k) => $"0x{k.ID:X}~{k.ModKey.FileName}";
        static string SpId(FormKey k) => $"{k.ModKey.FileName}|{k.ID:X}";

        // NJCGO_VERIFY=<mod data folder>: every plugin|FormID in the shipped KID and SkyPatcher files must resolve to a
        // record of the right type in this load order (past-mistakes 12464: a distributor INI built for another
        // variant of its master silently drops what it cannot find). Writes verify-report.txt; exit code via file.
        static int Verify(IPatcherState<ISkyrimMod, ISkyrimModGetter> state, string dataDir)
        {
            var lc = state.LinkCache; int ok = 0, bad = 0; var rep = new StringBuilder();
            var plugins = new HashSet<string>(state.LoadOrder.ListedOrder.Select(l => l.ModKey.FileName.String), StringComparer.OrdinalIgnoreCase);
            void Check(string file, string plugin, string hex, Type want)
            {
                var mk = state.LoadOrder.ListedOrder.Select(l => l.ModKey).FirstOrDefault(k => string.Equals(k.FileName.String, plugin, StringComparison.OrdinalIgnoreCase));
                if (mk.IsNull || !plugins.Contains(plugin)) { bad++; rep.AppendLine($"MISSING PLUGIN {file}: {plugin}|{hex}"); return; }
                var fk = new FormKey(mk, Convert.ToUInt32(hex, 16));
                if (lc.TryResolve(fk, want, out _)) ok++; else { bad++; rep.AppendLine($"UNRESOLVED {file}: {plugin}|{hex} as {want.Name}"); }
            }
            foreach (var f in Directory.GetFiles(dataDir, "*_KID.ini"))
                foreach (var line in File.ReadLines(f))
                {
                    var m = Regex.Match(line, @"^Keyword\s*=\s*[^|]+\|(Weapon|Armor)\|(.+)$"); if (!m.Success) continue;
                    var want = m.Groups[1].Value == "Weapon" ? typeof(IWeaponGetter) : typeof(IArmorGetter);
                    foreach (var tok in m.Groups[2].Value.Split('|')[0].Split(','))
                    { var t = Regex.Match(tok.Trim(), @"^0x([0-9A-Fa-f]+)~(.+)$"); if (t.Success) Check(Path.GetFileName(f), t.Groups[2].Value, t.Groups[1].Value, want); }
                }
            var sp = Path.Combine(dataDir, "SKSE", "Plugins", "SkyPatcher");
            if (Directory.Exists(sp))
                foreach (var f in Directory.GetFiles(sp, "*.ini", SearchOption.AllDirectories))
                    foreach (var line in File.ReadLines(f))
                    { var m = Regex.Match(line, @"^filterByNpcs=([^|:]+)\|([0-9A-Fa-f]+):"); if (m.Success) Check(Path.GetFileName(f), m.Groups[1].Value, m.Groups[2].Value, typeof(INpcGetter)); }
            rep.Insert(0, $"verify {dataDir}: {ok} references resolve, {bad} do not\n");
            File.WriteAllText(Path.Combine(Out, "verify-report.txt"), rep.ToString(), new UTF8Encoding(false));
            Console.WriteLine(rep.ToString().Split('\n')[0]);
            return bad;
        }

        static void Run(IPatcherState<ISkyrimMod, ISkyrimModGetter> state)
        {
            if (Environment.GetEnvironmentVariable("NJCGO_VERIFY") is string vd && vd != "")
            {
                int bad = Verify(state, vd);
                File.WriteAllText(Path.Combine(Out, "verify-exit.txt"), bad == 0 ? "0" : "1");
                return;
            }
            R($"# Njordlinger Combat Gameplay Overhaul - phase 1 build report ({DateTime.Now:yyyy-MM-dd HH:mm})");
            R($"load order: {state.LoadOrder.ListedOrder.Count()} plugins");
            Tiers(state);
            Sets(state);
            NpcLevels(state);
            File.WriteAllText(Path.Combine(Out, "build-report.txt"), Report.ToString(), new UTF8Encoding(false));
        }

        // ---------------------------------------------------------------- tiers
        static readonly Dictionary<FormKey, string> TierOf = new();

        static void Tiers(IPatcherState<ISkyrimMod, ISkyrimModGetter> state)
        {
            var cfg = Load<TierConfig>("tiers.json");
            var lc = state.LinkCache;
            var excluded = new HashSet<ModKey>(cfg.ExcludedPlugins.Select(p => ModKey.FromFileName(p)));
            var ignorePerk = new HashSet<string>(cfg.IgnorePerks, StringComparer.OrdinalIgnoreCase);
            var craft = new HashSet<string>(cfg.CraftBenches, StringComparer.OrdinalIgnoreCase);
            var temper = new HashSet<string>(cfg.TemperBenches, StringComparer.OrdinalIgnoreCase);
            var bands = cfg.Bands.OrderBy(b => b.MaxLevel).ToList();

            // 1. the smithing level of each perk: its own GetBaseActorValue(Smithing) condition, else the alias table
            var perkLevel = new Dictionary<FormKey, int>();
            var perkName = new Dictionary<FormKey, string>();
            foreach (var p in state.LoadOrder.PriorityOrder.Perk().WinningOverrides())
            {
                var e = p.EditorID ?? "";
                if (ignorePerk.Contains(e)) continue;
                int lvl = -1;
                foreach (var c in p.Conditions)
                    if (c.Data is IGetBaseActorValueConditionDataGetter g && g.ActorValue == ActorValue.Smithing && c is IConditionFloatGetter cf)
                        lvl = Math.Max(lvl, (int)cf.ComparisonValue);
                if (cfg.PerkLevelAliases.TryGetValue(e, out var al)) lvl = al;
                if (lvl >= 0) { perkLevel[p.FormKey] = lvl; perkName[p.FormKey] = e; }
            }
            R($"\n## Tiers\nsmithing perks with a level: {perkLevel.Count} ({string.Join(", ", perkName.OrderBy(x => perkLevel[x.Key]).Select(x => x.Value + "=" + perkLevel[x.Key]))})");

            string TierForLevel(int lvl) { if (lvl <= 0) return "Common"; foreach (var b in bands) if (lvl <= b.MaxLevel) return b.Tier; return bands.Last().Tier; }

            // 2. recipes: created item -> lowest required level over its craft recipes (and, separately, temper recipes)
            var craftLvl = new Dictionary<FormKey, int>();
            var temperLvl = new Dictionary<FormKey, int>();
            var conversionLvl = new Dictionary<FormKey, int>();
            var createdByAnyRecipe = new HashSet<FormKey>();
            foreach (var c in state.LoadOrder.PriorityOrder.ConstructibleObject().WinningOverrides())
            {
                if (c.CreatedObject.IsNull) continue;
                createdByAnyRecipe.Add(c.CreatedObject.FormKey);
                if (c.WorkbenchKeyword.IsNull || !lc.TryResolve<IKeywordGetter>(c.WorkbenchKeyword.FormKey, out var kw)) continue;
                var bench = kw.EditorID ?? "";
                bool isCraft = craft.Contains(bench), isTemper = temper.Contains(bench);
                if (!isCraft && !isTemper) continue;
                // the requirement is read from EVERY version of the recipe record and the highest is kept: a later
                // plugin that copies the recipe and drops its perk condition (seen on the vanilla ebony recipes) does
                // not make the material common - the perk overhaul's own version decides (PLAN.md D13 "in the installed Adamant")
                int need = 0, perkConds = 0;
                foreach (var ver in lc.ResolveAllContexts<IConstructibleObject, IConstructibleObjectGetter>(c.FormKey))
                {
                    var levels = new List<int>(); bool anyOr = false;
                    foreach (var cond in ver.Record.Conditions)
                    {
                        if (cond.Data is not IHasPerkConditionDataGetter hp) continue;
                        if (cond.Flags.HasFlag(Condition.Flag.OR)) anyOr = true;
                        if (perkLevel.TryGetValue(hp.Perk.Link.FormKey, out var l)) levels.Add(l);
                    }
                    perkConds += levels.Count;
                    need = Math.Max(need, levels.Count == 0 ? 0 : (anyOr ? levels.Min() : levels.Max()));
                }
                var levelsCount = perkConds;
                // a recipe that takes a weapon or an armour piece is a conversion or upgrade (Thane Weapons' axes from
                // other axes, replicas): it says nothing about the material, so it counts only when nothing else does
                bool conversion = isCraft && (c.Items ?? new List<IContainerEntryGetter>()).Any(e =>
                    lc.TryResolve<IWeaponGetter>(e.Item.Item.FormKey, out _) || lc.TryResolve<IArmorGetter>(e.Item.Item.FormKey, out _));
                if (Environment.GetEnvironmentVariable("NJCGO_TRACE") is string tr && lc.TryResolve(c.CreatedObject.FormKey, out var co) && co.EditorID == tr)
                    Console.WriteLine($"TRACE {c.FormKey} {c.EditorID} bench={bench} need={need} conversion={conversion} perks={levelsCount}");
                if (conversion) { var ck = c.CreatedObject.FormKey; conversionLvl[ck] = conversionLvl.TryGetValue(ck, out var cp) ? Math.Min(cp, need) : need; continue; }
                var dict = isCraft ? craftLvl : temperLvl;
                var target = c.CreatedObject.FormKey;
                dict[target] = dict.TryGetValue(target, out var prev) ? Math.Min(prev, need) : need;
            }
            foreach (var kv in conversionLvl) if (kv.Value > 0 && !craftLvl.ContainsKey(kv.Key)) craftLvl[kv.Key] = kv.Value;   // a perkless conversion leaves the item to the material table
            R($"craft recipes give a level to {craftLvl.Count} items (conversion recipes only where nothing else does: {conversionLvl.Count}), temper recipes to {temperLvl.Count}");

            // 3. LVLI entries (for the unique rule): every item some levelled list can hand out
            var inLists = new HashSet<FormKey>();
            foreach (var l in state.LoadOrder.PriorityOrder.LeveledItem().WinningOverrides())
                foreach (var e in l.Entries ?? Enumerable.Empty<ILeveledItemEntryGetter>())
                    if (e.Data != null) inLists.Add(e.Data.Reference.FormKey);

            var uniqueKw = new HashSet<FormKey>();
            foreach (var k in state.LoadOrder.PriorityOrder.Keyword().WinningOverrides())
                if (k.EditorID != null && cfg.UniqueKeywords.Contains(k.EditorID, StringComparer.OrdinalIgnoreCase)) uniqueKw.Add(k.FormKey);
            var uIncRx = cfg.UniqueIncludeRegex == "" ? null : new Regex(cfg.UniqueIncludeRegex, RegexOptions.IgnoreCase);
            var uExcRx = cfg.UniqueExcludeRegex == "" ? null : new Regex(cfg.UniqueExcludeRegex, RegexOptions.IgnoreCase);
            bool Listed(string e) => uIncRx != null && uIncRx.IsMatch(e);
            var uSrc = new HashSet<ModKey>(cfg.UniqueSourcePlugins.Select(p => ModKey.FromFileName(p)));
            var fromSource = new HashSet<FormKey>();
            foreach (var src in uSrc)
            {
                var m = state.LoadOrder.TryGetValue(src)?.Mod;
                if (m == null) { R($"  unique source plugin {src} is not in the load order"); continue; }
                foreach (var w in m.Weapons) fromSource.Add(w.FormKey);
                foreach (var a in m.Armors) fromSource.Add(a.FormKey);
            }

            // 4. collect the playable items, their template, their keywords
            var items = new Dictionary<FormKey, Item>();
            foreach (var w in state.LoadOrder.PriorityOrder.Weapon().WinningOverrides())
            {
                if (w.Data != null && w.Data.Flags.HasFlag(WeaponData.Flag.NonPlayable)) continue;
                if (w.MajorFlags.HasFlag(Weapon.MajorFlag.NonPlayable)) continue;
                items[w.FormKey] = new Item(w.FormKey, w.EditorID ?? "", w.Name?.String ?? "", "Weapon", w.Template.IsNull ? null : w.Template.FormKey,
                    !w.ObjectEffect.IsNull && w.Data?.AnimationType != WeaponAnimationType.Staff, (w.Keywords ?? new List<IFormLinkGetter<IKeywordGetter>>()).Select(k => k.FormKey).ToList());
            }
            foreach (var a in state.LoadOrder.PriorityOrder.Armor().WinningOverrides())
            {
                if (a.MajorFlags.HasFlag(Armor.MajorFlag.NonPlayable)) continue;
                if (a.BodyTemplate != null && a.BodyTemplate.Flags.HasFlag(BodyTemplate.Flag.NonPlayable)) continue;
                if (a.BodyTemplate == null || a.Armature.Count == 0) continue;               // helpers with nothing to wear
                // (names are not read: Skyrim.esm keeps its names in string files, which the staged folder leaves out)
                items[a.FormKey] = new Item(a.FormKey, a.EditorID ?? "", a.Name?.String ?? "", "Armor", a.TemplateArmor.IsNull ? null : a.TemplateArmor.FormKey,
                    !a.ObjectEffect.IsNull, (a.Keywords ?? new List<IFormLinkGetter<IKeywordGetter>>()).Select(k => k.FormKey).ToList());
            }
            R($"playable weapons and wearable armour: {items.Count}");

            var usedAsTemplate = items.Values.Where(i => i.Template != null).Select(i => i.Template!.Value).ToHashSet();
            Item Root(Item it) { var cur = it; for (int i = 0; i < 8 && cur.Template is FormKey t && items.TryGetValue(t, out var nx); i++) cur = nx; return cur; }

            // 5. tier per base item: unique rules, then recipe level, then temper level; material fallback after
            var reason = new Dictionary<FormKey, string>();
            var uniques = new List<Item>();
            // Q58 (Fable review / Main Agent, 2026-10-26): a named set's pieces are named uniques - unique tier
            var setPieces = cfg.SetPiecesAreUnique
                ? Load<List<SetDef>>("sets.json").SelectMany(sd => SetPieces(state, sd)).Select(a => a.FormKey).ToHashSet()
                : new HashSet<FormKey>();
            R($"set pieces made unique: {setPieces.Count}");
            foreach (var it in items.Values)
            {
                if (excluded.Contains(it.Key.ModKey)) { reason[it.Key] = "excluded (Q47)"; continue; }
                if (it.Template != null) continue;   // variants later
                bool unique = Listed(it.Edid) || fromSource.Contains(it.Key) || it.Keywords.Any(uniqueKw.Contains)
                              || (it.Enchanted && !inLists.Contains(it.Key) && !createdByAnyRecipe.Contains(it.Key) && !usedAsTemplate.Contains(it.Key));
                if (uExcRx != null && uExcRx.IsMatch(it.Edid) && !Listed(it.Edid)) unique = false;
                if (setPieces.Contains(it.Key)) { TierOf[it.Key] = "Unique"; reason[it.Key] = "unique: set piece"; uniques.Add(it); continue; }
                if (unique) { TierOf[it.Key] = "Unique"; reason[it.Key] = Listed(it.Edid) ? "unique: listed" : fromSource.Contains(it.Key) ? "unique: artefact plugin" : it.Keywords.Any(uniqueKw.Contains) ? "unique: keyword" : "unique: enchanted, not in any list, no recipe"; uniques.Add(it); continue; }
                if (craftLvl.TryGetValue(it.Key, out var cl)) { TierOf[it.Key] = TierForLevel(cl); reason[it.Key] = $"recipe level {cl}"; continue; }
                if (temperLvl.TryGetValue(it.Key, out var tl) && tl > 0) {   // a temper recipe with no material perk says nothing
                 TierOf[it.Key] = TierForLevel(tl); reason[it.Key] = $"temper level {tl}"; continue; }
            }
            // material fallback: the tier most recipe-tiered items of the same material keyword got
            var matVotes = new Dictionary<FormKey, Dictionary<string, int>>();
            var matName = new Dictionary<FormKey, string>();
            foreach (var it in items.Values)
            {
                if (!TierOf.TryGetValue(it.Key, out var t) || t == "Unique" || !reason[it.Key].StartsWith("recipe")) continue;   // craft recipes vote
                foreach (var k in it.Keywords)
                {
                    if (!lc.TryResolve<IKeywordGetter>(k, out var kw) || kw.EditorID == null) continue;
                    if (!Regex.IsMatch(kw.EditorID, "^(Armor|Weap)Material", RegexOptions.IgnoreCase)) continue;
                    matName[k] = kw.EditorID;
                    if (!matVotes.TryGetValue(k, out var v)) matVotes[k] = v = new();
                    v[t] = v.GetValueOrDefault(t) + 1;
                }
            }
            var matTier = matVotes.ToDictionary(x => x.Key, x => x.Value.OrderByDescending(v => v.Value).First().Key);
            R("material fallback table: " + string.Join(", ", matTier.OrderBy(x => matName[x.Key]).Select(x => $"{matName[x.Key]}={x.Value}")));
            int fbMat = 0, fbNone = 0;
            foreach (var it in items.Values)
            {
                if (excluded.Contains(it.Key.ModKey) || it.Template != null || TierOf.ContainsKey(it.Key)) continue;
                var m = it.Keywords.Where(matTier.ContainsKey).Select(k => matTier[k]).ToList();
                if (m.Count > 0) { var t = m.OrderBy(x => Array.IndexOf(cfg.Tiers, x)).Last(); TierOf[it.Key] = t; reason[it.Key] = "material"; fbMat++; }
                else { TierOf[it.Key] = "Common"; reason[it.Key] = "no recipe, no material"; fbNone++; }
            }
            // variants take their root's tier (D10: a found enchantment does not change the tier)
            int variants = 0;
            foreach (var it in items.Values)
            {
                if (it.Template == null || excluded.Contains(it.Key.ModKey)) continue;
                var root = Root(it);
                if (excluded.Contains(root.Key.ModKey)) { reason[it.Key] = "excluded (template Q47)"; continue; }
                if (!TierOf.TryGetValue(root.Key, out var t))
                    t = "Common";
                if (fromSource.Contains(it.Key) && !(uExcRx != null && uExcRx.IsMatch(it.Edid))) t = "Unique";
                if (setPieces.Contains(it.Key)) t = "Unique";
                if (t == "Unique") t = "Unique";
                TierOf[it.Key] = t; reason[it.Key] = $"variant of {root.Edid}"; variants++;
            }
            R($"material fallback {fbMat}, no recipe and no material {fbNone}, template variants {variants}, excluded {reason.Count(r => r.Value.StartsWith("excluded"))}");
            foreach (var t in cfg.Tiers) R($"  {t,-10} {TierOf.Count(x => x.Value == t),6}");

            // 6. write the KID file
            var sb = new StringBuilder();
            sb.AppendLine("; Njordlinger Combat Gameplay Overhaul - item tiers (GENERATED by tools\\TierBuild - do not edit by hand)");
            sb.AppendLine($"; built {DateTime.Now:yyyy-MM-dd HH:mm} against the staged load order; re-run tools\\build-data.py after the list changes.");
            sb.AppendLine("; One tier keyword per playable weapon and armour (PLAN.md D13). The keywords are created by KID when no plugin defines them.");
            foreach (var t in cfg.Tiers)
                foreach (var type in new[] { "Weapon", "Armor" })
                {
                    var keys = TierOf.Where(x => x.Value == t && items[x.Key].Type == type).Select(x => x.Key).OrderBy(k => k.ModKey.FileName.String).ThenBy(k => k.ID).ToList();
                    if (keys.Count == 0) continue;
                    sb.AppendLine($"\n; {t} - {type} ({keys.Count})");
                    for (int i = 0; i < keys.Count; i += cfg.ItemsPerLine)
                        sb.AppendLine($"Keyword = {cfg.KeywordPrefix}{t}|{type}|{string.Join(",", keys.Skip(i).Take(cfg.ItemsPerLine).Select(KidId))}");
                }
            File.WriteAllText(Path.Combine(Out, "NjordlingerCGO_Tiers_KID.ini"), sb.ToString(), new UTF8Encoding(false));

            // 7. the audit list (every item, its tier, why) and the uniques for phase 3
            var al2 = new StringBuilder("formkey\ttype\ttier\teditorid\tname\treason\n");
            foreach (var it in items.Values.OrderBy(i => i.Key.ModKey.FileName.String).ThenBy(i => i.Key.ID))
                al2.Append($"{SpId(it.Key)}\t{it.Type}\t{TierOf.GetValueOrDefault(it.Key, "-")}\t{it.Edid}\t{it.Name}\t{reason.GetValueOrDefault(it.Key, "")}\n");
            File.WriteAllText(Path.Combine(Out, "tier-audit.tsv"), al2.ToString(), new UTF8Encoding(false));
            R($"uniques: {uniques.Count} (listed in tier-audit.tsv with reason 'unique: ...')");
        }

        record Item(FormKey Key, string Edid, string Name, string Type, FormKey? Template, bool Enchanted, List<FormKey> Keywords);

        // ---------------------------------------------------------------- sets
        // The pieces of one set: playable armour whose EditorID starts with one of the set's prefixes or whose name matches
        // its regex (a replacer such as Konungr Gaaf for Deathbrand is matched by name). Used by the set keywords AND by the
        // tier rule (2026-10-26, Q58: set pieces are unique-tier).
        static List<IArmorGetter> SetPieces(IPatcherState<ISkyrimMod, ISkyrimModGetter> state, SetDef s)
        {
            var re = s.NameRegex == null ? null : new Regex(s.NameRegex);
            return state.LoadOrder.PriorityOrder.Armor().WinningOverrides()
                .Where(a => !a.MajorFlags.HasFlag(Armor.MajorFlag.NonPlayable))
                .Where(a => (a.EditorID != null && s.EditorIdPrefixes.Any(p => a.EditorID.StartsWith(p, StringComparison.OrdinalIgnoreCase)))
                         || (re != null && a.Name?.String != null && re.IsMatch(a.Name.String)))
                .OrderBy(a => a.FormKey.ModKey.FileName.String).ThenBy(a => a.FormKey.ID).ToList();
        }

        static void Sets(IPatcherState<ISkyrimMod, ISkyrimModGetter> state)
        {
            var sets = Load<List<SetDef>>("sets.json");
            var sb = new StringBuilder();
            sb.AppendLine("; Njordlinger Combat Gameplay Overhaul - unique set membership (GENERATED by tools\\TierBuild from config\\sets.json)");
            sb.AppendLine("; Keywords only: the 2- and 4-piece bonuses come in phase 3 (PLAN.md D14).");
            R("\n## Sets");
            foreach (var s in sets)
            {
                var hits = SetPieces(state, s);
                R($"{s.Name} ({s.Keyword}, {s.Status}): {hits.Count} pieces - {string.Join("; ", hits.Select(h => h.Name?.String + " [" + h.EditorID + "]"))}");
                if (hits.Count == 0) { sb.AppendLine($"; {s.Name}: no pieces found in this load order"); continue; }
                sb.AppendLine($"\n; {s.Name} - {s.Status}");
                sb.AppendLine($"Keyword = {s.Keyword}|Armor|{string.Join(",", hits.Select(h => KidId(h.FormKey)))}");
            }
            File.WriteAllText(Path.Combine(Out, "NjordlingerCGO_Sets_KID.ini"), sb.ToString(), new UTF8Encoding(false));
        }

        // ---------------------------------------------------------------- NPC levels
        static void NpcLevels(IPatcherState<ISkyrimMod, ISkyrimModGetter> state)
        {
            var cfg = Load<NpcConfig>("npc-tiers.json");
            var lc = state.LinkCache;
            var facKey = new Dictionary<string, FormKey>(StringComparer.OrdinalIgnoreCase);
            foreach (var f in state.LoadOrder.PriorityOrder.Faction().WinningOverrides()) if (f.EditorID != null) facKey.TryAdd(f.EditorID, f.FormKey);
            var raceKey = new Dictionary<string, FormKey>(StringComparer.OrdinalIgnoreCase);
            foreach (var r in state.LoadOrder.PriorityOrder.Race().WinningOverrides()) if (r.EditorID != null) raceKey.TryAdd(r.EditorID, r.FormKey);
            R("\n## NPC levels by lore tier");
            var tierFac = new List<(NpcTier t, HashSet<FormKey> fac, HashSet<FormKey> race)>();
            foreach (var t in cfg.Tiers)
            {
                var fs = new HashSet<FormKey>(); var rs = new HashSet<FormKey>();
                foreach (var f in t.Factions) { if (facKey.TryGetValue(f, out var k)) fs.Add(k); else R($"  WARN {t.Name}: faction {f} not in the load order"); }
                foreach (var r in t.Races) { if (raceKey.TryGetValue(r, out var k)) rs.Add(k); else R($"  WARN {t.Name}: race {r} not in the load order"); }
                tierFac.Add((t, fs, rs));
            }

            INpcGetter? Npc(FormKey k) => lc.TryResolve<INpcGetter>(k, out var n) ? n : null;
            // effective factions / race follow the template chain while the matching template flag is set (an LVLN
            // template ends the walk: the record's own data is what the picked entry inherits from)
            IEnumerable<FormKey> Factions(INpcGetter n) { var cur = n; for (int i = 0; i < 8; i++) { if (cur.Template.IsNull || !cur.Configuration.TemplateFlags.HasFlag(NpcConfiguration.TemplateFlag.Factions)) return cur.Factions.Select(f => f.Faction.FormKey); var nx = Npc(cur.Template.FormKey); if (nx == null) return cur.Factions.Select(f => f.Faction.FormKey); cur = nx; } return cur.Factions.Select(f => f.Faction.FormKey); }
            FormKey Race(INpcGetter n) { var cur = n; for (int i = 0; i < 8; i++) { if (cur.Template.IsNull || !cur.Configuration.TemplateFlags.HasFlag(NpcConfiguration.TemplateFlag.Traits)) return cur.Race.FormKey; var nx = Npc(cur.Template.FormKey); if (nx == null) return cur.Race.FormKey; cur = nx; } return cur.Race.FormKey; }

            var lines = new Dictionary<string, List<string>>();
            var counts = new Dictionary<string, (int clamp, int cap, int skip)>();
            foreach (var n in state.LoadOrder.PriorityOrder.Npc().WinningOverrides())
            {
                if (!n.Template.IsNull && n.Configuration.TemplateFlags.HasFlag(NpcConfiguration.TemplateFlag.Stats)) continue;   // its level is the template's
                if (n.Configuration.Flags.HasFlag(NpcConfiguration.Flag.Unique)) continue;                                        // named NPCs keep their levels
                var fs = Factions(n).ToHashSet(); var race = Race(n);
                var hit = tierFac.FirstOrDefault(t => t.fac.Overlaps(fs) || t.race.Contains(race));
                if (hit.t == null) continue;
                var t = hit.t;
                if (!lines.TryGetValue(t.Name, out var ls)) lines[t.Name] = ls = new();
                var c = counts.GetValueOrDefault(t.Name);
                if (n.Configuration.Level is IPcLevelMultGetter)
                {
                    if (n.Configuration.CalcMinLevel == t.Min && n.Configuration.CalcMaxLevel == t.Max) { c.skip++; }
                    else { ls.Add($";{n.EditorID} (PC level mult, was {n.Configuration.CalcMinLevel}-{n.Configuration.CalcMaxLevel})"); ls.Add($"filterByNpcs={SpId(n.FormKey)}:calcLevelMin={t.Min}:calcLevelMax={t.Max}"); c.clamp++; }
                }
                else if (n.Configuration.Level is INpcLevelGetter sl)
                {
                    if (sl.Level > t.Max) { ls.Add($";{n.EditorID} (fixed level {sl.Level})"); ls.Add($"filterByNpcs={SpId(n.FormKey)}:level={t.Max}"); c.cap++; }
                    else c.skip++;
                }
                counts[t.Name] = c;
            }
            var sb = new StringBuilder();
            sb.AppendLine("; Njordlinger Combat Gameplay Overhaul - NPC levels by lore tier (GENERATED by tools\\TierBuild from config\\npc-tiers.json)");
            sb.AppendLine("; PLAN.md D11: enemies by lore tier with Arena (zones) and Open World Loot (loot by place) in place.");
            sb.AppendLine("; A PC-level-mult NPC is clamped to its tier's band; a fixed-level NPC above the band's top is set to the top;");
            sb.AppendLine("; unique (named) NPCs and NPCs whose stats come from a template are never written. One NPC per line, plugin|FormID;");
            sb.AppendLine("; its EditorID and old level sit on the comment line above it (no trailing comments on SkyPatcher lines).");
            foreach (var t in cfg.Tiers)
            {
                var c = counts.GetValueOrDefault(t.Name);
                R($"{t.Name,-22} band {t.Min,3}-{t.Max,-3} clamped {c.clamp,4}  capped {c.cap,4}  left as they are {c.skip,5}   {t.Note}");
                if (!lines.TryGetValue(t.Name, out var ls) || ls.Count == 0) continue;
                sb.AppendLine($"\n; ---- {t.Name}: band {t.Min}-{t.Max}. {t.Note}");
                foreach (var l in ls) sb.AppendLine(l);
            }
            var dir = Path.Combine(Out, "SKSE", "Plugins", "SkyPatcher", "npc", cfg.OutputSubfolder);
            Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, "LoreTierLevels.ini"), sb.ToString(), new UTF8Encoding(false));
        }
    }
}
