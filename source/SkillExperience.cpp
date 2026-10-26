// Njordlinger Combat Gameplay Overhaul - copied from Character Progression Control (ApocryphaRealm, GPL-3.0-or-later),
// trimmed of skill points and static levelling; a class's locked skill (PLAN.md D3) banks nothing through use.
#include "PCH.h"

#include "SkillExperience.h"

#include "Patches.h"
#include "Settings.h"
#include "Signature.h"
#include "Skills.h"

#include "utils/Logger.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>

#include <Windows.h>

extern "C"
{
	// SkillCapStub.asm: takes the place of the game's `call` to its level-experience function -
	// calls the original with xmm0..xmm3 untouched, scales the result through NJ_LevelExp_Hook,
	// and returns to the game's call site.
	std::uintptr_t NJ_LevelExpOriginal = 0;   // the game's level-experience function; set at install
	void NJ_LevelExpCallStub();
}

namespace SkillExperience
{
	namespace
	{
		// The skill-improve function's prologue on Skyrim SE (from Kassent's 2017 SE uncapper;
		// present on 1.5.97 - measured 2026-09-05): mov r11,rsp / push rbp / push rsi / push r14 /
		// sub rsp,160h / lea eax,[rdx-6] / movaps [r11-38h],xmm7 / mov rbp,r9 / movaps xmm7,xmm2 /
		// mov esi,edx (the skill id every later site reads from rsi) / mov r14,rcx / cmp eax,11h.
		constexpr const char* kImproveSig =
			"4C 8B DC 55 56 41 56 48 81 EC 60 01 00 00 8D 42 FA 41 0F 29 7B C8 49 8B E9 0F 28 FA 8B F2 4C 8B F1 83 F8 11";
		// The three whole instructions a 5-byte branch at the entry overwrites - re-executed verbatim
		// by the "original" thunk (they are position-independent), then a jump to entry+5.
		constexpr std::uint8_t kPrologue5[5] = { 0x4C, 0x8B, 0xDC, 0x55, 0x56 };
		// The same shape without its first five bytes, anchored back to the entry: what is left of the prologue
		// once ANOTHER plugin has written its own 5-byte branch there. Actor Value Generator does (read out of the
		// running game on 2026-09-20: 0x6E61D0 began `E9 ..` into ActorValueGenerator.dll), and the Skyrim Skill
		// Uncapper hooks the same entry. The full shape then matches nowhere, and up to 1.1.8 this mod concluded
		// "not present in this build" and left skill experience to vanilla - with skill points on, that is the
		// Nexus report of 2026-09-20 (xLenax): skills still levelled through use.
		constexpr const char* kImproveTailSig =
			"41 56 48 81 EC 60 01 00 00 8D 42 FA 41 0F 29 7B C8 49 8B E9 0F 28 FA 8B F2 4C 8B F1 83 F8 11";
		std::uintptr_t g_foreignHook = 0;   // where the branch already sitting at the entry goes, when another plugin hooked first
		std::string g_foreignOwner;         // that plugin's file name, for the log and the Patches tab

