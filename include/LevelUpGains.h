#pragma once

// Njordlinger Combat Gameplay Overhaul - the health, magicka and stamina a level up grants.
//
// Copied from Character Progression Control's attribute-gains hook (ApocryphaRealm, GPL-3.0-or-later) and trimmed to
// the gain alone: the hook sits on the entry of the game's attribute-level-up routine, sets the game setting
// iAVDhmsLevelUp to this mod's value for the chosen attribute for the length of the call, and puts the install's own
// value back afterwards. PLAN.md D6 ships the gains at 0 (attributes grow from what the character does, through
// Adaptive Attributes SKSE). Carry weight from a level up is held at 0 - Adaptive Attributes is the one carry-weight writer (D6, Q51) - and CPC's starting values,
// choice counting and co-save are not part of this mod.
//
// PLAN.md phase 2 step 8b: with all three gains at 0 the LevelUp Menu's attribute pick is made for the player the
// moment the menu opens, so a level up simply grants its perk points.

#include <cstdint>

namespace LevelUpGains
{
	// Registers the patch group. Call at kDataLoaded, before Patches::InstallAll.
	void Register();

	struct State
	{
		bool attached = false;
		std::uint32_t levelUps = 0;      // attribute choices seen this session
		int lastChoice = -1;             // 0 health, 1 magicka, 2 stamina
		int lastGain = 0;                // what the last choice granted
		bool skipResolved = false;       // the choose-attribute function was found (step 8b)
		std::uint32_t picksSkipped = 0;  // attribute picks made for the player this session
	};
	State GetState();
}
