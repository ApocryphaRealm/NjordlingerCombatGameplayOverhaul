#pragma once

// Njordlinger Combat Gameplay Overhaul - the class table (PLAN.md phase 2 step 2): the three skill groups, the class
// nodes and their lineage, the titles. Read once at kDataLoaded from
// Data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/classes.json (source: config/classes.json in the repo).

#include "SkillList.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ClassData
{
	enum Group : int { kWarrior = 0, kMagic = 1, kCovert = 2, kGroupCount = 3 };
	inline constexpr const char* kGroupId[kGroupCount] = { "warrior", "magic", "covert" };

	inline constexpr int kTierCount = 6;   // Novice, Apprentice, Adept, Expert, Master, Legendary
	inline constexpr const char* kTierName[kTierCount] = { "Novice", "Apprentice", "Adept", "Expert", "Master", "Legendary" };

	struct ClassNode
	{
		std::string id;
		std::string name;          // English; the page shows NJ_ClassName_<id> when translated
		std::string description;   // English; NJ_ClassDesc_<id>
		int tier = 0;
		bool start = false;
		bool reachable = true;
		std::array<bool, kGroupCount> groups{};
		std::array<bool, skilllist::kCount> extraSkills{};
		std::vector<std::string> next;     // the two options at the next upgrade (Novice and Apprentice)
		// Forms from NjordlingerCombatGameplayOverhaul.esp (null when the plugin or the record is missing).
		RE::BGSPerk* ability = nullptr;    // the class ability (a perk: constant effects ride its ability spell)
		RE::TESGlobal* global = nullptr;
		RE::BGSKeyword* keyword = nullptr;
		// Rank perks of an Adept class (CLASS-CONTENT.md section 3): [step 0 Expert, 1 Master, 2 Legendary][0 A, 1 B].
		struct Rank
		{
			RE::BGSPerk* perk = nullptr;
			std::string name;
			std::string description;
		};
		Rank ranks[3][2];
		bool hasRanks = false;
	};

	struct Title
	{
		std::string id;
		std::string name;
		std::string description;
		std::string quest;                        // the questline's completing quest, by editor ID
		std::vector<std::string> starts;          // fits a lineage that began with one of these (empty = any)
		std::vector<std::string> classes;         // fits these current classes (empty = any)
		std::vector<std::string> excludeClasses;  // never these
		RE::BGSPerk* perk = nullptr;

		bool Fits(const std::string& a_start, const std::string& a_class) const;
	};

	struct Table
	{
		bool loaded = false;
		std::string error;
		std::array<std::array<bool, skilllist::kCount>, kGroupCount> groupSkills{};
		std::array<int, kGroupCount> craftSkill{ -1, -1, -1 };   // skill index of each group's craft
		std::vector<ClassNode> classes;
		std::vector<Title> titles;
		RE::TESGlobal* groupGlobal[kGroupCount]{};   // from the plugin, or created at run time
	};

	// Reads the table and resolves its forms (main thread, kDataLoaded).
	void Load();

	const Table& Get();
	const ClassNode* Find(const std::string& a_id);
	const Title* FindTitle(const std::string& a_id);

	// The group a skill belongs to, -1 if none (every vanilla skill belongs to one).
	int GroupOfSkill(int a_skillIndex);
	int SkillIndex(const std::string& a_iniName);   // "OneHanded" -> 0, -1 if unknown
}
