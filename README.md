# Njordlinger Combat Gameplay Overhaul

This mod is its own first build line (no upstream); its version is issued by the version gate.

An overhaul of character progression, classes, gear and soul stones for Skyrim Special Edition (1.5.97): you grow by
doing, your class is a defining choice, an item's tier is where it comes from, and soul stones spell the dragon tongue.
One plugin, one SKSE plugin, one settings page in the Apocrypha Menu Framework, and the data files beside them.

## What is built so far: the data layer (phase 1)

| File | What it does |
|---|---|
| `NjordlingerCGO_Tiers_KID.ini` | One tier keyword on every playable weapon and armour piece: `NJCGO_Tier_Common`, `_Magic`, `_Epic`, `_Legendary`, `_Unique`. The tier comes from the smithing perk the item's crafting recipe asks for; named uniques are unique; an enchanted copy keeps its base item's tier. |
| `NjordlingerCGO_Sets_KID.ini` | Set keywords on the pieces of five unique armour sets (bonuses arrive later). |
| `SKSE\Plugins\InventoryInjector\NjordlingerCGO_TierColours.json` | Inventory icon colour by tier: grey, blue, purple, orange, red. |
| `SKSE\Plugins\SkyPatcher\npc\NjordlingerCGO\LoreTierLevels.ini` | Enemy levels by lore tier: bandits stay low, dragons and Dwemer machines high. A level-scaled NPC is held inside its tier's band; a fixed-level one above the band is brought down to its top. Named NPCs are never touched. |
| `SKSE\Plugins\NjordlingerCombatGameplayOverhaul\stone-scheme.json` | The reserved names for soul stones (sixty words, five stone tiers) and the stone slots each item tier gives (0 / 1 / 2 / 3 / 3). |

The keywords need no plugin: Keyword Item Distributor creates them (the set keywords and `NJCGO_Tier_Unique` are
records of the plugin since the gear build, so KID finds them there).

## The gear (phase 3)

| File | What it does |
|---|---|
| `NjordlingerCGO_Uniques_KID.ini` | Every unique-tier item gets `NJCGO_NoEnchant` and the game's `MagicDisallowEnchanting` (it cannot be enchanted); each authored unique gets its own `NJCGO_Unique_<id>` keyword. |
| `NjordlingerCGO_Gear_DISTR.ini` | SPID: every actor carries one perk per set and one per authored unique. Their entry points and abilities hold only while the keyword is worn and the module is switched on. |
| `NjordlingerCombatGameplayOverhaul.esp` | The set and unique keywords, the 2- and 4-piece abilities, the set and unique perks and abilities, the two switch globals. |
| `SKSE\Plugins\NjordlingerCombatGameplayOverhaul\gear.json` | The gear records' form IDs for the SKSE plugin. |

No condition can count the pieces of one set, so the SKSE plugin does: on every equip and unequip and whenever an actor
loads it counts the worn armour carrying each set keyword and adds or removes the set's 2- and 4-piece abilities (the
set perk's entry points also require them). At the first game load it adds the no-enchant keywords to any unique-tier
item KID missed. The content (every number a default to tune) is `config\gear-content.json`; `tools\PluginBuild`
builds the records, both INIs and the form list, and `tools\build-classes.py` writes `gear.json` and the INI.

## Soul stones (phase 4)

A soul stone is a word of the dragon tongue: the sixty words of the twenty Shouts, each in five soul-gem tiers (petty to
grand). Gear's tier gives it slots (magic 1, epic 2, legendary and unique 3; staves none). A stone acts for the item it
sits in - a melee weapon, a bow or crossbow, or armour - and three stones of one Shout in word order add its phrase once
all three words are unlocked with dragon souls; a Shout-only phrase on armour lets you shout that Shout while it is worn,
up to the words you have learned at a wall. Stones are set and taken out by the runesmith, Thorvar Stone-Speaker at the
Sleeping Giant Inn in Riverwood, after his short quest, for one soul gem of the stone's tier each time.

| File | What it does |
|---|---|
| `NjordlingerCombatGameplayOverhaul.esp` | The 300 stones (the vanilla soul gem models, referenced), the word and phrase effects, perks, abilities, helper spells, tier glow shaders, the Shout clones, loot lists, the runesmith, his quest, dialogue and shop. |
| `Scripts\*.pex` (+ `Scripts\Source`) | Three dialogue fragments: the quest's two stages and his barter menu. |
| `SKSE\Plugins\SkyPatcher\leveledList\NjordlingerCGO\SoulStones.ini` | Stones in boss chests (tier by the place's level) and where soul gems are sold. |
| `SKSE\Plugins\InventoryInjector\NjordlingerCGO_StoneIcons.json` | A soul-gem icon in the tier colour (I4). |
| `SKSE\Plugins\NjordlingerCombatGameplayOverhaul\stones.json`, `runesmith.json` | The records' form IDs and the per-tier numbers for the SKSE plugin. |

The SKSE plugin keeps each item's stones in the co-save, composes the item's enchantment (its own enchantment kept),
applies the effects no enchantment can carry while the item is equipped, names the item after its words and makes a
drawn weapon glow. Content: `config\stones.json` (every number a default to tune) and `config\runesmith.json`; built by
`tools\PluginBuild` (needs `NJCGO_MASTERS` = the folders holding Skyrim.esm, Dawnguard.esm and Dragonborn.esm) and
`tools\build-papyrus.py` (needs the Papyrus compiler and the game's script sources). `tools\make-glyphs.py` renders the
glyph atlas from the game's own dragon font; `tools\word-numbers.py` writes the numbers into the design's word table.

## Requirements

- SKSE64 for 1.5.97
- Keyword Item Distributor (KID) 3.x
- SkyPatcher 7.x
- Spell Perk Item Distributor (SPID) 7.x - the gear effects
- Inventory Interface Information Injector (I4) for Skyrim 1.5 - for the tier colours
- Arena - An Encounter Zone Overhaul and Open World Loot - the zone and loot halves of "enemies by lore tier"
- Apocrypha Menu Framework (the settings pages); later builds: Souls to Perks

## Building the data files

The tier and NPC files are generated against a load order, so they follow the list they are built for:

```
py -3 tools\build-data.py --instance <MO2 instance> --profile "<profile>"
```

It stages the profile's active plugins (hardlinks, read only), runs `tools\TierBuild` (C#, Mutagen; needs the .NET 9 SDK),
copies the results into `data\`, and verifies that every form the files name exists in that load order. The rules it
applies live in `config\` (`tiers.json`, `sets.json`, `npc-tiers.json`); `generated\build-report.txt` says what it
decided and why. `tools\make-stone-scheme.py` writes the stone scheme from the design's word table.

## Licence

GPL-3.0-or-later (see `LICENSE`): later builds carry code from Character Progression Control, which is GPL-3.0.
Author: ApocryphaRealm.
