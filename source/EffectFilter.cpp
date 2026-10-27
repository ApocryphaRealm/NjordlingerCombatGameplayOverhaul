#include "PCH.h"

// Njordlinger Combat Gameplay Overhaul - soul-stone effect filters (stub batch 1: Faas, Krah, Zun and Krii on armour).
//
// While worn armour holds one of these stones, a matching harmful effect is refused when it would be added to the
// player: fear (Faas), a frost slow (Krah), a cut to weapon damage (Zun), a cut to armour (Krii). The player's
// MagicTarget::AddTarget (vfunc 1) is hooked; the vtable is the one whose RTTI locator says it is the MagicTarget part of
// the player (its offset matches), so no index is guessed - anything unexpected is refused with a log line.

#include "SoulStones.h"

#include "utils/Logger.h"

namespace EffectFilter
{
	namespace
	{
		using AddTarget_t = bool (*)(RE::MagicTarget*, RE::MagicTarget::AddTargetData&);
		REL::Relocation<AddTarget_t> g_original;

		bool Refuse(RE::EffectSetting* a_mg, std::uint32_t a_filters)
		{
			if (!a_mg || !a_mg->IsDetrimental()) { return false; }
			const auto arch = a_mg->GetArchetype();
			const auto av = a_mg->data.primaryAV;
			if ((a_filters & 1u) && arch == RE::EffectSetting::Archetype::kDemoralize) { return true; }
			if ((a_filters & 2u) && av == RE::ActorValue::kSpeedMult && (a_mg->data.resistVariable == RE::ActorValue::kResistFrost || a_mg->HasKeywordString("MagicDamageFrost"))) { return true; }
			if ((a_filters & 4u) && (av == RE::ActorValue::kAttackDamageMult || av == RE::ActorValue::kWeaponSpeedMult)) { return true; }
			if ((a_filters & 8u) && av == RE::ActorValue::kDamageResist) { return true; }
			return false;
		}

		bool AddTarget(RE::MagicTarget* a_this, RE::MagicTarget::AddTargetData& a_data)
		{
			const auto filters = SoulStones::ActiveFilters();
			if (filters && a_data.effect && Refuse(a_data.effect->baseEffect, filters))
			{
				logger::debug("soul stones: filter refused {} on the player", a_data.effect->baseEffect->GetName());
				return false;
			}
			return g_original(a_this, a_data);
		}
	}

	void Install()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) { logger::warn("effect filter: no player object - not installed"); return; }
		const auto want = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(player->AsMagicTarget()) - reinterpret_cast<std::uintptr_t>(player));
		for (std::size_t i = 0; i < RE::VTABLE_PlayerCharacter.size(); ++i)
		{
			const auto addr = RE::VTABLE_PlayerCharacter[i].address();   // 0 for an entry with no ID on this runtime
			if (!addr) { continue; }
			REL::Relocation<std::uintptr_t> vtbl{ addr };
			const auto col = *reinterpret_cast<const std::uintptr_t*>(vtbl.address() - sizeof(void*));
			if (!col) { continue; }
			const auto offset = *reinterpret_cast<const std::uint32_t*>(col + 4);
			if (offset != want) { continue; }
			g_original = vtbl.write_vfunc(0x1, AddTarget);
			logger::info("effect filter: the player's MagicTarget::AddTarget hooked (vtable {} at offset {:#x})", i, want);
			return;
		}
		logger::warn("effect filter: no player vtable sits at the MagicTarget offset {:#x} - the Faas, Krah, Zun and Krii armour stones do nothing", want);
	}
}
