#include "PCH.h"

#include "Skills.h"

#include "Patches.h"
#include "Settings.h"
#include "Signature.h"

#include "utils/Logger.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>

namespace
{
	std::atomic<float> g_lockedCap[skilllist::kCount] = {
		-1.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F,
		-1.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F
	};
}

// The symbols SkillCapStub.asm refers to. NJ_GetSkillCap is a pure lookup on a hot path: it never logs past its
// first eight calls (rule 14).
extern "C"
{
	void NJ_SkillCapStubXmm8();    // SkillCapStub.asm - SE 1.5.97 shape, cap in XMM8
	void NJ_SkillCapStubXmm10();   // SkillCapStub.asm - AE shape, cap in XMM10

	float NJ_GetSkillCap(std::uint32_t a_skillId)
	{
		float r = 100.0F;
		if (a_skillId >= 6 && a_skillId <= 23) { r = Skills::EffectiveCap(static_cast<int>(a_skillId - 6)); }
		static std::atomic<int> s_diag{ 0 };
		if (const int n = s_diag.fetch_add(1); n < 8) { logger::trace("cap-stub call #{}: skillId={} -> cap={:.1f}", n, a_skillId, r); }
		return r;
	}
}

namespace Skills
{
	namespace
	{
		// Two shapes of the same site (CPC's measurements, 2026-09-05). AE: the level copied to xmm8, the cap loaded
		// into xmm10, `comiss xmm0, xmm10`. SE 1.5.97: the cap loaded straight into xmm8, `comiss xmm6, xmm8`.
		// Technique credited to the Skyrim Skill Uncapper lineage (Kassent, Vadfromnu, Elys, Kasplat).
		constexpr const char* kSkillCapSigAE = "FF 50 18 44 0F 28 C0 F3 44 0F 10 15 ?? ?? ?? ?? 41 0F 2F C2 0F 83";
		constexpr const char* kSkillCapSigSE = "48 8B 01 FF 50 18 F3 44 0F 10 05 ?? ?? ?? ?? 0F 28 F0 41 0F 2F F0 0F 83";
		constexpr float kVanillaCapConstant = 100.0F;
		constexpr std::uint8_t kMovssRipPrefix[4] = { 0xF3, 0x44, 0x0F, 0x10 };
		std::uintptr_t g_capSite = 0;

		bool InstallCapPatch(std::string& a_reason)
		{
			std::uint8_t wantModrm = 0x15;
			const char* regName = "xmm10";
			void (*stub)() = &NJ_SkillCapStubXmm10;
			Signature::Result found = Signature::Find(kSkillCapSigAE, 7);
			if (!found.found)
			{
				found = Signature::Find(kSkillCapSigSE, 6);
				wantModrm = 0x05;
				regName = "xmm8";
				stub = &NJ_SkillCapStubXmm8;
			}
			if (!found.found)
			{
				a_reason = found.note + ". Skills cap at 100 as in vanilla, and a class's locked skills are not capped.";
				return false;
			}
			const auto site = found.address;
			const auto base = REL::Module::get().base();
			const auto* b = reinterpret_cast<const std::uint8_t*>(site);
			if (std::memcmp(b, kMovssRipPrefix, sizeof(kMovssRipPrefix)) != 0 || b[4] != wantModrm)
			{
				a_reason = std::format("refused: game offset 0x{:X} does not hold the expected movss {} load ({:02X} {:02X} {:02X} {:02X} {:02X}); nothing was written",
									   site - base, regName, b[0], b[1], b[2], b[3], b[4]);
				return false;
			}
			const auto disp = *reinterpret_cast<const std::int32_t*>(site + 5);
			const auto constAddr = site + 9 + static_cast<std::intptr_t>(disp);
			const float loaded = *reinterpret_cast<const float*>(constAddr);
			if (loaded != kVanillaCapConstant)
			{
				a_reason = std::format("refused: the constant that instruction loads is {} rather than 100.0, so this is not the skill-cap load; nothing was written", loaded);
				return false;
			}
			g_capSite = site;
			auto& trampoline = SKSE::GetTrampoline();
			trampoline.write_call<5>(site, reinterpret_cast<std::uintptr_t>(stub));
			REL::safe_fill(site + 5, 0x90, 4);
			logger::info("Skill caps: attached at game offset 0x{:X} ({} shape; the 100.0 constant sits at 0x{:X})", site - base, regName, constAddr - base);
			a_reason = std::format("attached at game offset 0x{:X} ({} shape): each skill stops at its cap, and a skill your class does not hold stops at its starting value.", site - base, regName);
			return true;
		}
	}

	void Register()
	{
		Patches::Register("Skill caps", "Where a skill stops advancing - the configured cap, and a locked skill's starting value.", InstallCapPatch);
	}

	std::uintptr_t CapSiteAddress() { return g_capSite; }

	void SetLockedCap(int a_skillIndex, float a_cap)
	{
		if (a_skillIndex < 0 || a_skillIndex >= skilllist::kCount) { return; }
		const float old = g_lockedCap[a_skillIndex].exchange(a_cap);
		if (old != a_cap) { logger::debug("skill cap: {} lock {:.1f} -> {:.1f}", skilllist::kDisplayName[a_skillIndex], old, a_cap); }
	}

	float LockedCap(int a_skillIndex)
	{
		return (a_skillIndex < 0 || a_skillIndex >= skilllist::kCount) ? -1.0F : g_lockedCap[a_skillIndex].load(std::memory_order_relaxed);
	}

	float EffectiveCap(int a_skillIndex)
	{
		if (a_skillIndex < 0 || a_skillIndex >= skilllist::kCount) { return 100.0F; }
		float r = settings::skills::overrideCaps ? settings::skills::cap[a_skillIndex] : 100.0F;
		const float locked = g_lockedCap[a_skillIndex].load(std::memory_order_relaxed);
		if (locked >= 0.0F && settings::classes::lockSkills) { r = std::min(r, locked); }
		return r;
	}

	State GetState()
	{
		State s;
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) { return s; }
		s.characterLevel = player->GetLevel();
		// Through GetPlayerRuntimeData(): the member's offset differs between runtimes and a direct read compiles and is
		// wrong (gate rule runtime-data-members-through-the-accessor).
		auto* skills = player->GetPlayerRuntimeData().skills;
		if (!skills || !skills->data) { return s; }
		const auto* data = skills->data;
		s.characterXp = data->xp;
		s.characterThreshold = data->levelThreshold;
		for (int i = 0; i < skilllist::kCount; ++i)
		{
			s.skill[i].level = data->skills[i].level;
			s.skill[i].xp = data->skills[i].xp;
			s.skill[i].levelThreshold = data->skills[i].levelThreshold;
		}
		s.readable = true;
		return s;
	}
}
