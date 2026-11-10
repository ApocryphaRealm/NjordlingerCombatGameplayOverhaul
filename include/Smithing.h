#pragma once

// Njordlinger Combat Gameplay Overhaul - smithing: the tempering mode, sharpening and edge wear (the owner, 2026-11-10;
// 4. plans\njcgo-tempering-mode-20261110\BRIEF.md with its REVISION 2).
//
// [Smithing] iTemperingMode
//   0 normal tempering - the game's own.
//   1 first level only - the improve result the smithing menu computes for every listed item (grindstone and armour
//     workbench alike) is held at the first quality, Fine. The cap sits at the source: the menu's list builder
//     (1.5.97 id 50296, 1.6/1.7 id 51360) asks the Mod Tempering Health perk entry point for the improvement above
//     fHealthDataValue1, adds it back and rounds to 0.1; this module wraps that one entry-point call and clamps the
//     improvement, so the menu shows and applies Fine. An item already Fine or better then has nothing to gain and the
//     menu refuses it; a notice says why when the menu opens.
//   2 sharpening - activating a grindstone (a workbench of type Smithing Weapon) no longer seats the player: it sets the
//     melee weapon in the right hand to Fine (fHealthDataValue1, read from the game) and plays the improve sound. While
//     that weapon's tempering is above normal (health > 1.0) the player carries the "Sharpened" ability (the plugin's
//     NJCGO_Sharpened: AttackDamageMult + fSharpenDamageBonus; HitData's damage step multiplies a hit's physical damage
//     by the aggressor's AttackDamageMult). The armour workbench keeps normal tempering.
// [Smithing] bEdgeWear, [Wear] iHits<Material>
//   Every hit the player LANDS with an equipped melee weapon (TESHitEvent, the player as the cause, no projectile, not a
//   bash) lowers that weapon's tempering by one quality step (fHealthDataValue2 - fHealthDataValue1) divided by its
//   material's hits-per-level row. It stops at normal (1.0). In every mode while the toggle is on. The weapon's own
//   ExtraHealth is the state - the game saves it; nothing goes to the co-save.

#include <cstdint>
#include <string>

namespace Smithing
{
	void Install();          // kDataLoaded: the hooks, the sinks, the keyword table, the Sharpened spell
	void OnGameLoaded();     // kPostLoadGame / kNewGame: re-apply the Sharpened ability from the weapon in hand
	void RequestApply();     // a setting changed (page, reload, DevBench): re-check the ability on the main thread

	// For the DevBench tool (op=smithing): queue a grindstone sharpen as if the player activated one, or N landed hits
	// with the right-hand weapon.
	void RequestSharpen();
	void RequestWearHits(int a_hits);

	struct Snapshot
	{
		std::string weapon;          // the right-hand weapon's name, empty for none
		bool melee = false;
		float health = 1.0F;         // its tempering (ItemHealth)
		int material = -1;           // row of settings::smithing::hits, -1 none
		bool sharpenedActive = false;
		float fine = 1.1F;           // fHealthDataValue1
		float step = 0.1F;           // one quality level of health
		bool temperHook = false;     // mode 1's cap attached
		bool furnitureHook = false;  // mode 2's grindstone attached
		std::uint32_t sharpens = 0, hitsWorn = 0, edgesLost = 0, capped = 0;
	};
	Snapshot GetSnapshot();          // values updated on the main thread; safe from any thread
	std::string StatusJson();
}
