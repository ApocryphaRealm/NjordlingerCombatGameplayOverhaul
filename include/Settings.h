#pragma once

// Njordlinger Combat Gameplay Overhaul - settings. Plain-file INI (never the Win32 profile API: under Mod Organizer 2
// it reports success and writes nothing). The reader, the writer and the progression keys are copied from Character
// Progression Control (ApocryphaRealm, GPL-3.0-or-later) and trimmed to what PLAN.md phase 2 step 1 names: the perk
// pool, the level-up gains, the skill cap and the skill-experience rates. Skill points, static levelling, the level
// cost and the difficulty tables are not part of this mod.
//
// Every compiled default below is the value in the shipped INI (rule 16) - change both together.

#include "SkillList.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace settings
{
	namespace debug
	{
		inline std::uint32_t logLevel = 2;  // uLogLevel:Debug - 2 = info (rule 14)
	}

	// Progression - the skill cap. Off = every skill stops at 100 as in vanilla; a class's locked skills are capped
	// by the Classes module whether this is on or off.
	namespace skills
	{
		inline bool overrideCaps = false;                  // bOverrideSkillCaps:Skills
		inline float cap[skilllist::kCount]{};             // fCap<Skill>:Skills - 100 = vanilla
	}

	// Progression - what one use of a skill pays toward that skill, and what a skill increase pays toward a level.
	namespace skillexp
	{
		inline bool overrideRates = false;                 // bOverrideSkillExp:SkillExperience
		inline float mult[skilllist::kCount]{};            // fMult<Skill>:SkillExperience - 1.0 = vanilla
		inline float toLevelMult = 1.0F;                   // fSkillToLevelMult:SkillExperience
	}

	// Progression - what a level up grants. ON by default: PLAN.md D6 has the attributes grow from what the character
	// does (Adaptive Attributes SKSE), so a level up grants no health, magicka or stamina; D5 keeps one perk point.
	// Carry weight is not touched here (one carry-weight writer, D6).
	namespace levelup
	{
		inline bool overrideRewards = true;                // bOverrideLevelUpRewards:LevelUp
		struct PerkRow { std::uint16_t fromLevel; std::uint8_t perks; };
		inline std::vector<PerkRow> perksByLevel{ { 1, 1 } };   // sPerksByLevel:LevelUp - "1:1,20:2"
		int PerksAtLevel(std::uint16_t a_level);
		std::string PerksTableText();
		bool ParsePerksTable(const std::string& a_text);
		inline float healthPerLevel = 0.0F;                // fHealthPerLevel:LevelUp
		inline float magickaPerLevel = 0.0F;               // fMagickaPerLevel:LevelUp
		inline float staminaPerLevel = 0.0F;               // fStaminaPerLevel:LevelUp
	}

	// Classes (PLAN.md D1-D4, D8).
	namespace classes
	{
		inline bool enabled = true;                        // bEnableClasses:Classes
		inline int levelsPerTier = 10;                     // iClassLevelsPerTier:Classes - Q41 default 10
		inline float xpPerIncrease = 1.0F;                 // fClassXpPerSkillIncrease:Classes
		inline float xpBase = 2.0F;                        // fClassXpBase:Classes - class level n -> n+1 costs base + perLevel x n
		inline float xpPerLevel = 0.2F;                    // fClassXpPerLevel:Classes - the curve's steepness
		inline bool lockPerks = true;                      // bLockPerks:Classes - D3, the perk conditions
		inline bool lockSkills = true;                     // bLockSkills:Classes - D3, a locked skill stays at its starting value
		inline bool craftingGate = true;                   // bCraftingGate:Classes - D4
		inline bool raceMenuClass = true;
		inline bool hideLockedTrees = true;
		inline bool routes = true;                         // bRoutesByClass:Classes - guild routes need the class's group (D30)                // bHideLockedTrees:Classes - the skills menu's ring hides the trees the class locks (phase 5 step 2)                  // bRaceMenuClass:Classes - D1, the Class category in the race menu
	}

	// Quest rewards (PLAN.md D7). The rows' shipped values are quest-rewards.json; a row the player changes is kept
	// here, by quest editor ID.
	namespace questrewards
	{
		inline bool enabled = true;                        // bEnableQuestRewards:QuestRewards
		inline std::map<std::string, int> overrides;       // [QuestRewardRows] <EditorID>=<points>
	}

	// Gear (PLAN.md phase 3, D14 / D15): the module switches and strengths. A strength scales every gear effect and
	// entry point from the value in the plugin (1.00 = as authored).
	namespace gear
	{
		inline bool sets = true;                           // bSetBonuses:Gear
		inline bool uniques = true;                        // bUniqueEffects:Gear
		inline float setStrength = 1.0F;                   // fSetStrength:Gear - 0.25 to 2.00
		inline float uniqueStrength = 1.0F;                // fUniqueStrength:Gear - 0.25 to 2.00
	}

	// Shouting (PLAN.md D26, D27): a NEW save's Shouting setting (each save keeps its own choice in the co-save).
	namespace shouting
	{
		inline bool enabledDefault = true;                 // bShouting:Shouting - the owner 22:53: on by default; off refuses the shout key (stone and quest Shouts still work)
		inline bool autoConvertDefault = false;            // bAutoConvertSouls:Shouting - dragon souls become perk points as they arrive
	}

	// Carry weight (the owner 2026-10-26 19:53/19:54): one owned modifier - see CarryWeight.h.
	namespace carry
	{
		inline bool enabled = true;                        // bControlCarryWeight:CarryWeight
		inline float base = 300.0F;                        // fStartingCarryWeight:CarryWeight - 50 to 1000
		inline float perStamina = 2.5F;                    // fCarryWeightPerStamina:CarryWeight - 0 to 10, per Adaptive Attributes Stamina point
	}

	// Soul Stones (PLAN.md phase 4, D17-D22): the module switch and the display choices.
	namespace stones
	{
		inline bool enabled = true;                        // bEnableSoulStones:SoulStones - stone effects, phrases, granted Shouts
		inline bool nameSlots = true;                      // bNameSlots:SoulStones - the socketed words after the item's name
		inline bool nameDragonLetters = false;             // bNameDragonLetters:SoulStones - the words in dragon letters (font-tag test)
		inline bool weaponGlow = true;                     // bWeaponGlow:SoulStones - a drawn weapon glows in its best stone's tier colour
	}

	void Init(const std::string& a_iniFileName);
	bool Reload();
	bool Save();
	void RestoreDefaults();
	void ApplyLogLevel();
	const std::string& GetIniPath();
}
