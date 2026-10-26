#pragma once

// Njordlinger Combat Gameplay Overhaul - the class lock-outs that are not the skill cap (PLAN.md D3, D4; phase 2 step 4).
//
// The perk pass (once, at kDataLoaded): every perk in the eighteen skill trees gets one condition group put in front
// of its own conditions and in front of the Perk Owner conditions of each of its entry points:
//     GetIsReference(Player) == 0  OR  GetGlobalValue(<the tree's group global>) == 1
// so an NPC is never affected, and the player can neither take the perk nor get its entry points while the class does
// not hold that group. The group globals are set by the Classes module (1 = open). Ability entries (a perk that adds a
// spell) are not conditioned this way - the spell stays once added; that is listed as a known limit.
//
// The crafting gate: the forge, grindstone and armour workbench (Smithing), the enchanting table (Enchanting) and the
// alchemy lab (Alchemy) close again with a notice when the class does not hold that craft's group. Smelters, tanning
// racks and cooking pots stay open to everyone.

#include <cstdint>
#include <string>

namespace Lockouts
{
	// kDataLoaded, after Classes::Install (the class table holds the group globals).
	void Install();

	struct State
	{
		std::uint32_t perksConditioned = 0;
		std::uint32_t entriesConditioned = 0;
		std::uint32_t abilityEntriesSkipped = 0;
		std::uint32_t craftingRefusals = 0;
		std::string lastRefusal;
	};
	State GetState();
}
