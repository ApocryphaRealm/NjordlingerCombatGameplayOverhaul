#pragma once

// Njordlinger Combat Gameplay Overhaul - the Enchanting setting (PLAN.md D28; ENCHANTING-OFF.md, the owner 19:59).
//
// On (the default, [Enchanting] bEnchanting=1): enchanting is class-gated as before (D4). Off - the owner's "if
// enchanting were gone" world, kept per save in the co-save (NJEN):
//   - the crafting gate refuses the arcane enchanter (and with it disenchanting) for every class;
//   - the Enchanting tree is locked for all: its perks and entry points read this module's own global (created at run
//     time; Lockouts conditions the tree on it instead of the magic group's), which the Classes module sets to
//     "magic group open AND Enchanting on";
//   - the Enchanting skill is capped at its value when Off took hold (the starting value on a new game, the current
//     value when switched mid-save - no experience is taken away);
//   - soul stones drop from the fallen (SoulStones' death roll).
// Switching back to On lifts all of it; nothing is removed from the save either way.

#include <cstdint>
#include <string>

namespace Enchanting
{
	inline constexpr std::uint32_t kRecord = 'NJEN';

	void OnGameLoaded();
	void OnSave(SKSE::SerializationInterface* a_intfc);
	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void OnRevert();

	bool IsOn();
	// The page and DevBench: switch this save's setting (re-applies the class on the main thread).
	void RequestSet(bool a_on);

	// The Enchanting tree's gate global (created on first call). Lockouts conditions the tree's perks on it.
	RE::TESGlobal* TreeGlobal();

	// Main thread (Classes::Apply): the cap the skill holds at while Off (-1 = none); fixes it at a_current the first time.
	float CapWhileOff(float a_current);

	std::string StatusJson();
}
