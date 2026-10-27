#pragma once

// Njordlinger Combat Gameplay Overhaul - the skills menu's class line and evolution prompt (PLAN.md phase 2 step 6).
//
// The plan named "one SWF edit of the stats menu". It is done WITHOUT shipping a SWF: when the Stats Menu opens, a text
// field is created on its movie's root at run time (createTextField) and filled with the class line ("Adept Knight -
// class level 14 of 20"), so whatever statsmenu.swf the load order supplies keeps working and nothing of it is
// distributed. When an upgrade is due, the game's own message box offers the two options (and "Later") over the menu;
// a choice there is the same RequestChooseUpgrade the Classes tab makes. A key was planned for the prompt; the box opens
// by itself instead, so no key is issued and a controller needs nothing extra.
//
// Every Scaleform call is a UI task (gate rule scaleform-calls-only-on-the-ui-thread); the message box is opened from a
// main-thread task, as Souls to Perks' Dragonstone opens its box. Every member is asked for before use (rule 30).

#include <string>

namespace StatsMenuClass
{
	// kDataLoaded: registers the menu sink.
	void Install();

	// Testing (DevBench): picks button a_index of the open evolution box (main thread).
	void RequestPick(int a_index);

	std::string StatusJson();

	// PLAN.md D32: write SKILLS.json for the current class's ring (main thread; Classes::Apply calls it)
	void ApplyRing();
}
