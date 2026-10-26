#pragma once

// Njordlinger Combat Gameplay Overhaul - perk points for Dragonborn quests (PLAN.md D7; phase 2 step 7).
//
// The rows table: Data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/quest-rewards.json (generated from the main-quest
// and Miraak records by the data build; a row = a quest editor ID and its points, a default for a row with none). A
// row the player changes on the Quest Rewards tab is kept in the INI ([QuestRewardRows]). The quest-completion event
// (the game's QuestStatus event, as CPC's experience sources use it) pays a row's points once per save: the paid
// quests are kept in the co-save.

#include <cstdint>
#include <string>
#include <vector>

namespace QuestRewards
{
	// kDataLoaded: reads the rows, registers the quest sink.
	void Install();

	struct Row
	{
		std::string quest;       // editor ID
		std::string name;        // the quest's name as the game has it (or the JSON's)
		int shipped = 0;         // the points in quest-rewards.json
		int points = 0;          // what it pays now (the INI's value when the player changed it)
		bool inLoadOrder = false;
		bool paid = false;       // this save
	};
	std::vector<Row> Rows();
	int DefaultPoints();

	// Sets a row's points (the page). Stored in settings::questrewards::overrides; Save writes it.
	void SetPoints(const std::string& a_quest, int a_points);

	// Testing (DevBench): pays a row as if its quest had just completed (main thread).
	void RequestSimulate(const std::string& a_quest);

	std::string StatusJson();

	inline constexpr std::uint32_t kRecord = 'NJQR';
	void OnSave(SKSE::SerializationInterface* a_intfc);
	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void OnRevert();
}
