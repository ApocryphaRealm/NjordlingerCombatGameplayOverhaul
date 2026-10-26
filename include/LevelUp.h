#pragma once

// Njordlinger Combat Gameplay Overhaul (copied from Character Progression Control, GPL-3.0-or-later) - stage 4: what a level up grants. Perk points first.
//
// The game adds the level-up perk point to the player's perk pool in one short sequence; the
// reference (Kassent's SE uncapper, whose Nexus permissions allow reading it) branches to its own
// routine at the start of that sequence and jumps back past it. Same here: located by its byte
// shape matched exactly once, PROVEN by the perk-pool displacement it reads and writes (checked
// against the live player object), and only then redirected. The pool is written through
// CommonLibSSE-NG's accessor, never a hard offset.

namespace LevelUp
{
	// Registers the perk-points patch group. Call at kDataLoaded, before Patches::InstallAll.
	void Register();
}
