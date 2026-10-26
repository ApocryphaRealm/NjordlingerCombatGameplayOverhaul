# Njordlinger Combat Gameplay Overhaul - changelog

## 1.0.0 - 2026-10-26 - untested

### Added (phase 2 - progression and classes, the SKSE plugin)
- **The SKSE plugin**, both build lines (Skyrim SE 1.5.97 / AE 1.6.1170 and Skyrim 1.7.x), installed as a FOMOD, with
  a settings page in the Apocrypha Menu Framework: Progression, Classes, Quest Rewards.
- **Progression**, copied from Character Progression Control and trimmed: one perk point per level up (a table by
  level), no health, magicka or stamina from a level up and no carry weight from it (Adaptive Attributes is the one
  carry-weight writer); while all three gains are 0 the attribute choice is made for you, so a level up just grants
  its perk points. Optional skill caps and skill experience rates.
- **Classes**: Warrior, Mage or Thief, chosen in the race menu's new Class category or on the Classes tab. A class
  holds a skill group; the other groups' skills stay at their starting values and bank no use, their perks cannot be
  taken and their entry points do nothing for you (other characters are not affected), and their crafting station
  refuses you. Class levels come from your class's skills; every 10 class levels (a setting) the class evolves through
  Novice, Apprentice, Adept, Expert, Master and Legendary, each time one of two options, for good: the Apprentice and
  Adept classes of the lineage (a hybrid opens its second group), two rank paths at Expert and Master, and at
  Legendary a title whose questline you have finished (Harbinger, Archmage, Listener, Nightingale, Dragonborn).
- **Quest rewards**: the main quest's and the Miraak line's quests pay a perk point each, once per character.
- **The class tree**: Warrior to Knight or Barbarian, Mage to Sorcerer or Healer, Thief to Assassin or Archer; then
  Crusader or Spellsword (Knight), Rogue or Scout (Barbarian), Battlemage or Nightblade (Sorcerer), Witchhunter or Monk
  (Healer), Agent or Nightblade (Assassin), Scout or Witchhunter (Archer). Every class has its own ability, kept for
  good as the class evolves; the Archer's Archery stays with the Scout or Witchhunter it becomes.
- **Rank perks**: at Expert, Master and Legendary each Adept class offers two perks (54 in all). At Legendary a title
  you have earned takes the place of the last pair - Harbinger, Archmage, Listener, Nightingale, Dragonborn or Hero of
  Old, each with its own ability; every title you earn applies.
- **The skills menu** shows your class line, and when your class can evolve it offers the two choices (or Later).
- **NjordlingerCombatGameplayOverhaul.esp** (light): the group and class globals, class keywords, and the abilities,
  title abilities and rank perks as perks.
- Eleven languages for every string of the pages, notices and perks shown on the pages.

- **Every class, title and rank effect works.** The ones a perk cannot carry are done by the SKSE plugin on the game's
  hit, kill, spell, potion, sleep and menu events: silencing bashes, Last Bulwark (60 s cooldown, kept in the save),
  Spellsword's 3-second windows, spellstrikes, knock-downs, piercing arrows, Sense Life pulses, Vanish and Ghost Step,
  Silver Chill, purifying potions that last longer, the Monk's flurry and Mana Strike, Soothing Venom, Final Dose,
  pardoned small bounties, Silver Tongue in conversations, the Harbinger's followers and beast-blood rest, the
  Nightingale's unbreakable lockpicks. "Undetected" now means no nearby actor detects you while you sneak; "in shadow"
  is a light level under 50.

### Fixed after combined run 78d7
- The level-up screen now closes by itself: the game's own "Increase Health?" box that follows the automatic pick is
  answered for you (it appeared with the vanilla and the Norden UI level-up screen alike).
- A refused crafting station no longer leaves you seated at it; the refusal notice is in the log.
- Past its tier's mark the class line reads "... class level 18 - ready to evolve" instead of "18 of 10".

### Close readings (owner may want them tuned)
- A "critical hit" (Spellsword) is a power-attack hit; a "dodge" (Rogue) is a hit that leaves your health where it
  was; a "bash" (Crusader B1) is a power attack while blocking with a shield; a poison taking effect (Agent) is a
  harmful touch effect from you while your weapon is poisoned; Long Legs lowers the game's sprint drain setting, which
  every runner shares. The full list is generated/plugin-forms.json ("approximations" and "dll").

### Added (phase 1 - data layer)
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