		// The module that owns an address, following up to two `jmp [rip+0]` thunks first (an SKSE trampoline sits
		// outside every module; the hook it forwards to does not).
		std::string OwnerOf(std::uintptr_t a_target)
		{
			auto p = a_target;
			for (int hop = 0; hop < 2; ++hop)
			{
				const auto* b = reinterpret_cast<const std::uint8_t*>(p);
				if (b[0] == 0xFF && b[1] == 0x25)
				{
					const auto rel = *reinterpret_cast<const std::int32_t*>(p + 2);
					p = *reinterpret_cast<const std::uintptr_t*>(p + 6 + rel);
				}
				else { break; }
			}
			HMODULE mod = nullptr;
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(p), &mod) && mod)
			{
				wchar_t buf[MAX_PATH]{};
				if (GetModuleFileNameW(mod, buf, MAX_PATH))
				{
					std::wstring w(buf);
					const auto slash = w.find_last_of(L"\\/");
					w = slash == std::wstring::npos ? w : w.substr(slash + 1);
					std::string out;
					for (wchar_t c : w) { out += static_cast<char>(c < 128 ? c : '?'); }
					return out;
				}
			}
			return std::format("code outside every loaded module (0x{:X})", p);
		}

		std::string ChainNote()
		{
			return g_foreignHook ? std::format(" {} hooks the same entry; this mod runs first and hands every call it does not stop on to it.", g_foreignOwner) : std::string();
		}
		// The level-experience function - "what a skill increase pays toward a level". On 1.5.97 it is a
		// real leaf function (measured 2026-09-05 from a byte dump): `sub rsp,18h` and then this shape,
		// ending in `add rsp,18h; ret`. The shape starts 4 bytes AFTER the entry - an earlier version
		// looked for calls to the shape itself and found none - and the function reads xmm0..xmm3, so
		// the wrapper passes all four through. The skill-improve function calls it exactly once.
		constexpr const char* kLevelExpSig =
			"F3 0F 58 D3 0F 28 E0 0F 29 34 24 0F 57 F6 0F 28 CA F3 0F 58 CB F3 0F 59 CA F3 0F 59 CD F3 0F 2C C1";
		constexpr std::ptrdiff_t kImproveScan = 0x1800;  // the function is long; the call sits well past the entry
		constexpr std::ptrdiff_t kLevelExpEntryScan = 0x60;                    // the entry sits within this many bytes before the shape
		constexpr std::uint8_t kLevelExpPrologue[4] = { 0x48, 0x83, 0xEC, 0x18 }; // sub rsp,18h
		constexpr std::uint8_t kLevelExpEpilogue[5] = { 0x48, 0x83, 0xC4, 0x18, 0xC3 }; // add rsp,18h; ret

		using Improve_t = void(RE::PlayerCharacter::PlayerSkills*, std::uint32_t, float, std::uint64_t, std::uint32_t, std::uint8_t, bool);
		REL::Relocation<Improve_t> g_origImprove;
		std::uintptr_t g_improveEntry = 0;

		void Improve_Hook(RE::PlayerCharacter::PlayerSkills* a_skills, std::uint32_t a_skill, float a_exp,
						  std::uint64_t a_1, std::uint32_t a_2, std::uint8_t a_3, bool a_4)
		{
			const float incoming = a_exp;
			if (a_skill >= 6 && a_skill <= 23)
			{
				const auto i = a_skill - 6;
				if (Skills::LockedCap(static_cast<int>(i)) >= 0.0F && settings::classes::lockSkills)
				{
					// PLAN.md D3: the class does not hold this skill. The improve is SKIPPED, not fed 0 - the game multiplies
					// what arrives by the skill's use multiplier and then ADDS its use offset (Lockpicking 25, Pickpocket
					// 8.1, Alchemy 3, Sneak 1.5), so 0 in would still bank the offset (CPC 1.1.9's finding).
					static std::atomic<int> skipped{ 0 };
					if (skipped.fetch_add(1) < 8) { logger::debug("class lock: skill {} use ({:.2f}) not banked", a_skill, incoming); }
					return;
				}
				if (settings::skillexp::overrideRates)
				{
					a_exp *= settings::skillexp::mult[i];
				}
			}
			{
				// TRACE (first 24 calls): how many times one use enters here, and with what.
				static std::atomic<int> n{ 0 };
				const int k = n.fetch_add(1);
				if (k < 24 && a_skill >= 6 && a_skill <= 23)
				{
					const bool ok = a_skills && a_skills->data;
					logger::trace("improve call #{}: skill {} in {:.2f} -> out {:.2f} (level {:.1f}, xp {:.1f}/{:.1f}) args {} {} {} {}", k, a_skill, incoming, a_exp,
								 ok ? a_skills->data->skills[a_skill - 6].level : -1.0F, ok ? a_skills->data->skills[a_skill - 6].xp : -1.0F,
								 ok ? a_skills->data->skills[a_skill - 6].levelThreshold : -1.0F, a_1, a_2, a_3, a_4);
				}
			}
			g_origImprove(a_skills, a_skill, a_exp, a_1, a_2, a_3, a_4);
		}

		// Locate the skill-improve function once, and prove it: the shape matches exactly once, the
		// entry carries the prologue bytes the thunk will re-execute, and the skill-cap site this mod
		// already verified lies inside it.
		bool LocateImprove(std::string& a_reason)
		{
			if (g_improveEntry) { return true; }
			const auto base = REL::Module::get().base();
			auto found = Signature::Find(kImproveSig, 0);
			if (!found.found && found.matches == 0)
			{
				// Nowhere at all: another plugin may already hold the entry. The shape minus the five bytes a
				// branch overwrites still has to match exactly once, and what sits at the entry has to BE a
				// branch - any other bytes there are refused, as before.
				const auto tail = Signature::Find(kImproveTailSig, -5);
				if (tail.found)
				{
					const auto* b = reinterpret_cast<const std::uint8_t*>(tail.address);
					if (b[0] != 0xE9)
					{
						a_reason = std::format("refused: game offset 0x{:X} starts with {:02X} {:02X} {:02X} {:02X} {:02X}, neither the prologue nor "
											   "another plugin's branch; nothing was written", tail.address - base, b[0], b[1], b[2], b[3], b[4]);
						return false;
					}
					const auto rel = *reinterpret_cast<const std::int32_t*>(tail.address + 1);
					g_foreignHook = tail.address + 5 + rel;
					g_foreignOwner = OwnerOf(g_foreignHook);
					logger::info("skill-improve entry at game offset 0x{:X} is already hooked by {} (branch to 0x{:X}); this mod's hook will sit in front of it and call it",
								 tail.address - base, g_foreignOwner, g_foreignHook);
					found = tail;
				}
			}
			if (!found.found) { a_reason = found.note; return false; }
			const auto entry = found.address;
			if (!g_foreignHook && std::memcmp(reinterpret_cast<const void*>(entry), kPrologue5, sizeof(kPrologue5)) != 0)
			{
				a_reason = std::format("refused: game offset 0x{:X} does not start with the expected prologue; nothing was written", entry - base);
				return false;
			}
			const auto capSite = Skills::CapSiteAddress();
			if (capSite && !(capSite > entry && capSite < entry + kImproveScan))
			{
				a_reason = std::format("refused: the verified skill-cap site (0x{:X}) does not lie inside the function found at 0x{:X}; nothing was written", capSite - base, entry - base);
				return false;
			}
			g_improveEntry = entry;
			return true;
		}

		// Attaches the entry hook once; both the rates group and the static group ride it.
		bool AttachImprove(std::string& a_reason)
		{
			if (g_origImprove.address()) { return true; }
			const auto base = REL::Module::get().base();
			auto& trampoline = SKSE::GetTrampoline();
			auto* thunk = static_cast<std::uint8_t*>(trampoline.allocate(5 + 6 + 8));
			if (!thunk) { a_reason = "refused: no trampoline space for the prologue thunk; nothing was written"; return false; }
			if (g_foreignHook)
			{
				// Another plugin's branch is at the entry. Its own thunk re-executes the prologue, so the "original"
				// for this hook is that plugin's hook: `jmp [rip+0]` straight to it. The branch this mod writes over
				// theirs is what the game runs first, and theirs still runs on every call this mod passes through.
				thunk[0] = 0xFF; thunk[1] = 0x25; std::memset(thunk + 2, 0, 4);
				std::memcpy(thunk + 6, &g_foreignHook, 8);
			}
			else
			{
				// The "original": the 5 overwritten prologue bytes, then `jmp [rip+0]` to entry+5.
				std::memcpy(thunk, kPrologue5, 5);
				thunk[5] = 0xFF; thunk[6] = 0x25; std::memset(thunk + 7, 0, 4);
				const std::uintptr_t resume = g_improveEntry + 5;
				std::memcpy(thunk + 11, &resume, 8);
			}
			g_origImprove = REL::Relocation<Improve_t>(reinterpret_cast<std::uintptr_t>(thunk));
			trampoline.write_branch<5>(g_improveEntry, Improve_Hook);
			if (g_foreignHook) { logger::info("skill-improve entry hook attached at game offset 0x{:X}, in front of {}'s hook, which it calls", g_improveEntry - base, g_foreignOwner); }
			else { logger::info("skill-improve entry hook attached at game offset 0x{:X}", g_improveEntry - base); }
			return true;
		}

		bool InstallRates(std::string& a_reason)
		{
			if (!LocateImprove(a_reason)) { a_reason += ". Experience is earned exactly as in vanilla."; return false; }
			const auto base = REL::Module::get().base();
			// Attached whether or not the toggle is on (gate rule no-restart-gated-installers); the hook reads it live.
			if (!AttachImprove(a_reason)) { return false; }
			logger::info("Skill experience rates: applied at the skill-improve entry, game offset 0x{:X}; each use now pays "
						 "fMult<Skill> times vanilla", g_improveEntry - base);
			a_reason = std::format("attached at game offset 0x{:X}: each use of a skill pays vanilla times that skill's "
								   "Experience rate.{}", g_improveEntry - base, ChainNote());
			return true;
		}

		bool InstallLevelIncome(std::string& a_reason)
		{
			if (!LocateImprove(a_reason)) { a_reason += ". A skill increase pays toward a level exactly as in vanilla."; return false; }
			const auto base = REL::Module::get().base();
			const auto maths = Signature::Find(kLevelExpSig, 0);
			if (!maths.found) { a_reason = maths.note + ". A skill increase pays toward a level exactly as in vanilla."; return false; }
			// Prove it is a self-contained function: its prologue `sub rsp,18h` is the first instruction after the
			// previous function's padding or `ret` (measured 2026-09-05: constant loads sit between the prologue
			// and the shape, so the entry is not a fixed distance back), and its epilogue is within the shape.
			std::uintptr_t fnEntry = 0;
			for (std::uintptr_t p = maths.address - 4; p >= maths.address - kLevelExpEntryScan; --p)
			{
				if (std::memcmp(reinterpret_cast<const void*>(p), kLevelExpPrologue, 4) != 0) { continue; }
				const auto before = *reinterpret_cast<const std::uint8_t*>(p - 1);
				if (before == 0xCC || before == 0xC3) { fnEntry = p; break; }
			}
			if (!fnEntry)
			{
				std::string hex;
				for (std::uintptr_t p = maths.address - kLevelExpEntryScan; p < maths.address; ++p) { hex += std::format("{:02X} ", *reinterpret_cast<const std::uint8_t*>(p)); }
				logger::info("bytes before the level-experience shape (shape-0x{:X}..shape): {}", kLevelExpEntryScan, hex);
				a_reason = std::format("refused: no function-boundary `sub rsp,18h` within 0x{:X} bytes before the level-experience shape at 0x{:X}; "
									   "nothing was written", kLevelExpEntryScan, maths.address - base);
				return false;
			}
			bool epilogue = false;
			for (std::uintptr_t p = maths.address; p + 5 <= maths.address + 0x100; ++p)
			{
				if (std::memcmp(reinterpret_cast<const void*>(p), kLevelExpEpilogue, 5) == 0) { epilogue = true; break; }
			}
			if (!epilogue)
			{
				a_reason = std::format("refused: no `add rsp,18h; ret` after the level-experience shape at 0x{:X}; nothing was written", maths.address - base);
				return false;
			}
			// The one call to it inside the skill-improve function is the site.
			std::uintptr_t callSite = 0; int calls = 0;
			const auto scanEnd = std::min<std::uintptr_t>(g_improveEntry + kImproveScan, fnEntry);
			for (std::uintptr_t p = g_improveEntry; p + 5 <= scanEnd; ++p)
			{
				if (*reinterpret_cast<const std::uint8_t*>(p) != 0xE8) { continue; }
				const auto rel = *reinterpret_cast<const std::int32_t*>(p + 1);
				if (p + 5 + static_cast<std::intptr_t>(rel) == fnEntry) { callSite = p; ++calls; }
			}
			if (calls != 1)
			{
				a_reason = std::format("refused: expected exactly one call to the level-experience function (0x{:X}) inside the "
									   "skill-improve function, found {}; nothing was written", fnEntry - base, calls);
				return false;
			}
			// Attached whether or not the toggle is on (gate rule no-restart-gated-installers); the hook reads it live.
			NJ_LevelExpOriginal = fnEntry;
			SKSE::GetTrampoline().write_call<5>(callSite, reinterpret_cast<std::uintptr_t>(&NJ_LevelExpCallStub));
			logger::info("Skill-to-level income: the level-experience call at game offset 0x{:X} now goes through this mod "
						 "(function at 0x{:X}); a skill increase pays fSkillToLevelMult times vanilla", callSite - base, fnEntry - base);
			a_reason = std::format("attached at game offset 0x{:X}: a skill increase pays vanilla times the Skill increase -> level multiplier "
								   "while Control skill experience rates is on, and vanilla while it is off.", callSite - base);
			return true;
		}
	}

	bool ImproveHookAttached() { return g_origImprove.address() != 0; }

	void Register()
	{
		Patches::Register(
			"Skill experience rates",
			"What one use of a skill pays toward that skill.",
			InstallRates);
		Patches::Register(
			"Skill-to-level income",
			"What a skill increase pays toward a character level.",
			InstallLevelIncome);
	}
}

extern "C" float NJ_LevelExp_Hook(float a_value, std::uint32_t a_skillId)
{
	// This runs inside NJ_LevelExpCallStub, which preserves every register the game's call site relies on
	// (rcx above all - the site adds the result to [rcx] the instruction after the call), so ordinary code,
	// logging included, is safe here. It is called once per skill increase, never per frame.
	float result = a_value;
	const char* why = "vanilla";
	if (settings::skillexp::overrideRates) { result = a_value * settings::skillexp::toLevelMult; why = "times the Skill increase -> level multiplier"; }
	logger::debug("level income: skill {} increase pays {:.1f} -> {:.1f} ({})", a_skillId, a_value, result, why);
	return result;
}
