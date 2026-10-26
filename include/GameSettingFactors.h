#pragma once

// Njordlinger Combat Gameplay Overhaul - one writer for the game settings several effects scale (main thread only).
//
// The Scout's Long Legs (ClassEffects) and Savior's Hide (Gear) both lower fSprintStaminaDrainMult; Shield of Ysgramor
// lowers fStaminaBlockDmgMult. Each owner states its own factor; the setting is always the game's value times the
// product of the factors, so two owners never undo each other and switching every owner off restores the game's value
// exactly. The game's value is read the first time a setting is touched.

#include "utils/Logger.h"

#include <map>
#include <string>

namespace GameSettingFactors
{
	namespace detail
	{
		struct Entry
		{
			float vanilla = 0.0F;
			std::map<std::string, float> factors;
		};
		inline std::map<std::string, Entry> g_entries;
	}

	// Sets a_owner's factor on a_setting (1.0 = no change) and writes the setting. Returns false when the game has no
	// such setting (logged once).
	inline bool Set(const char* a_setting, const char* a_owner, float a_factor)
	{
		auto* gs = RE::GameSettingCollection::GetSingleton();
		auto* setting = gs ? gs->GetSetting(a_setting) : nullptr;
		if (!setting)
		{
			static std::map<std::string, bool> warned;
			if (!warned[a_setting]) { logger::warn("game setting {} not found - {} cannot scale it", a_setting, a_owner); warned[a_setting] = true; }
			return false;
		}
		auto [it, fresh] = detail::g_entries.try_emplace(a_setting);
		auto& e = it->second;
		if (fresh) { e.vanilla = setting->data.f; }
		const auto old = e.factors.count(a_owner) ? e.factors[a_owner] : 1.0F;
		if (old == a_factor) { return true; }
		e.factors[a_owner] = a_factor;
		float product = 1.0F;
		for (const auto& [owner, f] : e.factors) { product *= f; }
		setting->data.f = e.vanilla * product;
		logger::info("{}: {} factor {:.2f} -> {:.3f} (game value {:.3f})", a_setting, a_owner, a_factor, setting->data.f, e.vanilla);
		return true;
	}
}
