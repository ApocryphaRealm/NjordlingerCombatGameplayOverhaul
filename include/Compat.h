#pragma once

// Njordlinger Combat Gameplay Overhaul - detecting the mods this one must not run beside (pattern from Character
// Progression Control's Compat). Detection asks the running process (GetModuleHandle, the game's own plugin list);
// a detected conflict is never a load failure - it is logged and said on the Progression tab.

#include <string>
#include <vector>

namespace Compat
{
	struct Detection
	{
		std::string name;
		bool present = false;
		std::string consequence;
	};

	// Runs the checks once. Call at kDataLoaded, before the patches install.
	void Detect();

	const std::vector<Detection>& All();

	// Character Progression Control hooks the same sites (PLAN.md: the two are never installed together).
	bool CharacterProgressionControlPresent();
	bool PluginLoaded();   // NjordlingerCombatGameplayOverhaul.esp
}
