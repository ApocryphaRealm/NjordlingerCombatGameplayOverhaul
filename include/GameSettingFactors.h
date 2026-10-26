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
#include <vector>

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

	// Sprint stamina cost. In vanilla it is the game setting fSprintStaminaDrainMult; Paragon - A Modern Perk Overhaul
	// sets that setting to 0 and drains sprint stamina with its own spell instead (ParagonPerks.esp
	// APO_VALOR_SprintStaminaDrainSpell 000DB6 and the horse one 000172, cast while sprinting - found by the 99fc retest,
	// 2026-10-26: the setting read 0.0 there). So a sprint-cost owner (Long Legs, Savior's Hide) scales both: the setting,
	// and the magnitude of Paragon's drain spells when that plugin is loaded (each new cast takes the scaled value).
	namespace detail
	{
		struct SprintSpell { RE::Effect* effect; float base; };
		inline std::vector<SprintSpell> g_paragonDrain;
		inline bool g_paragonLooked = false;
		inline std::map<std::string, float> g_sprintFactors;

		inline void FindParagonDrain()
		{
			if (g_paragonLooked) { return; }
			g_paragonLooked = true;
			auto* handler = RE::TESDataHandler::GetSingleton();
			if (!handler || !handler->LookupModByName("ParagonPerks.esp")) { logger::info("sprint cost: ParagonPerks.esp not loaded - the game setting alone carries sprint cost"); return; }
			for (const RE::FormID local : { 0x000DB6u, 0x000172u })
			{
				auto* spell = handler->LookupForm<RE::SpellItem>(local, "ParagonPerks.esp");
				if (!spell) { logger::warn("sprint cost: ParagonPerks.esp has no spell {:06X} - its sprint drain is not scaled", local); continue; }
				for (auto* e : spell->effects)
				{
					if (e) { g_paragonDrain.push_back({ e, e->effectItem.magnitude }); }
				}
				logger::info("sprint cost: Paragon's sprint drain {:08X} ({}) - {} effect(s) will be scaled", spell->GetFormID(), spell->GetName(), spell->effects.size());
			}
		}
	}

	inline void SetSprintCost(const char* a_owner, float a_factor)
	{
		Set("fSprintStaminaDrainMult", a_owner, a_factor);
		detail::FindParagonDrain();
		auto& f = detail::g_sprintFactors[a_owner];
		if (f == 0.0F) { f = 1.0F; }
		if (f == a_factor) { return; }
		f = a_factor;
		float product = 1.0F;
		for (const auto& [owner, v] : detail::g_sprintFactors) { product *= v; }
		for (auto& s : detail::g_paragonDrain) { s.effect->effectItem.magnitude = s.base * product; }
		if (!detail::g_paragonDrain.empty()) { logger::info("sprint cost: {} factor {:.2f} - Paragon's drain at x{:.2f} of its own", a_owner, a_factor, product); }
	}
}
