#pragma once

// Character Progression Control - the eighteen skills, in the game's own order.
//
// The order is PlayerSkills::Data::Skills::Skill, so an index here indexes the game's own
// skills[] array directly. Kept in its own dependency-free header because the settings, the
// pages and the DevBench tool all need the same list and none of them should own it.

namespace skilllist
{
	inline constexpr int kCount = 18;

	// Used to build INI keys, so: one word, no spaces, never changed once released.
	inline constexpr const char* kIniName[kCount] = {
		"OneHanded", "TwoHanded", "Archery", "Block", "Smithing", "HeavyArmor",
		"LightArmor", "Pickpocket", "Lockpicking", "Sneak", "Alchemy", "Speech",
		"Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Enchanting"
	};

	// What the player sees, spelled the way the game spells it.
	inline constexpr const char* kDisplayName[kCount] = {
		"One-handed", "Two-handed", "Archery", "Block", "Smithing", "Heavy Armor",
		"Light Armor", "Pickpocket", "Lockpicking", "Sneak", "Alchemy", "Speech",
		"Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Enchanting"
	};
}
