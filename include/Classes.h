#pragma once

// Njordlinger Combat Gameplay Overhaul - the Classes module (PLAN.md D1-D3, D8; phase 2 steps 2, 4, 5).
//
// One class per character, kept in the co-save. Class experience comes from the class's own skills rising (the game's
// SkillIncrease event - present on every build line); a class level costs base + perLevel x level; after N class levels
// per tier the character is offered ONE OF TWO options and the choice is permanent:
//   Novice -> Apprentice -> Adept   a new class from the lineage (a hybrid opens its second group);
//   Adept -> Expert -> Master       one of two rank paths (the rank perks are authored later);
//   Master -> Legendary             a title whose questline is finished (Harbinger, Archmage, Listener, Nightingale,
//                                   Dragonborn).
// The class decides which skills are locked: a locked skill is capped at its starting value and banks no use
// (Skills / SkillExperience), its perks are refused and give nothing (Lockouts), and its craft's menu refuses (Lockouts).
// Locks follow the current class and never loosen, except that a hybrid opens its second group.
//
// Abilities, rank perks and titles are cumulative: what a character has been, it keeps.

#include "SkillList.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Classes
{
	// kDataLoaded: reads the class table, registers the skill-increase and menu sinks.
	void Install();

	// A save loaded or a new game started (the co-save has been read): applies locks, globals and abilities.
	void OnGameLoaded();

	// The settings changed on the page (lock toggles, enable): re-applies the locks (main thread queued).
	void RequestApply();

	// Choose the starting class (only while none is chosen). Queued to the main thread; false when refused now.
	bool RequestChooseStart(const std::string& a_classId, std::string& a_why);

	struct Option
	{
		std::string id;          // class id, "path:0" / "path:1", or title id
		std::string label;       // what the page shows
		std::string description;
		bool available = true;
		std::string why;         // why not, when unavailable
	};

	// Choose option a_index of the upgrade on offer. Queued to the main thread; false when nothing is on offer.
	bool RequestChooseUpgrade(int a_index, std::string& a_why);

	// Testing (DevBench): add class experience as if skills had risen.
	void RequestGrantXp(float a_amount);

	struct Snapshot
	{
		bool tableLoaded = false;
		std::string tableError;
		bool enabled = false;
		bool chosen = false;
		std::string classId;
		std::string className;     // translated
		std::string line;          // "Adept Knight - class level 14 of 20"
		int tier = 0;
		int level = 0;
		float xp = 0.0F;
		float xpToNext = 0.0F;
		int nextUpgradeLevel = 0;
		bool upgradeDue = false;
		std::vector<Option> options;   // when upgradeDue
		std::array<bool, skilllist::kCount> held{};
		std::array<float, skilllist::kCount> lockedCap{};
		std::array<bool, 3> groupsHeld{};
		std::vector<std::string> history;
		int rankPath[2]{ -1, -1 };
		std::string title;
		std::uint32_t increasesCounted = 0;
	};
	Snapshot GetSnapshot();
	std::string StatusJson();

	// The starting classes, for the race menu and the page: {id, translated name, translated description}.
	struct StartClass { std::string id, name, description; };
	std::vector<StartClass> StartClasses();

	// Whether the current class holds a skill (true while no class is chosen or classes are off).
	bool Holds(int a_skillIndex);
	// Whether the current class holds a group (same rule).
	bool HoldsGroup(int a_group);

	// SKSE co-save plumbing.
	inline constexpr std::uint32_t kRecord = 'NJCL';
	void OnSave(SKSE::SerializationInterface* a_intfc);
	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void OnRevert();
}
