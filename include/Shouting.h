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
// With this save's auto-convert on, every dragon soul the player holds becomes perk points (checked every second), at
// Souls to Perks' rate (uSoulsPerPoint in its INI, else 1) - except while a "reserve" quest is unfinished (The Way of the
// Voice: Dragon Rising's soul unlocks Fus). Converted souls stay converted. The per-save state lives in the co-save (NJSH).
//
// DESIGN-RETHINK.md 14 (the owner 20:56, 20:58): Shouting is a plain On / Off, Off by default (D27's Auto is withdrawn;
// a save that held Auto or Off reads as Off, On stays On). Dragon souls stay souls - the player spends them through
// Souls to Perks - unless this save's "Auto-convert dragon souls" is on (default off, independent of Shouting): then
// they become perk points as they arrive, at Souls to Perks' rate, with the Dragon Rising reserve. While Souls to Perks'
// own bAutoConvert is on it is the converter and this one stands aside (never two). Points already given are kept.

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

	struct State
	{
		bool loaded = false;           // the hook is in and the table was read
		std::string error;
		bool enabled = false;          // this save can shout
		bool autoConvert = false;      // this save converts dragon souls to perk points as they arrive
		bool stpConverts = false;      // Souls to Perks' own bAutoConvert is on: it converts, this module stands aside
		std::uint32_t soulsConverted = 0;
		std::uint32_t pointsGiven = 0;
		int soulsPerPoint = 1;
		bool reserved = false;         // a reserve quest is unfinished: souls wait
		std::uint32_t refusals = 0;    // shout presses refused this session
		std::uint32_t rows = 0;
	};
	State GetState();

	// The page and DevBench: switch this save's Shouting (applied on the main thread).
	void RequestSetEnabled(bool a_on);
	void RequestSetAutoConvert(bool a_on);
	std::string StatusJson();
}
