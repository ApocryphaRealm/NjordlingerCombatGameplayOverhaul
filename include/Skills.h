#pragma once

// Njordlinger Combat Gameplay Overhaul - the progression module's skill cap.
//
// Copied from Character Progression Control's Skills tab (ApocryphaRealm, GPL-3.0-or-later): the game's skill-advance
// routine loads the constant 100.0 at one site and compares the skill against it; the site is found by its byte shape
// (matched exactly once), proven by the register it loads and the 100.0 constant, and only then redirected to
// NJ_GetSkillCap through a trampoline call. Trimmed: the formula cap is not part of this mod.
//
// Added for the Classes module (PLAN.md D3): a skill the character's class does not hold is capped at its starting
// value - SetLockedCap() hands the hook that number, and the lower of it and the configured cap wins. The patch is
// attached whenever its site verifies (no-restart-gated-installers); every setting is read live inside the hook.

#include "SkillList.h"

#include <cstdint>

namespace Skills
{
	// Registers the cap patch group. Call at kDataLoaded, before Patches::InstallAll.
	void Register();

	// The verified skill-cap site (0 until located) - the skill-experience hooks use it to prove they found the same function.
	std::uintptr_t CapSiteAddress();

	// A locked skill's cap (its starting value); a negative value clears the lock. Safe from any thread.
	void SetLockedCap(int a_skillIndex, float a_cap);
	float LockedCap(int a_skillIndex);   // negative = not locked

	// What the cap site will answer for this skill right now.
	float EffectiveCap(int a_skillIndex);

	struct SkillState
	{
		float level = 0.0F;
		float xp = 0.0F;
		float levelThreshold = 0.0F;
	};

	struct State
	{
		bool readable = false;
		std::uint16_t characterLevel = 0;
		float characterXp = 0.0F;
		float characterThreshold = 0.0F;
		SkillState skill[skilllist::kCount];
	};
	// Reads the player's skills (main thread, or a caller that tolerates a racing read of plain floats).
	State GetState();
}
