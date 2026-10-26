#pragma once

// Njordlinger Combat Gameplay Overhaul (copied from Character Progression Control, GPL-3.0-or-later) - stage 3: what a use of a skill pays toward that skill, and what
// a skill increase pays toward a character level.
//
// Both hooks live on the game's skill-improve function, located the way the cap patch is: by a
// byte shape matched exactly once and then PROVEN before anything is written. The reference is
// the Skyrim Skill Uncapper lineage (Kassent's SE source, whose Nexus permissions allow reading
// it): a branch at the function's entry into a hook that scales the experience and calls an
// "original" thunk re-executing the overwritten prologue; and the call to the standalone
// level-experience function (inlined on AE, a real call on SE 1.5.97) redirected to a wrapper
// that scales its result. No code is copied - our own hooks, settings and location checks.

namespace SkillExperience
{
	// Registers the two patch groups. Call at kDataLoaded, before Patches::InstallAll.
	void Register();

	// True once the skill-improve entry hook is attached (the rates and the class lock ride it).
	bool ImproveHookAttached();
}
