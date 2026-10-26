// Njordlinger Combat Gameplay Overhaul - the perk-pool write, copied from Character Progression Control
// (ApocryphaRealm, GPL-3.0-or-later) unchanged but for its symbol names.
#include "PCH.h"

#include "LevelUp.h"

#include "Patches.h"
#include "Settings.h"
#include "Signature.h"

#include "utils/Logger.h"

#include <algorithm>
#include <cstring>
#include <format>

extern "C"
{
	std::uintptr_t NJ_PerkPoolReturn = 0;   // where the stub resumes: the instruction after the sequence
	std::uintptr_t NJ_PerkPoolPlayer = 0;   // the game's player-pointer global the sequence loads into rdx
	std::int32_t   NJ_PerkPoolNewCount = 0; // what the sequence leaves in ecx/eax: the new perk count
	void NJ_PerkPoolStub();                 // SkillCapStub.asm

	// Called by the stub with the count the game was about to add (positive on a level up; negative
	// when something takes points away). Applies this mod's table on a grant and leaves removals
	// exactly as the game had them.
	void NJ_PerkPool_Hook(std::int32_t a_count)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) { return; }
		auto& stats = player->GetGameStatsData();
		int delta = a_count;
		if (settings::levelup::overrideRewards && a_count > 0)
		{
			delta = settings::levelup::PerksAtLevel(player->GetLevel());
		}
		const int sum = std::clamp(static_cast<int>(stats.perkCount) + delta, 0, 127);
		NJ_PerkPoolNewCount = sum;
		logger::info("level up: perk points {} -> {} (game wanted {:+}, this mod granted {:+} at level {})",
					 stats.perkCount, sum, a_count, delta, player->GetLevel());
		stats.perkCount = static_cast<std::int8_t>(sum);
	}
}

namespace LevelUp
{
	namespace
	{
		// The perk-pool sequence on Skyrim SE (Kassent's 2017 SE uncapper; present on 1.5.97 - measured
		// 2026-09-05). The signature anchors 0x1C bytes before it; the sequence itself is:
		//   48 8B 15 xx xx xx xx   mov rdx, [rip+thePlayer]
		//   0F B6 8A dd dd dd dd   movzx ecx, byte ptr [rdx+perkCount]
		//   8B C1                  mov eax, ecx
		//   03 C3                  add eax, ebx            (ebx = the count to add)
		//   78 08                  js  past-the-store
		//   02 CB                  add cl, bl
		//   88 8A dd dd dd dd      mov byte ptr [rdx+perkCount], cl
		// 0x1C bytes in all; the stub replaces the lot and resumes after it.
		constexpr const char* kPerkPoolSig =
			"48 85 C0 74 33 66 0F 6E C3 0F 5B C0 F3 0F 58 40 34 F3 0F 11 40 34 48 83 C4 20 5B C3 "
			"48 8B 15 ?? ?? ?? ?? 0F B6 8A ?? ?? ?? ?? 8B C1 03 C3 78 08 02 CB 88 8A ?? ?? ?? ??";
		constexpr std::ptrdiff_t kPerkPoolOffset = 0x1C;
		constexpr std::size_t kSequenceLength = 0x1C;

		bool InstallPerkPoints(std::string& a_reason)
		{
			const auto found = Signature::Find(kPerkPoolSig, kPerkPoolOffset);
			if (!found.found) { a_reason = found.note + ". A level up grants exactly what vanilla grants."; return false; }
			const auto site = found.address;
			const auto base = REL::Module::get().base();
			const auto* b = reinterpret_cast<const std::uint8_t*>(site);

			// Prove it is the perk pool: the displacement the sequence reads and writes must be the
			// perk-count field of the live player object, as CommonLibSSE-NG lays it out.
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) { a_reason = "the player object was not available when patches were installed; nothing was written"; return false; }
			const auto perkOffset = static_cast<std::uint32_t>(
				reinterpret_cast<std::uintptr_t>(&player->GetGameStatsData().perkCount) - reinterpret_cast<std::uintptr_t>(player));
			std::uint32_t readDisp = 0, writeDisp = 0;
			std::memcpy(&readDisp, b + 10, 4);
			std::memcpy(&writeDisp, b + 0x18, 4);
			if (readDisp != perkOffset || writeDisp != perkOffset)
			{
				a_reason = std::format("refused: the sequence at game offset 0x{:X} reads/writes +0x{:X}/+0x{:X}, but the player's perk "
									   "count sits at +0x{:X}; nothing was written. A level up grants exactly what vanilla grants.",
									   site - base, readDisp, writeDisp, perkOffset);
				return false;
			}
			const std::string located = std::format("verified at game offset 0x{:X} (the perk-pool sequence; perk count at +0x{:X})", site - base, perkOffset);
			// Attached whether or not the toggle is on (gate rule no-restart-gated-installers); the hook reads it live.
			// The sequence's own register effects are reproduced by the stub: rdx = the player pointer it
			// loads (rip-relative in its first instruction), ecx/eax = the new count.
			{
				std::int32_t rel = 0; std::memcpy(&rel, b + 3, 4);
				NJ_PerkPoolPlayer = site + 7 + static_cast<std::intptr_t>(rel);
			}
			NJ_PerkPoolReturn = site + kSequenceLength;
			SKSE::GetTrampoline().write_branch<5>(site, reinterpret_cast<std::uintptr_t>(&NJ_PerkPoolStub));
			REL::safe_fill(site + 5, 0x90, kSequenceLength - 5);
			logger::info("Perk points: attached at game offset 0x{:X} (resume 0x{:X}); each level up now grants the table's whole "
						 "number of perks", site - base, NJ_PerkPoolReturn - base);
			a_reason = located + ". Attached: while Control what a level up grants is on, each level up grants the number of perks the table gives for the level reached; off, exactly what vanilla grants.";
			return true;
		}
	}

	void Register()
	{
		Patches::Register(
			"Perk points",
			"How many perk points a level up grants, from the table by level.",
			InstallPerkPoints);
	}
}
