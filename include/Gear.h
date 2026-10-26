#pragma once

// Njordlinger Combat Gameplay Overhaul - the Gear module (PLAN.md phase 3, Njordlinger 1.1; D13-D16).
//
// What the records do (NjordlingerCombatGameplayOverhaul.esp, built from config/gear-content.json):
//   - every actor carries, by SPID (NjordlingerCGO_Gear_DISTR.ini), one perk per unique armour set and one per authored
//     unique; their entry points and abilities are conditioned on the worn keyword KID placed (NjordlingerCGO_Sets_KID.ini,
//     NjordlingerCGO_Uniques_KID.ini) and on the module switch (globals NJCGO_Gear_SetsOn / NJCGO_Gear_UniquesOn);
//   - a set's 2- and 4-piece bonuses are two ability spells. No condition can COUNT the pieces of one set, so this module
//     does (D16: the DLL only where a condition cannot): on every equip / unequip and when an actor loads, it counts the
//     worn armour carrying each set keyword and adds or removes the two abilities; the set perk's entry points also
//     require the ability (HasSpell), so they follow the same count.
// What this module adds on top:
//   - the Gear tab's switches and strengths: the two globals, and the magnitudes of every gear effect and entry point
//     scaled from the values in the plugin (a strength of 1.00 = as authored); the player's gear abilities are re-applied
//     after a strength change so the new magnitudes take hold at once;
//   - the unique tier refuses enchanting (D15): KID gives every NJCGO_Tier_Unique item NJCGO_NoEnchant and the game's own
//     MagicDisallowEnchanting; at the first game load of a session this module walks every weapon and armour and adds
//     whichever of the two is missing (KID's keyword-filtered lines depend on its own ordering), and counts them.
// Threading: event sinks only record which actor changed; a small pacer thread queues the recount to the main thread
// (SKSE AddTask - no Scaleform anywhere in this module). Snapshots for the page and DevBench are mutex-guarded copies.

#include <cstdint>
#include <string>
#include <vector>

namespace Gear
{
	// kDataLoaded, after the plugin is known to be loaded: reads gear.json, resolves the forms, records the authored
	// magnitudes and registers the event sinks.
	void Install();

	// kPostLoadGame / kNewGame: the no-enchant pass (once per session), the switches and strengths, the player's count.
	void OnGameLoaded();

	// Applies the Gear settings (switches, strengths) on the main thread. Safe from any thread.
	void RequestApply();

	// Recounts the player's set pieces on the main thread. Safe from any thread.
	void RequestRecountPlayer();

	struct SetState
	{
		std::string name;
		int pieces = 0;          // the player's worn pieces of this set
		bool has2 = false;       // the 2-piece ability is on the player
		bool has4 = false;
		bool resolved = false;   // its keyword and abilities were found in the plugin
	};

	struct UniqueState
	{
		std::string id;
		std::string name;
		bool worn = false;       // the player wears or wields an item carrying its keyword
		bool resolved = false;
	};

	struct Snapshot
	{
		bool loaded = false;
		std::string error;
		std::vector<SetState> sets;
		std::vector<UniqueState> uniques;
		std::uint32_t recounts = 0;            // actor recounts this session
		std::uint32_t abilitiesAdded = 0;
		std::uint32_t abilitiesRemoved = 0;
		std::uint32_t uniqueTierItems = 0;     // weapons and armour carrying NJCGO_Tier_Unique (after the pass)
		std::uint32_t noEnchantAdded = 0;      // keywords the pass had to add (KID had not)
		std::uint32_t disallowAdded = 0;
		bool noEnchantPassDone = false;
		float appliedSetStrength = 1.0F;
		float appliedUniqueStrength = 1.0F;
	};

	Snapshot GetSnapshot();
	std::string StatusJson();
}
