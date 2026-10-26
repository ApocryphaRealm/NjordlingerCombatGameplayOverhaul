#pragma once

// Njordlinger Combat Gameplay Overhaul - the settings page inside the Apocrypha Menu Framework (PLAN.md phase 2 step 8):
// one section, a tab per module - Progression, Classes, Quest Rewards, Gear (phase 3), Soul Stones (phase 4). The patterns (export probe, precise sliders,
// toggles, Save / Reload / Restore, TR() strings) are Character Progression Control's and Souls to Perks'.

namespace UI
{
	// Call at kDataLoaded.
	void Register();

	namespace Pages
	{
		void __stdcall Progression();
		void __stdcall Classes();
		void __stdcall QuestRewards();
		void __stdcall Gear();
		void __stdcall SoulStones();
	}

	// For the DevBench tool: the page's last status line.
	const char* LastStatus();
}
