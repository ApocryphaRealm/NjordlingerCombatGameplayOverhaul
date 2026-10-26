# Njordlinger Combat Gameplay Overhaul - changelog

## 1.0.0 - 2026-10-26 - untested

### Added
- **Item tiers (phase 1 data layer).** Every playable weapon and armour piece gets one tier keyword from Keyword Item
  Distributor: common, magic, epic, legendary or unique. The tier is the smithing level of the perk its crafting recipe
  asks for in the installed perk overhaul (Basic 20 = magic; Journeyman 40 and Intermediate 60 = epic; Advanced 80 and
  Mythic 100 = legendary; no perk = common), read from every version of the recipe so a later copy that dropped the
  perk does not lower the tier. Items with no recipe take the tier of their material; enchanted copies keep their base
  item's tier. Uniques: the Daedric artefacts, ArteFakes' artefacts, listed named weapons (Chillrend, the Nightingale
  Blade...) and enchanted items that no levelled list hands out and no recipe makes. On Njordlinger Test: 16,459 items
  (4,173 common, 2,367 magic, 4,532 epic, 5,098 legendary, 289 unique). New standalone equipment still waiting for the
  author's keep-or-remove mark is left out.
- **Tier colours** for the inventory icons through Inventory Interface Information Injector: grey, blue, purple,
  orange, red.
- **Set keywords** for five unique armour sets: Nightingale, Ancient Shrouded, Guild Master's, Deathbrand, Linwe's.
- **Enemy levels by lore tier** through SkyPatcher: 26 groups by faction and race, each with a level band (bandits
  1-20, Falmer 15-45, giants 30-35...). 510 NPC records on Njordlinger Test.
- **The soul stone keyword scheme** and the slots per item tier (0, 1, 2, 3, 3), for the later soul stone module.
- `tools/build-data.py` and `tools/TierBuild`: the build step that writes the tier, set and level files from a
  profile's load order and verifies every form they name.
