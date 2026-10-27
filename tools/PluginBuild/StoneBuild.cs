// Njordlinger Combat Gameplay Overhaul - the soul-stone records (PLAN.md phase 4 step 1; D17-D22; config/stones.json).
//
// Records (all appended after phases 2 and 3, so no earlier form ID moves):
//   keywords   NJCGO_Stone, NJCGO_StoneTier_<tier> x5, NJCGO_StoneWord_<word> x60, and the four item tier keywords KID's
//              tier rules use (NJCGO_Tier_Common / Magic / Epic / Legendary - so KID finds these instead of making its own)
//   MISC       300 stones, NJCGO_Stone_<word>_<tier>: the vanilla filled soul gem model of that tier (referenced, not
//              copied - no mesh is redistributed), tier value, the stone keywords and VendorItemSoulGem
//   MGEF       180 word effects (60 words x melee / ranged / armour) and 60 phrase effects (20 x 3): a real effect where
//              the cell is 'rec', otherwise an inert line whose description shows on the item card; melee and ranged ones
//              deliver on contact, armour ones are constant on the wearer. Shout copies take the Shout's hit shader,
//              hit art, impact data and hit sounds. Plus 5 tier glow effects (a recoloured enchant shader each), two
//              'a phrase you do not yet know' lines, and the helper spells' effects
//   EFSH       5 tier glow shaders, copied from a vanilla weapon enchant shader and recoloured to the soul-gem palette
//   PERK/SPEL  one perk per 'perk' cell and one ability per 'self' cell (the SKSE plugin gives them to the player while
//              the item is equipped, and sets their value from the stone's tier); helper spells the plugin casts
//   LVLI       one list per stone tier (the 60 words), one loot list over the tiers by level, one merchant list
//   SHOU/WOOP  a hidden clone of each Shout a phrase grants on armour (D21) and of its three words
// The magnitude by tier lives on the composed enchantment's effect entry (the SKSE plugin builds it), not on the record.
using System.Drawing;
using System.Text.Json;
using System.Text.Json.Nodes;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;
using Noggog;

static class StoneBuild
{
    static readonly string[] Hosts = { "melee", "ranged", "armour" };

    public static void Run(SkyrimMod mod, ModKey modKey, string repo, JsonNode cfg, JsonNode scheme,
        Dictionary<string, FormKey> keywords, JsonObject forms, List<string> stubs, List<string> approx, List<string> dllDone,
        Func<JsonNode, string, Condition> makeCondition, Func<APerkEntryPointEffect.EntryType, byte> tabCount, Action? afterBase = null, Action? afterLate = null)
    {
        // ---- the masters ------------------------------------------------------------------------------------------------
        var folders = (Environment.GetEnvironmentVariable("NJCGO_MASTERS") ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries);
        string Find(string file) => folders.Select(f => Path.Combine(f, file)).FirstOrDefault(File.Exists)
            ?? throw new InvalidOperationException($"soul stones: {file} not found - set NJCGO_MASTERS to the folders holding Skyrim.esm, Dawnguard.esm and Dragonborn.esm");
        var masters = new List<ISkyrimModGetter>();
        foreach (var m in new[] { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "Dragonborn.esm" })
        {
            var path = folders.Select(f => Path.Combine(f, m)).FirstOrDefault(File.Exists);
            if (path == null && m == "Update.esm") { continue; }
            masters.Add(SkyrimMod.CreateFromBinaryOverlay(path ?? Find(m), SkyrimRelease.SkyrimSE));
        }
        var cache = masters.ToImmutableLinkCache();
        // Q69 (Fable 19:00): build-time copies follow the INSTALLED Stormcrown where the copied effect references no
        // Stormcrown-owned form; otherwise the vanilla copy. Its plugins are read (never made masters) from NJCGO_SOFT -
        // folders separated by ';' - when they are there; without them every copy is vanilla.
        var masterKeys = masters.Select(m => m.ModKey).ToHashSet();
        var softFolders = (Environment.GetEnvironmentVariable("NJCGO_SOFT") ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries);
        var soft = new List<ISkyrimModGetter>();
        foreach (var name in new[] { "Stormcrown.esp" })
        {
            var path = softFolders.Select(f => Path.Combine(f, name)).FirstOrDefault(File.Exists);
            if (path != null) { soft.Add(SkyrimMod.CreateFromBinaryOverlay(path, SkyrimRelease.SkyrimSE)); }
        }
        var fullCache = masters.Concat(soft).ToImmutableLinkCache();
        Console.WriteLine($"stones: Shout copies read {(soft.Count > 0 ? string.Join(", ", soft.Select(m => m.ModKey.FileName)) + " where its forms are vanilla" : "the vanilla masters only")}");
        string Ref(IFormKeyGetter fk) => fk.FormKey.ModKey == modKey ? $"0x{fk.FormKey.ID:X3}" : $"{fk.FormKey.ModKey.FileName}|0x{fk.FormKey.ID:X}";
        string Hex(IMajorRecordGetter r) => $"0x{r.FormKey.ID:X3}";

        T ByEdid<T>(string edid) where T : class, IMajorRecordGetter
        {
            foreach (var m in Enumerable.Reverse(masters))
            {
                foreach (var r in m.EnumerateMajorRecords<T>()) { if (r.EditorID == edid) { return r; } }
            }
            throw new InvalidOperationException($"soul stones: {typeof(T).Name} {edid} is not in the masters");
        }
        T? TryEdid<T>(string edid) where T : class, IMajorRecordGetter
        {
            try { return ByEdid<T>(edid); } catch (InvalidOperationException) { return null; }
        }
        // The player's Shouts by name, through their editor IDs (many dragon and NPC Shouts share a player Shout's name).
        var shoutEdids = cfg["shoutEdids"]!.AsObject().ToDictionary(kv => kv.Key, kv => kv.Value!.GetValue<string>());
        IShoutGetter ShoutNamed(string name) => shoutEdids.TryGetValue(name, out var e) ? ByEdid<IShoutGetter>(e)
            : throw new InvalidOperationException($"soul stones: '{name}' is not in stones.json's shoutEdids");
        ISpellGetter ShoutSpell(string name, int level, ILinkCache? c = null)
        {
            var sh = (c ?? cache).Resolve<IShoutGetter>(ShoutNamed(name).FormKey);
            var w = sh.WordsOfPower[Math.Clamp(level, 1, 3) - 1];
            return (c ?? cache).Resolve<ISpellGetter>(w.Spell.FormKey);
        }
        IMagicEffectGetter? FirstEffectOf(ISpellGetter s, ILinkCache? c = null) =>
            s.Effects.Select(e => e.BaseEffect.TryResolve(c ?? cache)).FirstOrDefault(x => x != null);

        foreach (var name in cfg["vanillaKeywords"]!.AsArray().Select(n => n!.GetValue<string>()))
        {
            keywords[name] = ByEdid<IKeywordGetter>(name).FormKey;
        }

        // ---- keywords ---------------------------------------------------------------------------------------------------
        var tiers = cfg["tiers"]!.AsArray().Select(n => n!.GetValue<string>()).ToArray();
        float Lerp(JsonNode? n, int t, float fallback = 0)
        {
            if (n == null) { return fallback; }
            if (n is JsonArray a) { var lo = a[0]!.GetValue<float>(); var hi = a[^1]!.GetValue<float>(); return lo + (hi - lo) * t / (tiers.Length - 1); }
            return n.GetValue<float>();
        }
        JsonArray ByTier(JsonNode? n, float fallback = 0) => new(Enumerable.Range(0, tiers.Length).Select(t => (JsonNode)JsonValue.Create(MathF.Round(Lerp(n, t, fallback), 3))!).ToArray());

        var itemTierKw = new JsonObject();
        foreach (var t in new[] { "Common", "Magic", "Epic", "Legendary" })
        {
            itemTierKw[t] = Hex(mod.Keywords.AddNew("NJCGO_Tier_" + t));
        }
        var stoneKw = mod.Keywords.AddNew(scheme["stoneFamilyKeyword"]!.GetValue<string>());
        var tierKws = tiers.Select(t => mod.Keywords.AddNew("NJCGO_StoneTier_" + t)).ToArray();
        var wordDefs = scheme["words"]!.AsArray();
        var wordKws = new Dictionary<string, Keyword>();
        foreach (var w in wordDefs) { wordKws[w!["word"]!.GetValue<string>()] = mod.Keywords.AddNew(w["keyword"]!.GetValue<string>()); }

        // ---- glow shaders and effects (one per stone tier) --------------------------------------------------------------
        var glowSrc = ByEdid<IMagicEffectGetter>(cfg["glowShaderFrom"]!.GetValue<string>());
        var glowShaderSrc = glowSrc.EnchantShader.TryResolve(cache) ?? throw new InvalidOperationException("soul stones: the glow source effect has no enchant shader");
        var colours = cfg["tierColour"]!.AsArray().Select(n => ColorTranslator.FromHtml(n!.GetValue<string>())).ToArray();
        var glowForms = new List<string>();
        for (var t = 0; t < tiers.Length; ++t)
        {
            var sh = mod.EffectShaders.DuplicateInAsNewRecord(glowShaderSrc, $"NJCGO_StoneGlowShader_{tiers[t]}");
            sh.FillColorKey1 = colours[t];
            sh.FillColorKey2 = colours[t];
            sh.FillColorKey3 = colours[t];
            sh.EdgeColor = colours[t];
            // aed2 / aa48: the copied frost shader takes its membrane colour from a palette texture (GradFrostEnch.dds), so
            // the fill keys alone did nothing; the palette is dropped and every colour of the shader is the tier's
            // ed4e: still pale (the grey flame texture lit the colour keys to near white) - a palette of the tier colour
            // (tools/make-glow-palettes.py, our own DDS) maps the texture's brightness to saturated tier colours
            sh.MembranePaletteTexture = $@"Effects\NjordlingerCGO\StoneGlow_{tiers[t]}.dds";
            sh.EdgeEffectColor = Color.FromArgb(255, colours[t]);
            sh.ColorKey1 = colours[t];
            sh.ColorKey2 = colours[t];
            sh.ColorKey3 = colours[t];
            // 68e5: the shell read pale lavender - the membrane ADDS its colour over the lit blade, so a dark tier colour
            // washes out. It now blends over the blade (source alpha / inverse source alpha) at full strength, steadier,
            // with a wider edge.
            sh.FillPersistentAlphaRatio = 1f;
            sh.FillFullAlphaRatio = 1f;
            sh.FillAlphaPulseAmplitude = 0.25f;
            sh.EdgeEffectPersistentAlphaRatio = 1f;
            sh.EdgeEffectFallOff = 0.6f;
            sh.MembraneDestBlendMode = EffectShader.BlendMode.SourceInvertedAlpha;
            var mg = mod.MagicEffects.AddNew($"NJCGO_StoneGlow_{tiers[t]}");
            mg.Name = $"Soul stone glow ({tiers[t]})";
            mg.Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.Script);
            mg.CastType = CastType.FireAndForget;
            mg.TargetType = TargetType.Touch;
            mg.Flags = MagicEffect.Flag.NoArea | MagicEffect.Flag.NoDuration | MagicEffect.Flag.HideInUI | MagicEffect.Flag.NoHitEvent | MagicEffect.Flag.NoHitEffect | MagicEffect.Flag.Painless;
            if (glowSrc.Flags.HasFlag(MagicEffect.Flag.FXPersist)) { mg.Flags |= MagicEffect.Flag.FXPersist; }
            mg.BaseCost = 10000f;   // aed2: the game draws ONE effect's enchant shader on a weapon - the costliest
            mg.MagicSkill = ActorValue.None;
            mg.ResistValue = ActorValue.None;
            mg.SecondActorValue = ActorValue.None;
            mg.EnchantShader.SetTo(sh.FormKey);
            mg.EnchantArt.SetTo(glowSrc.EnchantArt.FormKeyNullable);
            glowForms.Add(Hex(mg));
        }

