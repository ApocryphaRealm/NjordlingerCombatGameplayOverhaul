#pragma once

// Njordlinger Combat Gameplay Overhaul - the Shouting toggle and souls to perks (PLAN.md D26; SHOUTING-OFF.md, Fable
// 2026-10-26 19:30, the owner 19:12).
//
// With Shouting OFF (the default of a new save; [Shouting] bShouting in the INI) the player's shout key is refused at the
// input handler (ShoutHandler::CanProcess) before the Voice fires - nothing is removed: known Shouts, learned and unlocked
// words stay, and switching it back on restores the Voice at once. NPCs and dragons are untouched; a POWER on the shout
// key still works (only a Shout is refused). Exemptions, in order: a Shout the soul stones grant (D21's hidden clones);
// a Shout a quest needs while that quest is running below its completion stage (data/.../shouting.json, generated from
// the masters by tools/PluginBuild - The Way of the Voice, The Throat of the World, The Fallen, Alduin's Bane,
// Dragonslayer, At the Summit of Apocrypha).
// While Shouting is off, every dragon soul the player holds becomes perk points (checked every second), at Souls to
// Perks' rate (uSoulsPerPoint in its INI, else 1) - except while a "reserve" quest is unfinished (The Way of the Voice:
// Dragon Rising's soul unlocks Fus). Converted souls stay converted. The per-save state lives in the co-save (NJSH).
//
// D27 (SHOUTING-OFF.md 6, the owner 19:39): the setting has three states - Auto (the default), On, Off. Auto allows
// shouting while the current class holds the magic group; the class module publishes that one bool here
// (SetHoldsMagic) every time it applies the class - on load, at the start choice and at every evolution - so a Warrior
// who evolves into a Spellsword can shout from the next frame. Souls convert only while shouting is refused.

#include <cstdint>
#include <string>

namespace Shouting
{
	inline constexpr std::uint32_t kRecord = 'NJSH';

	void Install();        // kDataLoaded: the table, the input hook, the pacer
	void OnGameLoaded();   // kPostLoadGame / kNewGame
	void OnSave(SKSE::SerializationInterface* a_intfc);
	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void OnRevert();

	enum Mode : int { kAuto = 0, kOn = 1, kOff = 2 };

	// The class module: does the current class hold the magic group (true while no class is chosen)?
	void SetHoldsMagic(bool a_holds);

	struct State
	{
		bool loaded = false;           // the hook is in and the table was read
		std::string error;
		int mode = kAuto;              // this save's setting
		bool holdsMagic = true;        // as the class module last published it
		bool enabled = false;          // resolved: this save can shout now
		std::uint32_t soulsConverted = 0;
		std::uint32_t pointsGiven = 0;
		int soulsPerPoint = 1;
		bool reserved = false;         // a reserve quest is unfinished: souls wait
		std::uint32_t refusals = 0;    // shout presses refused this session
		std::uint32_t rows = 0;
	};
	State GetState();

	// The page and DevBench: switch this save's Shouting (applied on the main thread).
	void RequestSetMode(int a_mode);
	std::string StatusJson();
}
