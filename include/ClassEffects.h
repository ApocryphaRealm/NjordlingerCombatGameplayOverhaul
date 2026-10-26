#pragma once

// Njordlinger Combat Gameplay Overhaul - the class, title and rank effects a perk record cannot carry (CLASS-CONTENT.md
// sections 2 and 3; the perks list them under "dll" in generated/plugin-forms.json).
//
// Each effect is small and keyed to its perk: it runs only while the player holds that perk. The game's events (hit,
// death, spell cast, magic effect applied, equip, sleep stop, menu open / close) are sinks that copy what they need and
// queue the work to the main thread; a half-second tick (a worker thread that only queues tasks) keeps the time-based
// state: the 3-second windows (globals NJCGO_Win_SpellThenHit / NJCGO_Win_HitThenSpell), "undetected" (global
// NJCGO_PlayerUndetected: sneaking and no nearby actor detects you), the detect-life pulses, the followers' ability, the
// bounty pardon, the sprint setting, cooldowns. Spells are the helper records of NjordlingerCombatGameplayOverhaul.esp,
// cast with the player's instant caster. No Scaleform is touched. The state that must survive a save (the Last Bulwark
// cooldown, the Rested bonus's end, the once-per-combat Final Dose) is the co-save record NJFX.

#include <cstdint>
#include <string>

namespace ClassEffects
{
	// kDataLoaded, after Classes::Install (the class table holds the perks and the helper records).
	void Install();

	std::string StatusJson();

	inline constexpr std::uint32_t kRecord = 'NJFX';
	void OnSave(SKSE::SerializationInterface* a_intfc);
	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void OnRevert();
}