        // Q66 (Fable 14:20): a cell not built yet never reads as active - "<line> (dormant)" on the card and the page.
        string CardText(JsonNode cell, bool forCard = true)
        {
            var d = cell["d"]!.GetValue<string>();
            if (!forCard) { return d + (cell["k"]?.GetValue<string>() == "stub" ? " (dormant)" : ""); }   // 6616: the page fills the tier's own value
            // efc8: a fraction shown as a percent (AttackDamageMult 0.2 = 20% less) - the engine prints the magnitude as a whole
            // number ("0%"), so the card names the tier range instead; the page fills the tier's own value
            if (d.Contains("<mag>%") && cell["m"] is JsonArray ma && ma.Count == 2 && ma.All(v => v!.GetValue<double>() <= 1.0))
            {
                d = d.Replace("<mag>%", $"{ma[0]!.GetValue<double>() * 100:0}-{ma[1]!.GetValue<double>() * 100:0}%");
            }
            return d + (cell["k"]?.GetValue<string>() == "stub" ? " (dormant)" : "");
        }

        // ---- the effect of one cell -------------------------------------------------------------------------------------
        int recCount = 0, markerCount = 0;
        MagicEffect CellEffect(string edid, string name, string host, JsonNode cell)
        {
            var kind = cell["k"]!.GetValue<string>();
            var weapon = host != "armour";
            var mg = mod.MagicEffects.AddNew(edid);
            mg.Name = name;
            mg.Description = CardText(cell);
            mg.MagicSkill = ActorValue.None;
            mg.ResistValue = ActorValue.None;
            mg.SecondActorValue = ActorValue.None;
            mg.CastType = weapon ? CastType.FireAndForget : CastType.ConstantEffect;
            mg.TargetType = weapon ? TargetType.Touch : TargetType.Self;
            var isRec = kind == "rec";
            if (!isRec)
            {
                // an inert line: it puts the effect's text on the item card (and Active Effects for armour) and nothing else
                mg.Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.Script);
                mg.Flags = MagicEffect.Flag.NoArea | MagicEffect.Flag.NoDuration | MagicEffect.Flag.NoMagnitude | MagicEffect.Flag.Painless;
                if (weapon) { mg.Flags |= MagicEffect.Flag.NoHitEvent | MagicEffect.Flag.NoHitEffect; }
                else { mg.Flags |= MagicEffect.Flag.Recover; }
                ++markerCount;
                return mg;
            }
            var arch = Enum.Parse<MagicEffectArchetype.TypeEnum>(cell["a"]?.GetValue<string>() ?? "ValueModifier");
            mg.Archetype = new MagicEffectArchetype(arch);
            var av = cell["av"]?.GetValue<string>();
            if (av != null) { mg.Archetype.ActorValue = Enum.Parse<ActorValue>(av); }
            else
            {
                mg.Archetype.ActorValue = arch switch
                {
                    MagicEffectArchetype.TypeEnum.Paralysis => ActorValue.Paralysis,
                    MagicEffectArchetype.TypeEnum.Calm or MagicEffectArchetype.TypeEnum.Frenzy => ActorValue.Aggression,
                    MagicEffectArchetype.TypeEnum.Demoralize => ActorValue.Confidence,
                    MagicEffectArchetype.TypeEnum.DetectLife => ActorValue.DetectLifeRange,
                    _ => ActorValue.None
                };
            }
            var hasDur = cell["dur"] != null && weapon;
            mg.Flags = MagicEffect.Flag.NoArea;
            if (!hasDur) { mg.Flags |= MagicEffect.Flag.NoDuration; }
            if (!weapon) { mg.Flags |= MagicEffect.Flag.Recover; }
            if (weapon && hasDur && arch == MagicEffectArchetype.TypeEnum.ValueModifier && cell["mkw"] == null && av != "Health") { mg.Flags |= MagicEffect.Flag.Recover; }
            if (cell["det"]?.GetValue<bool>() == true)
            {
                mg.Flags |= MagicEffect.Flag.Detrimental;
                if (weapon) { mg.Flags |= MagicEffect.Flag.Hostile; }
            }
            if (cell["res"] is JsonNode res) { mg.ResistValue = Enum.Parse<ActorValue>(res.GetValue<string>()); }
            if (cell["mkw"] is JsonNode mkw)
            {
                mg.Keywords = new ExtendedList<IFormLinkGetter<IKeywordGetter>> { new FormLink<IKeywordGetter>(keywords[mkw.GetValue<string>()]) };
            }
            var conds = new List<Condition>();
            if (cell["if"] is JsonArray ifs) { foreach (var c in ifs) { conds.Add(makeCondition(c!, edid)); } }
            if (cell["chance"] is JsonNode ch)
            {
                conds.Add(makeCondition(new JsonObject { ["f"] = "GetRandomPercent", ["op"] = "<", ["v"] = ch.GetValue<float>() }, edid));
            }
            foreach (var c in conds) { mg.Conditions.Add(c); }
            IMagicEffectGetter? src = null, installed = null;
            if (cell["copy"] is JsonNode cp)
            {
                src = FirstEffectOf(ShoutSpell(cp.GetValue<string>(), 3));
                installed = FirstEffectOf(ShoutSpell(cp.GetValue<string>(), 3, fullCache), fullCache);
            }
            else if (cell["copyMgef"] is JsonNode cm)
            {
                src = TryEdid<IMagicEffectGetter>(cm.GetValue<string>());
                if (src != null) { installed = fullCache.Resolve<IMagicEffectGetter>(src.FormKey); }
            }
            if (src != null) { CopyVisuals(mg, src, installed); }
            ++recCount;
            return mg;
        }

        void CopyVisuals(MagicEffect mg, IMagicEffectGetter src, IMagicEffectGetter? installed = null)
        {
            // each link from the installed version when it names a form of our masters, else the vanilla one (Q69)
            FormKey? Pick(FormKey? inst, FormKey? van) => inst is FormKey k && masterKeys.Contains(k.ModKey) ? inst : van;
            mg.HitShader.SetTo(Pick(installed?.HitShader.FormKeyNullable, src.HitShader.FormKeyNullable));
            mg.HitEffectArt.SetTo(Pick(installed?.HitEffectArt.FormKeyNullable, src.HitEffectArt.FormKeyNullable));
            mg.ImpactData.SetTo(Pick(installed?.ImpactData.FormKeyNullable, src.ImpactData.FormKeyNullable));
            if (installed?.Sounds != null && installed.Sounds.All(x => masterKeys.Contains(x.Sound.FormKey.ModKey))) { src = installed; }
            if (src.Sounds != null)
            {
                var hits = src.Sounds.Where(s => s.Type.ToString().Contains("Hit")).Select(s => s.DeepCopy()).ToList();
                if (hits.Count > 0) { mg.Sounds = new ExtendedList<MagicEffectSound>(hits); }
            }
        }

        // A perk of one entry point for a 'perk' cell; its value is set by the SKSE plugin from the stone's tier.
        int perkCount = 0, selfCount = 0;
        Perk CellPerk(string edid, string name, JsonNode cell)
        {
            var perk = mod.Perks.AddNew(edid);
            perk.Name = name;
            perk.Description = cell["d"]!.GetValue<string>();
            perk.Playable = false;
            perk.Hidden = true;
            perk.NumRanks = 1;
            var ep = Enum.Parse<APerkEntryPointEffect.EntryType>(cell["ep"]!.GetValue<string>());
            var mult = cell["mult"] != null;
            var mv = new PerkEntryPointModifyValue
            {
                EntryPoint = ep,
                Modification = mult ? PerkEntryPointModifyValue.ModificationType.Multiply : PerkEntryPointModifyValue.ModificationType.Add,
                Value = Lerp(mult ? cell["mult"] : cell["add"], 0),
                PerkConditionTabCount = tabCount(ep),
            };
            if (cell["if"] is JsonArray conds)
            {
                foreach (var group in conds.GroupBy(c => c!["tab"]?.GetValue<int>() ?? 0))
                {
                    var pc = new PerkCondition { RunOnTabIndex = (byte)group.Key };
                    foreach (var c in group) { pc.Conditions.Add(makeCondition(c!, edid)); }
                    mv.Conditions.Add(pc);
                }
            }
            perk.Effects.Add(mv);
            ++perkCount;
            return perk;
        }

        // An ability of one constant effect for a 'self' cell; its magnitude is set by the SKSE plugin from the tier.
        Spell CellAbility(string edid, string name, JsonNode cell)
        {
            var spell = mod.Spells.AddNew(edid);
            spell.Name = name;
            spell.Type = SpellType.Ability;
            spell.CastType = CastType.ConstantEffect;
            spell.TargetType = TargetType.Self;
            spell.Description = "";
            var mg = mod.MagicEffects.AddNew(edid.Replace("NJCGO_StoneAb_", "NJCGO_StoneAbFx_"));
            mg.Name = name;
            mg.Description = cell["d"]!.GetValue<string>();
            mg.Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.ValueModifier) { ActorValue = Enum.Parse<ActorValue>(cell["av"]!.GetValue<string>()) };
            mg.CastType = CastType.ConstantEffect;
            mg.TargetType = TargetType.Self;
            mg.Flags = MagicEffect.Flag.Recover | MagicEffect.Flag.NoArea | MagicEffect.Flag.NoDuration | MagicEffect.Flag.HideInUI;
            if (cell["det"]?.GetValue<bool>() == true) { mg.Flags |= MagicEffect.Flag.Detrimental; }
            mg.MagicSkill = ActorValue.None;
            mg.ResistValue = ActorValue.None;
            mg.SecondActorValue = ActorValue.None;
            var effect = new Effect { BaseEffect = mg.ToNullableLink(), Data = new EffectData { Magnitude = Lerp(cell["m"], 0), Area = 0, Duration = 0 } };
            if (cell["if"] is JsonArray conds) { foreach (var c in conds) { effect.Conditions.Add(makeCondition(c!, edid)); } }
            spell.Effects.Add(effect);
            ++selfCount;
            return spell;
        }

        // ---- helper spells (cast by the SKSE plugin; magnitude overridden by the tier) -----------------------------------
        var helperForms = new JsonObject();
        void MakeHelper(string hid, JsonNode def)
        {
            var target = def!["target"]!.GetValue<bool>();
            var spell = mod.Spells.AddNew("NJCGO_StoneFx_" + hid);
            spell.Name = def["name"]!.GetValue<string>();
            spell.Type = SpellType.Spell;
            spell.CastType = CastType.FireAndForget;
            spell.TargetType = target ? TargetType.TargetActor : TargetType.Self;
            spell.Description = "";
            var k = 0;
            foreach (var e in def["effects"]!.AsArray())
            {
                var mg = mod.MagicEffects.AddNew($"NJCGO_StoneFxEf_{hid}_{k++}");
                mg.Name = spell.Name.String;
                var arch = Enum.Parse<MagicEffectArchetype.TypeEnum>(e!["a"]?.GetValue<string>() ?? "ValueModifier");
                mg.Archetype = new MagicEffectArchetype(arch) { ActorValue = Enum.Parse<ActorValue>(e["av"]!.GetValue<string>()) };
                mg.CastType = CastType.FireAndForget;
                mg.TargetType = target ? TargetType.TargetActor : TargetType.Self;
                var dur = e["dur"]?.GetValue<int>() ?? 0;
                mg.Flags = MagicEffect.Flag.NoArea | (dur == 0 ? MagicEffect.Flag.NoDuration : 0);
                if (dur > 0 && e["av"]!.GetValue<string>() != "Health") { mg.Flags |= MagicEffect.Flag.Recover; }
                if (e["det"]?.GetValue<bool>() == true) { mg.Flags |= MagicEffect.Flag.Detrimental | MagicEffect.Flag.Hostile; }
                mg.MagicSkill = ActorValue.None;
                mg.ResistValue = e["res"] is JsonNode r ? Enum.Parse<ActorValue>(r.GetValue<string>()) : ActorValue.None;
                mg.SecondActorValue = ActorValue.None;
                if (e["mkw"] is JsonNode mkw) { mg.Keywords = new ExtendedList<IFormLinkGetter<IKeywordGetter>> { new FormLink<IKeywordGetter>(keywords[mkw.GetValue<string>()]) }; }
                if (e["copyMgef"] is JsonNode cm && TryEdid<IMagicEffectGetter>(cm.GetValue<string>()) is IMagicEffectGetter src) { CopyVisuals(mg, src); }
                spell.Effects.Add(new Effect { BaseEffect = mg.ToNullableLink(), Data = new EffectData { Magnitude = e["m"]!.GetValue<float>(), Area = 0, Duration = dur } });
            }
            helperForms[hid] = Hex(spell);
        }
        foreach (var (hid, def) in cfg["helpers"]!.AsObject())
        {
            if (def!["late"]?.GetValue<bool>() != true && def!["latest"]?.GetValue<bool>() != true) { MakeHelper(hid, def); }
        }

        // A cell's proc, resolved for the SKSE plugin.
        // the Detect Life effect's hit shader: the game's own "seen through walls" look
        string? revealShader = null;
        string RevealShader()
        {
            if (revealShader != null) { return revealShader; }
            foreach (var m in Enumerable.Reverse(masters))
            {
                foreach (var mg in m.MagicEffects.Records)
                {
                    if (mg.EditorID == null || !mg.EditorID.Contains("DetectLife", StringComparison.OrdinalIgnoreCase)) { continue; }
                    if (!mg.HitShader.IsNull) { revealShader = Ref(mg.HitShader); Console.WriteLine($"stones: reveal shader from {mg.EditorID}"); return revealShader; }
                    if (!mg.EnchantShader.IsNull) { revealShader = Ref(mg.EnchantShader); Console.WriteLine($"stones: reveal shader from {mg.EditorID} (enchant shader)"); return revealShader; }
                }
            }
            throw new InvalidOperationException("stones: no Detect Life effect with a shader in the masters");
        }

        JsonObject? ResolveProc(JsonNode? p, string owner)
        {
            if (p == null) { return null; }
            var o = JsonNode.Parse(p.ToJsonString())!.AsObject();
            if (o["cast"] is JsonObject cast)
            {
                string spell = "";
                if (cast["shout"] is JsonNode sh) { o["shoutForm"] = Ref(ShoutNamed(sh.GetValue<string>())); o["level"] = cast["level"]?.GetValue<int>() ?? 1; }
                else if (cast["external"] is JsonNode ex) { o["shoutForm"] = ex.GetValue<string>(); o["level"] = cast["level"]?.GetValue<int>() ?? 1; }
                else if (cast["helper"] is JsonNode hp) { o["helper"] = hp.GetValue<string>(); }   // resolved by the SKSE plugin from the table's helpers (a late helper does not exist yet here)
                else if (cast["edid"] is JsonNode ed) { spell = Ref(ByEdid<ISpellGetter>(ed.GetValue<string>())); }
                else { throw new InvalidOperationException($"{owner}: a proc cast names no spell"); }
                o.Remove("cast");
                if (spell.Length > 0) { o["spell"] = spell; }
            }
            if (o["mag"] is JsonNode mag) { o["mag"] = ByTier(mag); }
            if (o["mult"] is JsonNode mu) { o["mult"] = ByTier(mu); }
            if (o["targetKeyword"] is JsonNode tk) { o["targetKeyword"] = Ref(new FormLink<IKeywordGetter>(keywords[tk.GetValue<string>()])); }
            // stub batch 1: a keyword the nearby targets must carry (ActorTypeUndead, ActorTypeAnimal), and the reveal shader
            if (o["nearKeyword"] is JsonNode nk) { o["nearKeyword"] = Ref(ByEdid<IKeywordGetter>(nk.GetValue<string>())); }
            if (o["op"]?.GetValue<string>() == "shader") { o["shader"] = RevealShader(); }
            return o;
        }

        JsonObject BuildCell(string edid, string name, string host, JsonNode cell, bool phrase)
        {
            var kind = cell["k"]!.GetValue<string>();
            var mg = CellEffect(edid, name, host, cell);
            var o = new JsonObject { ["kind"] = kind, ["effect"] = Hex(mg), ["text"] = CardText(cell, false) };
            if (kind == "rec")
            {
                o["m"] = phrase ? new JsonArray(Enumerable.Range(0, tiers.Length).Select(_ => (JsonNode)JsonValue.Create(Lerp(cell["m"], 0, 1))!).ToArray()) : ByTier(cell["m"], 1);
                o["dur"] = cell["dur"] != null && host != "armour" ? ByTier(cell["dur"]) : ByTier(null);
            }
            else if (kind == "perk")
            {
                var perk = CellPerk(edid.Replace("NJCGO_StoneFx_", "NJCGO_StonePerk_"), name, cell);
                o["perk"] = Hex(perk);
                o["value"] = ByTier(cell["mult"] ?? cell["add"]);
            }
            else if (kind == "self")
            {
                var ab = CellAbility(edid.Replace("NJCGO_StoneFx_", "NJCGO_StoneAb_"), name, cell);
                o["ability"] = Hex(ab);
                o["value"] = ByTier(cell["m"]);
            }
            else if (kind == "proc") { o["proc"] = ResolveProc(cell["proc"], edid); dllDone.Add($"{edid}: {cell["d"]!.GetValue<string>()}"); }
            else if (kind == "filter") { o["filter"] = cell["filter"]!.GetValue<string>(); dllDone.Add($"{edid}: {cell["d"]!.GetValue<string>()}"); }
            else if (kind == "stub") { stubs.Add($"{edid}: {cell["note"]?.GetValue<string>() ?? "not built yet"}"); }
            if (cell["approx"] is JsonNode ap) { approx.Add($"{edid}: {ap.GetValue<string>()}"); }
            return o;
        }

        // ---- the 60 words: stones, effects --------------------------------------------------------------------------------
        var gems = cfg["tierSoulGems"]!.AsArray().Select(n => ByEdid<ISoulGemGetter>(n!.GetValue<string>())).ToArray();
        var values = cfg["tierValue"]!.AsArray().Select(n => n!.GetValue<uint>()).ToArray();
        var vendorSoulGem = keywords["VendorItemSoulGem"];
        var stonesByTier = tiers.Select(_ => new List<MiscItem>()).ToArray();
        var wordOut = new JsonArray();
        var phraseOut = new JsonArray();
        var phrases = new JsonArray(cfg["phrases"]!.AsArray().Where(x => x!["external"] == null).Select(x => JsonNode.Parse(x!.ToJsonString())).ToArray());
        var latePhrases = cfg["phrases"]!.AsArray().Where(x => x!["external"] != null).ToList();
        var wordIndex = 0;
        for (var p = 0; p < phrases.Count; ++p)
        {
            var ph = phrases[p]!;
            var words = ph["words"]!.AsArray().Select(n => n!.GetValue<string>()).ToArray();
            for (var i = 0; i < 3; ++i)
            {
                var word = words[i];
                if (wordDefs[wordIndex]!["word"]!.GetValue<string>() != word) { throw new InvalidOperationException($"stones.json word {word} is out of step with the stone scheme ({wordDefs[wordIndex]!["word"]})"); }
                var def = ph["word"]![word] ?? throw new InvalidOperationException($"stones.json: no cells for {word}");
                var stoneForms = new JsonArray();
                for (var t = 0; t < tiers.Length; ++t)
                {
                    var misc = mod.MiscItems.AddNew($"NJCGO_Stone_{word}_{tiers[t]}");
                    misc.Name = $"Soul Stone: {word} ({tiers[t]})";
                    misc.Model = gems[t].Model?.DeepCopy();
                    misc.PickUpSound.SetTo(gems[t].PickUpSound.FormKeyNullable);
                    misc.PutDownSound.SetTo(gems[t].PutDownSound.FormKeyNullable);
                    misc.Value = values[t];
                    misc.Weight = 0.1f;
                    misc.Keywords = new ExtendedList<IFormLinkGetter<IKeywordGetter>>
                    {
                        stoneKw.ToLink(), tierKws[t].ToLink(), wordKws[word].ToLink(), new FormLink<IKeywordGetter>(vendorSoulGem)
                    };
                    stonesByTier[t].Add(misc);
                    stoneForms.Add(Hex(misc));
                }
                var hostsOut = new JsonObject();
                foreach (var h in Hosts)
                {
                    var cell = def[h] ?? throw new InvalidOperationException($"stones.json: {word} has no {h} cell (every cell is filled, FEASIBILITY 19)");
                    hostsOut[h] = BuildCell($"NJCGO_StoneFx_{word}_{h}", word, h, cell, false);
                }
                wordOut.Add(new JsonObject
                {
                    ["word"] = word, ["phrase"] = p, ["position"] = i, ["keyword"] = Hex(wordKws[word]), ["stones"] = stoneForms, ["hosts"] = hostsOut
                });
                ++wordIndex;
            }
            var shoutName = ph["shout"]!.GetValue<string>();
            var shout = ShoutNamed(shoutName);
            var phHosts = new JsonObject();
            var phraseName = string.Join(" ", words);
            JsonObject? grant = null;
            foreach (var h in Hosts)
            {
                var cell = ph["phrase"]![h] ?? throw new InvalidOperationException($"stones.json: {phraseName} has no {h} phrase");
                phHosts[h] = BuildCell($"NJCGO_StoneFx_{words[0]}{words[1]}{words[2]}_{h}", phraseName, h, cell, true);
                if (cell["k"]!.GetValue<string>() == "grant")
                {
                    // D21: a hidden clone of the Shout and its three words; the SKSE plugin marks the clone's words known
                    // up to the stone count and adds / removes the clone from the player.
                    var clone = mod.Shouts.DuplicateInAsNewRecord(shout, $"NJCGO_StoneShout_{new string(shoutName.Where(char.IsLetter).ToArray())}");
                    clone.Name = $"{shoutName} (Soul Stones)";
                    var cloneWords = new JsonArray();
                    for (var i = 0; i < 3; ++i)
                    {
                        var srcWord = cache.Resolve<IWordOfPowerGetter>(shout.WordsOfPower[i].Word.FormKey);
                        var w = mod.WordsOfPower.DuplicateInAsNewRecord(srcWord, $"NJCGO_StoneWord{i + 1}_{new string(shoutName.Where(char.IsLetter).ToArray())}");
                        clone.WordsOfPower[i].Word.SetTo(w.FormKey);
                        cloneWords.Add(Hex(w));
                    }
                    grant = new JsonObject { ["shout"] = Hex(clone), ["words"] = cloneWords };
                    dllDone.Add($"{phraseName} armour: the Shout {shoutName} granted as a hidden clone (D21)");
                }
            }
            var realWords = new JsonArray(shout.WordsOfPower.Select(w => (JsonNode)JsonValue.Create(Ref(w.Word))!).ToArray());
            // the word records' own names are written for the dragon font (e.g. "D4" for Dah): kept for the glyph mapping
            var glyphNames = new JsonArray(shout.WordsOfPower.Select(w => (JsonNode)JsonValue.Create(cache.Resolve<IWordOfPowerGetter>(w.Word.FormKey).Name?.String ?? "")!).ToArray());
            var po = new JsonObject { ["glyphNames"] = glyphNames, ["shout"] = shoutName, ["shoutForm"] = Ref(shout), ["words"] = new JsonArray(words.Select(w => (JsonNode)JsonValue.Create(w)!).ToArray()), ["wordForms"] = realWords, ["hosts"] = phHosts };
            if (grant != null) { po["grant"] = grant; }
            phraseOut.Add(po);
        }

        // 'A phrase you do not yet know' (D20, FEASIBILITY 17.1): one line for weapons, one for armour.
        var unknown = new JsonObject();
        foreach (var (h, weapon) in new[] { ("weapon", true), ("armour", false) })
        {
            var mg = CellEffect($"NJCGO_StonePhraseUnknown_{h}", "Soul stones", weapon ? "melee" : "armour",
                new JsonObject { ["k"] = "marker", ["d"] = "The stones spell a phrase you do not yet know." });
            unknown[h] = Hex(mg);
        }

        // ---- loot and merchants (Q14: loot by zone tier, merchants, the runesmith) -----------------------------------------
        var tierLists = new List<LeveledItem>();
        for (var t = 0; t < tiers.Length; ++t)
        {
            var ll = mod.LeveledItems.AddNew($"NJCGO_StoneList_{tiers[t]}");
            ll.Entries = new ExtendedList<LeveledItemEntry>(stonesByTier[t].Select(s => new LeveledItemEntry
            {
                Data = new LeveledItemEntryData { Level = 1, Count = 1, Reference = s.ToLink<IItemGetter>() }
            }));
            tierLists.Add(ll);
        }
        var levels = cfg["lootLevels"]!.AsArray().Select(n => n!.GetValue<short>()).ToArray();
        LeveledItem OverTiers(string edid, byte chanceNone)
        {
            var ll = mod.LeveledItems.AddNew(edid);
            ll.ChanceNone = new Percent(chanceNone / 100.0);
            ll.Flags = LeveledItem.Flag.CalculateFromAllLevelsLessThanOrEqualPlayer;
            ll.Entries = new ExtendedList<LeveledItemEntry>(tierLists.Select((l, t) => new LeveledItemEntry
            {
                Data = new LeveledItemEntryData { Level = levels[t], Count = 1, Reference = l.ToLink<IItemGetter>() }
            }));
            return ll;
        }
        var loot = OverTiers("NJCGO_StoneLoot", cfg["lootChanceNone"]!.GetValue<byte>());
        var vendor = OverTiers("NJCGO_StoneVendor", 0);
        string Sky(IMajorRecordGetter r) => $"{r.FormKey.ModKey.FileName}|{r.FormKey.ID:X8}";
        string Own(IMajorRecordGetter r) => $"{modKey.FileName}|{r.FormKey.ID:X}";
        var spLines = new List<string>
        {
            "; Njordlinger Combat Gameplay Overhaul - soul stones in loot and at merchants (GENERATED by tools\\PluginBuild from",
            "; config\\stones.json). PLAN.md Q14: stones come from loot by zone tier, merchants and the runesmith. Our lists pick the",
            "; stone tier by level (petty from 1, lesser 10, common 20, greater 30, grand 40), so the zone's level sets the tier.",
            ""
        };
        var lootLists = cfg["lootLists"]!.AsArray().Select(n => TryEdid<ILeveledItemGetter>(n!.GetValue<string>())).Where(x => x != null).ToList();
        var vendorLists = cfg["vendorLists"]!.AsArray().Select(n => TryEdid<ILeveledItemGetter>(n!.GetValue<string>())).Where(x => x != null).ToList();
        // D22 revised (STONE-CRAFTING.md 4): stones are neither loot nor merchandise - the SkyPatcher file is no longer
        // written (tools/PluginBuild moves an old one aside, never deletes it); the levelled lists stay so no form ID moves.
        _ = spLines; _ = Sky(loot); _ = Own(vendor);
        var spDir = Path.Combine(repo, "data", "SKSE", "Plugins", "SkyPatcher", "leveledList", "NjordlingerCGO");
        var spFile = Path.Combine(spDir, "SoulStones.ini");
        if (File.Exists(spFile))
        {
            var keep = Path.Combine(repo, "_retired", "SkyPatcher-SoulStones.ini." + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
            Directory.CreateDirectory(Path.GetDirectoryName(keep)!);
            File.Move(spFile, keep);
            Console.WriteLine($"stones: the SkyPatcher loot file moved aside to {keep}");
        }

        // ---- I4: soul-gem icon in the tier colour (PLAN.md phase 4 step 5) --------------------------------------------------
        var i4Rules = new JsonArray();
        for (var t = 0; t < tiers.Length; ++t)
        {
            i4Rules.Add(new JsonObject
            {
                ["match"] = new JsonObject { ["formType"] = "MiscItem", ["keywords"] = new JsonArray("NJCGO_StoneTier_" + tiers[t]) },
                ["assign"] = new JsonObject { ["iconSource"] = "skyui/icons_item_psychosteve.swf", ["iconLabel"] = "soulgem_full", ["iconColor"] = cfg["tierColour"]![t]!.GetValue<string>() }
            });
            // soul gem dust of the tier: the same icon in the tier colour (the keyword is built late; I4 matches by name)
            i4Rules.Add(new JsonObject
            {
                ["match"] = new JsonObject { ["formType"] = "MiscItem", ["keywords"] = new JsonArray("NJCGO_DustTier_" + tiers[t]) },
                ["assign"] = new JsonObject { ["iconSource"] = "skyui/icons_item_psychosteve.swf", ["iconLabel"] = "soulgem_full", ["iconColor"] = cfg["tierColour"]![t]!.GetValue<string>() }
            });
        }
        // Combined run 68e5: I4 reads a rule file ONLY when a loaded plugin carries the same name (its log listed
        // Skyrim.json, Dawnguard.json, I4WeaponIconsOverhaul.json ... and skipped ours and every CC file whose plugin is
        // absent). So the tier colours (config/i4-tier-colours.json, phase 1) and the stone icons go into ONE file named
        // after our plugin.
        var tierRules = JsonNode.Parse(File.ReadAllText(Path.Combine(repo, "config", "i4-tier-colours.json")))!["rules"]!.AsArray();
        var all = new JsonArray(tierRules.Select(r => JsonNode.Parse(r!.ToJsonString())).Concat(i4Rules.Select(r => JsonNode.Parse(r!.ToJsonString()))).ToArray());
        var i4 = new JsonObject { ["$schema"] = "https://raw.githubusercontent.com/Exit-9B/InventoryInjector/main/docs/InventoryInjector.schema.json", ["rules"] = all };
        Directory.CreateDirectory(Path.Combine(repo, "data", "SKSE", "Plugins", "InventoryInjector"));
        File.WriteAllText(Path.Combine(repo, "data", "SKSE", "Plugins", "InventoryInjector", modKey.Name + ".json"), i4.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");

        forms["stones"] = new JsonObject { ["stoneKeyword"] = Hex(stoneKw), ["loot"] = Hex(loot), ["vendor"] = Hex(vendor) };
        afterBase?.Invoke();

        // ---- the late pass: phrases of Shouts another plugin adds (Stormcrown, its Wyrmstooth patch) ---------------------
        // A soft link - no master: the Shout is named "plugin|0xID" and the SKSE plugin resolves it at load (the phrase is
        // unavailable without the plugin). A word may already exist (Strun, Kest, Laas, Slen, Aus); a new word gets its
        // stones, keyword and cells here. Everything is created after every earlier record, so no earlier form ID moves.
        foreach (var (hid, def) in cfg["helpers"]!.AsObject())
        {
            if (def!["late"]?.GetValue<bool>() == true) { MakeHelper(hid, def); }
        }
        var lateWords = 0;
        foreach (var ph in latePhrases)
        {
            var words = ph!["words"]!.AsArray().Select(n => n!.GetValue<string>()).ToArray();
            for (var i = 0; i < 3; ++i)
            {
                var word = words[i];
                if (wordOut.Any(w => w!["word"]!.GetValue<string>() == word)) { continue; }
                var def = ph["word"]?[word] ?? throw new InvalidOperationException($"stones.json: the new word {word} has no cells");
                var kw = mod.Keywords.AddNew("NJCGO_StoneWord_" + new string(word.Where(char.IsLetter).ToArray()));
                wordKws[word] = kw;
                var stoneForms = new JsonArray();
                for (var t = 0; t < tiers.Length; ++t)
                {
                    var misc = mod.MiscItems.AddNew($"NJCGO_Stone_{word}_{tiers[t]}");
                    misc.Name = $"Soul Stone: {word} ({tiers[t]})";
                    misc.Model = gems[t].Model?.DeepCopy();
                    misc.PickUpSound.SetTo(gems[t].PickUpSound.FormKeyNullable);
                    misc.PutDownSound.SetTo(gems[t].PutDownSound.FormKeyNullable);
                    misc.Value = values[t];
                    misc.Weight = 0.1f;
                    misc.Keywords = new ExtendedList<IFormLinkGetter<IKeywordGetter>> { stoneKw.ToLink(), tierKws[t].ToLink(), kw.ToLink(), new FormLink<IKeywordGetter>(vendorSoulGem) };
                    stonesByTier[t].Add(misc);
                    tierLists[t].Entries!.Add(new LeveledItemEntry { Data = new LeveledItemEntryData { Level = 1, Count = 1, Reference = misc.ToLink<IItemGetter>() } });
                    stoneForms.Add(Hex(misc));
                }
                var hostsOut = new JsonObject();
                foreach (var h in Hosts)
                {
                    var cell = def[h] ?? throw new InvalidOperationException($"stones.json: {word} has no {h} cell");
                    hostsOut[h] = BuildCell($"NJCGO_StoneFx_{word}_{h}", word, h, cell, false);
                }
                wordOut.Add(new JsonObject { ["word"] = word, ["phrase"] = -1, ["position"] = i, ["keyword"] = Hex(kw), ["stones"] = stoneForms, ["hosts"] = hostsOut });
                ++lateWords;
            }
            var phraseName = string.Join(" ", words);
            var phHosts = new JsonObject();
            foreach (var h in Hosts)
            {
                var cell = ph["phrase"]![h] ?? throw new InvalidOperationException($"stones.json: {phraseName} has no {h} phrase");
                if (cell["k"]!.GetValue<string>() == "grant") { throw new InvalidOperationException($"{phraseName}: a soft-linked Shout cannot be granted as a clone (it would need its plugin as a master)"); }
                phHosts[h] = BuildCell($"NJCGO_StoneFx_{words[0]}{words[1]}{words[2]}_{h}", phraseName, h, cell, true);
            }
            var ex = ph["external"]!;
            phraseOut.Add(new JsonObject
            {
                ["shout"] = ph["shout"]!.GetValue<string>(), ["shoutForm"] = $"{ex["plugin"]!.GetValue<string>()}|{ex["id"]!.GetValue<string>()}", ["soft"] = true,
                ["words"] = new JsonArray(words.Select(w => (JsonNode)JsonValue.Create(w)!).ToArray()), ["hosts"] = phHosts
            });
        }
        Console.WriteLine($"stones (late pass): {latePhrases.Count} soft-linked phrases, {lateWords} new words");

        // ---- D22 revised (STONE-CRAFTING.md 2): soul gem dust, built after every earlier record --------------------------
        var dustCfg = cfg["dust"]!;
        var dustModel = ByEdid<IIngredientGetter>(dustCfg["modelFrom"]!.GetValue<string>());
        var dustKw = mod.Keywords.AddNew("NJCGO_Dust");
        var dustForms = new JsonArray();
        for (var t = 0; t < tiers.Length; ++t)
        {
            var tkw = mod.Keywords.AddNew("NJCGO_DustTier_" + tiers[t]);
            var d = mod.MiscItems.AddNew("NJCGO_Dust_" + tiers[t]);
            d.Name = dustCfg["names"]![t]!.GetValue<string>();
            d.Model = dustModel.Model?.DeepCopy();
            d.PickUpSound.SetTo(dustModel.PickUpSound.FormKeyNullable);
            d.PutDownSound.SetTo(dustModel.PutDownSound.FormKeyNullable);
            d.Value = (uint)Math.Max(1, ByEdid<ISoulGemGetter>(dustCfg["valueFrom"]![t]!.GetValue<string>()).Value / 2);
            d.Weight = dustCfg["weight"]!.GetValue<float>();
            d.Keywords = new ExtendedList<IFormLinkGetter<IKeywordGetter>> { dustKw.ToLink(), tkw.ToLink() };
            dustForms.Add(Hex(d));
        }
        var grindOut = new JsonArray();
        foreach (var g in cfg["grind"]!.AsArray())
        {
            grindOut.Add(new JsonObject
            {
                ["gem"] = Ref(ByEdid<ISoulGemGetter>(g!["gem"]!.GetValue<string>())), ["gemEditorId"] = g["gem"]!.GetValue<string>(),
                ["tier"] = g["tier"]!.GetValue<int>(), ["empty"] = g["empty"]!.GetValue<int>(), ["filled"] = g["filled"]!.GetValue<int>()
            });
        }
        var craftOut = new JsonObject
        {
            ["ingot"] = Ref(ByEdid<IMiscItemGetter>(cfg["craft"]!["ingot"]!.GetValue<string>())),
            ["dust"] = JsonNode.Parse(cfg["craft"]!["dust"]!.ToJsonString())
        };
        var addCfg = cfg["addSlot"]!;
        var addOut = new JsonObject
        {
            ["ingot"] = Ref(ByEdid<IMiscItemGetter>(addCfg["ingot"]!.GetValue<string>())),
            ["gems"] = new JsonArray(addCfg["gems"]!.AsArray().Select(g => (JsonNode)new JsonArray(g!.AsArray().Select(n => (JsonNode)JsonValue.Create(Ref(ByEdid<ISoulGemGetter>(n!.GetValue<string>())))!).ToArray())).ToArray())
        };
        var dropsCfg = cfg["drops"]!;
        var dropsOut = JsonNode.Parse(dropsCfg.ToJsonString())!.AsObject();
        dropsOut.Remove("_comment");
        dropsOut["bossLocRefType"] = Ref(ByEdid<ILocationReferenceTypeGetter>(dropsCfg["bossLocRefType"]!.GetValue<string>()));
        var bossKwOut = new JsonArray();
        foreach (var k in dropsCfg["bossKeywords"]!.AsArray())
        {
            var kw = TryEdid<IKeywordGetter>(k!.GetValue<string>());
            if (kw != null) { bossKwOut.Add(Ref(kw)); } else { Console.WriteLine($"stones: drops - keyword {k} is not in the masters (skipped)"); }
        }
        dropsOut["bossKeywords"] = bossKwOut;
        Console.WriteLine($"stones: {dustForms.Count} dust items, {grindOut.Count} grindable gems; drops from dragons and named bosses (D29)");
        afterLate?.Invoke();

        // ---- the owner 23:11: the Rune Blank, built after every other record (the runesmith's late topics included) ------
        var ingotRec = ByEdid<IMiscItemGetter>(cfg["craft"]!["ingot"]!.GetValue<string>());
        var blank = mod.MiscItems.AddNew("NJCGO_RuneBlank");
        blank.Name = cfg["craft"]!["blankName"]!.GetValue<string>();
        blank.Model = ingotRec.Model?.DeepCopy();
        blank.PickUpSound.SetTo(ingotRec.PickUpSound.FormKeyNullable);
        blank.PutDownSound.SetTo(ingotRec.PutDownSound.FormKeyNullable);
        blank.Value = (uint)Math.Max(1, ingotRec.Value / 3);
        blank.Weight = cfg["craft"]!["blankWeight"]!.GetValue<float>();
        craftOut["blank"] = Hex(blank);
        craftOut["blanksPerIngot"] = cfg["craft"]!["blanksPerIngot"]!.GetValue<int>();
        Console.WriteLine($"stones: Rune Blank {Hex(blank)} - {craftOut["blanksPerIngot"]} per orichalcum ingot");
        // stub batch 1: helpers marked "latest", after every other record
        foreach (var (hid, def) in cfg["helpers"]!.AsObject())
        {
            if (def!["latest"]?.GetValue<bool>() == true) { MakeHelper(hid, def); }
        }

        // ---- D26 (SHOUTING-OFF.md 5): the quests that need the Voice while Shouting is off -------------------------------
        // Rows found by the quest's NAME in the masters (Fable's editor IDs were a guess - verified here, not typed); the
        // exemption runs from the quest's start to its completion stage (the stage whose log entry completes the quest).
        // "reserveSoul": no dragon soul is converted until that quest completes (Dragon Rising's soul unlocks Fus).
        var shoutRows = new (string quest, string shout, bool reserve)[]
        {
            ("The Way of the Voice", "Unrelenting Force", true), ("The Way of the Voice", "Whirlwind Sprint", false),
            ("The Throat of the World", "Clear Skies", false), ("The Fallen", "Call Dragon", false),
            ("Alduin's Bane", "Dragonrend", false), ("Dragonslayer", "Dragonrend", false), ("At the Summit of Apocrypha", "Bend Will", false),
        };
        var questsByName = new Dictionary<string, IQuestGetter>();
        foreach (var m in masters) { foreach (var q in m.Quests.Records) { var qn = q.Name?.String; if (!string.IsNullOrEmpty(qn) && !questsByName.ContainsKey(qn)) { questsByName[qn] = q; } } }
        var shoutOut = new JsonArray();
        foreach (var (qname, sname, reserve) in shoutRows)
        {
            // the Dragonborn plugin's names come from strings this build does not load: its quest by editor id
            IQuestGetter? q = questsByName.TryGetValue(qname, out var qq) ? qq : qname == "At the Summit of Apocrypha" ? TryEdid<IQuestGetter>("DLC2MQ06") : null;
            if (q == null) { throw new InvalidOperationException($"shouting: no quest named '{qname}' in the masters"); }
            var done = q.Stages.Where(st => st.LogEntries.Any(le => le.Flags is QuestLogEntry.Flag fl && fl.HasFlag(QuestLogEntry.Flag.CompleteQuest))).Select(st => (int)st.Index).DefaultIfEmpty(-1).Min();
            if (done < 0) { throw new InvalidOperationException($"shouting: quest {q.EditorID} has no completion stage"); }
            // 6616: a start-game-enabled quest runs at stage 0 from the new game on, so the window starts where the quest has
            // actually begun for the player - its first stage with a journal entry (MQ105: arriving at High Hrothgar)
            var begun = q.Stages.Where(st => st.Index > 0 && st.LogEntries.Any(le => !string.IsNullOrWhiteSpace(le.Entry?.String))).Select(st => (int)st.Index).DefaultIfEmpty(1).Min();
            Console.WriteLine($"shouting: {q.EditorID} journal: {string.Join(" | ", q.Stages.Where(st => st.LogEntries.Any(le => !string.IsNullOrWhiteSpace(le.Entry?.String))).Take(6).Select(st => st.Index + ": " + (st.LogEntries.First(le => !string.IsNullOrWhiteSpace(le.Entry?.String)).Entry!.String!.Length > 60 ? st.LogEntries.First(le => !string.IsNullOrWhiteSpace(le.Entry?.String)).Entry!.String![..60] : st.LogEntries.First(le => !string.IsNullOrWhiteSpace(le.Entry?.String)).Entry!.String)))}");
            shoutOut.Add(new JsonObject
            {
                ["quest"] = Ref(q), ["questEditorId"] = q.EditorID, ["questName"] = qname, ["from"] = begun, ["to"] = done - 1,
                ["shout"] = Ref(ShoutNamed(sname)), ["shoutName"] = sname, ["reserveSoul"] = reserve
            });
            Console.WriteLine($"shouting: {sname} exempt while {q.EditorID} ({qname}) is at stage {begun} to {done - 1}{(reserve ? "; souls reserved until it completes" : "")}");
        }
        var shoutingTable = new JsonObject
        {
            ["schema"] = "NjordlingerCGO shouting 1",
            ["note"] = "GENERATED by tools/PluginBuild (SHOUTING-OFF.md 5). Add rows for quest mods by hand in the same shape: a Shout is exempt while its quest is running and its stage is between from and to.",
            ["rows"] = shoutOut
        };
        File.WriteAllText(Path.Combine(repo, "data", "SKSE", "Plugins", "NjordlingerCombatGameplayOverhaul", "shouting.json"), shoutingTable.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");

        // ---- the SKSE plugin's table ------------------------------------------------------------------------------------
        var tierOut = new JsonArray();
        var feeGems = cfg["tierFeeGems"]!.AsArray();
        for (var t = 0; t < tiers.Length; ++t)
        {
            tierOut.Add(new JsonObject
            {
                ["name"] = tiers[t], ["keyword"] = Hex(tierKws[t]), ["glow"] = glowForms[t], ["list"] = Hex(tierLists[t]),
                // D22 revised: the fee is one dust of the tier (tierFeeGems in the config is history)
                ["feeGems"] = new JsonArray((JsonNode)JsonValue.Create(dustForms[t]!.GetValue<string>())!)
            });
        }
        itemTierKw["Unique"] = forms["gear"]?["keywords"]?["tierUnique"]?.GetValue<string>();
        var table = new JsonObject
        {
            ["schema"] = "NjordlingerCGO stones 1",
            ["note"] = "GENERATED by tools/PluginBuild from config/stones.json - do not edit; 0x... = NjordlingerCombatGameplayOverhaul.esp",
            ["lowHealth"] = cfg["lowHealth"]!.GetValue<float>(),
            ["stoneKeyword"] = Hex(stoneKw),
            ["itemTierKeywords"] = itemTierKw,
            ["slotsByItemTier"] = JsonNode.Parse(scheme["slotsByItemTier"]!.ToJsonString()),
            ["tiers"] = tierOut,
            ["unknownPhrase"] = unknown,
            ["loot"] = Hex(loot),
            ["vendor"] = Hex(vendor),
            ["helpers"] = helperForms,
            ["dust"] = dustForms,
            ["grind"] = grindOut,
            ["craft"] = craftOut,
            ["drops"] = dropsOut,
            ["addSlot"] = addOut,
            ["words"] = wordOut,
            ["phrases"] = phraseOut,
        };
        var outDir = Path.Combine(repo, "data", "SKSE", "Plugins", "NjordlingerCombatGameplayOverhaul");
        File.WriteAllText(Path.Combine(outDir, "stones.json"), table.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
        forms["stones"] = new JsonObject
        {
            ["stoneKeyword"] = Hex(stoneKw), ["loot"] = Hex(loot), ["vendor"] = Hex(vendor),
            ["counts"] = new JsonObject
            {
                ["stones"] = stonesByTier.Sum(l => l.Count), ["recEffects"] = recCount, ["markerEffects"] = markerCount,
                ["perks"] = perkCount, ["abilities"] = selfCount, ["helpers"] = helperForms.Count, ["lootLists"] = 0, ["vendorLists"] = 0, ["dust"] = dustForms.Count
            }
        };
        Console.WriteLine($"stones: {stonesByTier.Sum(l => l.Count)} stones, {recCount} effects that act, {markerCount} card lines, {perkCount} perks, {selfCount} abilities, {helperForms.Count} helpers; not in loot or at merchants (D22 revised)");
    }
}
