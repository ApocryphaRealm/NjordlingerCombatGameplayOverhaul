#pragma once

// Njordlinger Combat Gameplay Overhaul - the starting class chosen inside the race menu (PLAN.md D1; phase 2 step 3).
//
// The approach Apprentice - A Class Overhaul takes, as read live from RaceMenu's lists (past-mistakes library,
// 2026-10-04; RaceMenu's own ActionScript read with FFDec for the member names): no SWF is shipped. When the RaceSex
// Menu has filled its lists, Scaleform calls against RaceMenu's panel object
// (_root.RaceSexMenuBaseInstance.RaceSexPanelsInstance):
//   * a "CLASS" category is pushed onto racePanel.slidingCategoryList.categoryList.entryList with its own filter flag
//     (2^29, which the ALL category's flag does not include);
//   * one entry per starting class is pushed onto racePanel.itemList.entryList, filed under that flag, type 1 (drawn
//     like a race row), isClass = true, the class id in callbackName;
//   * the panel's onItemPress and onSelectionChange are replaced by functions of ours that serve a class entry
//     (record the pick, show its description) and hand every other entry to the panel's own handler, kept as
//     njOrigItemPress / njOrigSelectionChange and called by name on the panel (never through a function object -
//     past-mistakes library 5273).
// The pick is committed when the menu closes (the race - and so the starting skills - can still change while it is
// open). Every call runs as a UI task (gate rule scaleform-calls-only-on-the-ui-thread); every member is asked for
// before it is used and what was missing is logged once (rule 30). When anything is missing the menu is left alone
// and the class is chosen on the Classes tab instead.

#include <string>

namespace RaceMenuClass
{
	// kDataLoaded: registers the menu sink.
	void Install();

	std::string StatusJson();
}
