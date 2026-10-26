#pragma once

// Character Progression Control - the patch register.
//
// From the plan's technical shape: "Each patch group installs independently and reports in the
// log whether it installed, so a failed hook degrades to vanilla behaviour rather than to a
// crash." This is that mechanism, and it exists BEFORE the first hook does, on purpose - a patch
// that cannot be installed has to be a reported fact rather than a surprise.
//
// A group that does not install is not an error. It means that part of the mod is off and the
// game behaves exactly as it would without us, and the Patches tab says so in plain words.

#include <functional>
#include <string>
#include <vector>

namespace Patches
{
	struct Group
	{
		std::string name;         // what the player sees
		std::string touches;      // a plain statement of what it changes, for the Patches tab
		bool installed = false;
		std::string status;       // why, when it did not install
		// 1.2.0: the installer REFUSED it (its reason starts "refused") - the game's code did not look as expected, so the
		// feature behind it is off however it is set. Distinct from a group that is simply not wanted.
		bool refused = false;
		// 1.1.10 (test 71d9): installed with Custom Skills Framework's code around the comparison - kept as its own flag,
		// because status says only "installed" once a group is in and the installer's reason is not kept
		bool alongsideCsf = false;
	};

	// Installer returns true when the patch is in place; on false it sets a_reason to a plain
	// sentence the player can read.
	using Installer = std::function<bool(std::string& a_reason)>;
	// Whether the player has the feature behind a group switched on (1.2.0). Empty means always - the group is part of
	// what the mod does whatever is set.
	using Wanted = std::function<bool()>;

	void Register(std::string a_name, std::string a_touches, Installer a_installer, Wanted a_wanted = {});

	// The groups that were refused while the player has their feature on - what the in-game notice names (1.2.0: three
	// legendary groups were refused on the owner's game and the only sign was a log line).
	std::vector<std::string> RefusedAndWanted();

	// Runs every registered installer once and logs the outcome of each. Call at kDataLoaded.
	void InstallAll();

	const std::vector<Group>& All();

	// True when a named group installed - the guard a feature checks before assuming it works.
	bool IsInstalled(const std::string& a_name);
}
