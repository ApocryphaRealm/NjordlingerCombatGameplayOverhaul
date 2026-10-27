#pragma once

// Njordlinger Combat Gameplay Overhaul - carry weight (the owner, 2026-10-26 19:53 and 19:54).
//
// One writer for the player's carry weight: this module owns ONE permanent modifier on CarryWeight, worth
//     (starting carry weight - the race's base carry weight) + (carry weight per stamina point x Adaptive Attributes'
//     permanent Stamina points)
// and moves it by the difference whenever a slider or the Stamina bonus changes. What it has applied is kept in the
// co-save (NJCW), so a load never adds it twice; other mods' temporary effects (potions, the Steed Stone, enchantments)
// stack on top as before, and vanilla's +5 per Stamina level-up choice stays in the base value.
// Adaptive Attributes' points are read at run time through its own Papyrus function (AdaptiveAttributesSKSE.
// GetBonusStamina). When its own carry-weight option (bEnableCarryWeightFromStaminaBonus) is on, it is already the writer
// of that part: this module then leaves the per-stamina part alone and says so on the page (two writers never).

#include <cstdint>
#include <string>

namespace CarryWeight
{
	inline constexpr std::uint32_t kRecord = 'NJCW';

	void Install();        // kDataLoaded: reads Adaptive Attributes' INI, starts the pacer
	void OnGameLoaded();
	void OnSave(SKSE::SerializationInterface* a_intfc);
	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void OnRevert();

	struct State
	{
		bool ready = false;          // a game is loaded
		bool aaFound = false;        // Adaptive Attributes answered
		bool aaWritesCarry = false;  // its own carry-weight option is on: the per-stamina part is its, not ours
		int aaStamina = 0;           // its permanent Stamina points
		float raceBase = 300.0F;
		float applied = 0.0F;        // our modifier now
		float carryWeight = 0.0F;    // the player's carry weight now
	};
	State GetState();
	std::string StatusJson();
}
