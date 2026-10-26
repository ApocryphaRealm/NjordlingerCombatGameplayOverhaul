#include "PCH.h"

#include "LevelUpGains.h"

#include "Patches.h"
#include "Settings.h"
#include "Signature.h"

#include "utils/Logger.h"

#include <atomic>
#include <cstring>
#include <format>

namespace LevelUpGains
{
	namespace
	{
		// The attribute-level-up routine (Kassent's SE pattern, 0x1E bytes into it; entry `push rdi; sub rsp,30h`),
		// as CPC locates it - present on 1.5.97 and 1.7.104 (CPC's Patches tab, 2026-09).
		constexpr const char* kSig =
			"0F B6 DA 48 8B F9 48 8B 15 ?? ?? ?? ?? 48 81 C2 28 01 00 00 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? "
			"84 C0 0F 84 BA 00 00 00 84 DB 0F 85 AA 00 00 00";
		constexpr std::ptrdiff_t kEntryBack = 0x1E;
		constexpr std::uint8_t kPrologue6[6] = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x30 };
		// The chosen attribute: an ActorValue id at +0x18 of the first argument.
		constexpr std::ptrdiff_t kChoiceOffset = 0x18;
		constexpr std::uint32_t kHealth = 0x18, kMagicka = 0x19, kStamina = 0x1A;
		constexpr const char* kName[3] = { "health", "magicka", "stamina" };

		using Fn_t = std::uint64_t(void*, std::uint8_t);
		REL::Relocation<Fn_t> g_orig;
		std::atomic<bool> g_attached{ false };
		std::atomic<std::uint32_t> g_levelUps{ 0 };
		std::atomic<int> g_lastChoice{ -1 };
		std::atomic<int> g_lastGain{ 0 };


		// PLAN.md phase 2 step 8b: with every gain at 0 the attribute pick is skipped and the level up completes on its
		// own (what No Levelup Menu SKSE does, without the extra requirement). The LevelUp Menu's choose-attribute
		// function - what its addHealth / addMagicka / addStamina delegate handlers jump to - is resolved the way CPC's
		// test op resolves it (from the "addHealth" string: its one code reference is the delegate registration, the
		// handler is loaded beside it, three instructions ending in a jump to the function); CPC drove the game's own
		// level-up with it headlessly. Calling it with health completes the level up through the game's own path: the
		// gain is 0 (the hook above) and the perk point is granted by the perk-pool hook.
		std::uintptr_t g_choose = 0;
		std::atomic<std::uint32_t> g_skipped{ 0 };

		void ResolveChooseFunction()
		{
			const auto& mod = REL::Module::get();
			const auto base = mod.base();
			const auto text = mod.segment(REL::Segment::textx);
			const auto rdata = mod.segment(REL::Segment::rdata);
			const std::uintptr_t t0 = text.address(), t1 = t0 + text.size();
			const char* needle = "addHealth";
			const std::size_t nlen = std::strlen(needle) + 1;
			std::uintptr_t strAddr = 0;
			for (std::uintptr_t p = rdata.address(); p + nlen <= rdata.address() + rdata.size(); ++p)
			{
				if (std::memcmp(reinterpret_cast<const void*>(p), needle, nlen) == 0) { strAddr = p; break; }
			}
			if (!strAddr) { logger::warn("level-up skip: the addHealth string was not found - the attribute pick stays"); return; }
			for (std::uintptr_t p = t0; p + 4 <= t1 && !g_choose; ++p)
			{
				const auto rel = *reinterpret_cast<const std::int32_t*>(p);
				if (p + 4 + static_cast<std::intptr_t>(rel) != strAddr) { continue; }
				for (std::uintptr_t q = p - 0x20; q < p + 0x20 && !g_choose; ++q)
				{
					const auto* b2 = reinterpret_cast<const std::uint8_t*>(q);
					if ((b2[0] != 0x48 && b2[0] != 0x4C) || b2[1] != 0x8D || (b2[2] & 0xC7) != 0x05) { continue; }
					const auto r2 = *reinterpret_cast<const std::int32_t*>(q + 3);
					const auto tgt = q + 7 + static_cast<std::intptr_t>(r2);
					if (tgt < t0 || tgt >= t1) { continue; }
					const auto* h = reinterpret_cast<const std::uint8_t*>(tgt);
					// handler shape: mov rcx,[rcx+18h]; test rcx,rcx; jz +A; mov edx,<av>; jmp <choose>
					if (h[0] == 0x48 && h[1] == 0x8B && h[2] == 0x49 && h[3] == 0x18 && h[9] == 0xBA && h[14] == 0xE9)
					{
						const auto jrel = *reinterpret_cast<const std::int32_t*>(h + 15);
						g_choose = tgt + 19 + static_cast<std::intptr_t>(jrel);
						logger::info("level-up skip: the LevelUp Menu's choose-attribute function is at game offset 0x{:X}", g_choose - base);
					}
				}
			}
			if (!g_choose) { logger::warn("level-up skip: the choose-attribute function was not resolved - the attribute pick stays"); }
		}

		bool AllGainsZero()
		{
			return settings::levelup::overrideRewards && settings::levelup::healthPerLevel < 0.5F && settings::levelup::magickaPerLevel < 0.5F &&
				   settings::levelup::staminaPerLevel < 0.5F;
		}

		class LevelUpMenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || !a_event->opening || a_event->menuName != RE::LevelUpMenu::MENU_NAME) { return RE::BSEventNotifyControl::kContinue; }
				if (!g_choose || !g_attached.load() || !AllGainsZero())
				{
					logger::debug("level-up skip: not skipping (resolved {}, gains hook {}, all gains 0 {})", g_choose != 0, g_attached.load(), AllGainsZero());
					return RE::BSEventNotifyControl::kContinue;
				}
				// A UI task: it runs inside the UI's own message processing, after the menu has finished opening
				// (gate rule scaleform-calls-only-on-the-ui-thread - the function drives the menu's movie).
				if (auto* tasks = SKSE::GetTaskInterface())
				{
					const auto choose = g_choose;
					tasks->AddUITask([choose]() {
						auto* ui = RE::UI::GetSingleton();
						auto menu = ui ? ui->GetMenu(RE::LevelUpMenu::MENU_NAME) : RE::GPtr<RE::IMenu>{};
						if (!menu || !menu->uiMovie) { logger::warn("level-up skip: the LevelUp Menu was gone before the pick could be made"); return; }
						using Choose_t = void(RE::IMenu*, std::uint32_t);
						reinterpret_cast<Choose_t*>(choose)(menu.get(), kHealth);
						++g_skipped;
						logger::info("level-up skip: attribute pick made for the player (every gain is 0); the level up completes with its perk points");
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		LevelUpMenuSink g_menuSink;

		int IndexOf(std::uint32_t a_choice)
		{
			return a_choice == kHealth ? 0 : a_choice == kMagicka ? 1 : a_choice == kStamina ? 2 : -1;
		}

		std::uint64_t Hook(void* a_arg0, std::uint8_t a_arg1)
		{
			if (!a_arg0) { return g_orig(a_arg0, a_arg1); }
			const auto choice = *reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(a_arg0) + kChoiceOffset);
			const int idx = IndexOf(choice);
			auto* gs = RE::GameSettingCollection::GetSingleton();
			auto* gain = gs ? gs->GetSetting("iAVDhmsLevelUp") : nullptr;
			if (!gain || idx < 0 || !settings::levelup::overrideRewards)
			{
				if (!gain || idx < 0)
				{
					static std::atomic<bool> once{ false };
					if (!once.exchange(true)) { logger::warn("level-up gains: could not apply this mod's value (choice {}, setting {}) - the game's own applied", choice, gain != nullptr); }
				}
				return g_orig(a_arg0, a_arg1);
			}
			const auto vanillaGain = gain->data.i;
			const float amount = idx == 0 ? settings::levelup::healthPerLevel : idx == 1 ? settings::levelup::magickaPerLevel : settings::levelup::staminaPerLevel;
			gain->data.i = static_cast<std::int32_t>(amount + 0.5F);
			// One carry-weight writer (D6, Q51): Adaptive Attributes' stamina milestones. The game's own +carry weight on the
			// stamina choice is held at 0 for the length of the call.
			auto* carry = gs->GetSetting("fLevelUpCarryWeightMod");
			const float vanillaCarry = carry ? carry->data.f : 0.0F;
			if (carry) { carry->data.f = 0.0F; }
			logger::info("level-up gains: {} chosen -> +{}, carry weight +0 (the install's own values {} / {:.1f}, restored after the call)", kName[idx], gain->data.i, vanillaGain, vanillaCarry);
			const auto r = g_orig(a_arg0, a_arg1);
			g_lastGain = gain->data.i;
			gain->data.i = vanillaGain;
			if (carry) { carry->data.f = vanillaCarry; }
			g_lastChoice = idx;
			++g_levelUps;
			return r;
		}

		bool Install(std::string& a_reason)
		{
			const auto found = Signature::Find(kSig, -kEntryBack);
			if (!found.found) { a_reason = found.note + ". A level up grants exactly the health, magicka or stamina vanilla grants."; return false; }
			const auto entry = found.address;
			const auto base = REL::Module::get().base();
			if (std::memcmp(reinterpret_cast<const void*>(entry), kPrologue6, 6) != 0)
			{
				const auto* b = reinterpret_cast<const std::uint8_t*>(entry);
				a_reason = std::format("refused: game offset 0x{:X} does not start with push rdi; sub rsp,30h ({:02X} {:02X} {:02X} {:02X} {:02X} {:02X}); nothing was written",
									   entry - base, b[0], b[1], b[2], b[3], b[4], b[5]);
				return false;
			}
			auto& trampoline = SKSE::GetTrampoline();
			auto* thunk = static_cast<std::uint8_t*>(trampoline.allocate(6 + 6 + 8));
			if (!thunk) { a_reason = "refused: no trampoline space for the prologue thunk; nothing was written"; return false; }
			// The "original": the 6 overwritten prologue bytes, then jmp [rip+0] to entry+6.
			std::memcpy(thunk, kPrologue6, 6);
			thunk[6] = 0xFF;
			thunk[7] = 0x25;
			std::memset(thunk + 8, 0, 4);
			const std::uintptr_t resume = entry + 6;
			std::memcpy(thunk + 12, &resume, 8);
			g_orig = REL::Relocation<Fn_t>(reinterpret_cast<std::uintptr_t>(thunk));
			trampoline.write_branch<5>(entry, Hook);
			g_attached = true;
			ResolveChooseFunction();
			logger::info("Level-up gains: attached at the attribute level-up routine, game offset 0x{:X}", entry - base);
			a_reason = std::format("attached at game offset 0x{:X}: while Control what a level up grants is on, the chosen attribute grows by this mod's value (a level up adds no carry weight: Adaptive Attributes is the one carry-weight writer).", entry - base);
			return true;
		}
	}

	void Register()
	{
		Patches::Register("Level-up gains", "The health, magicka or stamina a level up grants; with all three at 0 the attribute pick is skipped.", Install);
		if (auto* ui = RE::UI::GetSingleton()) { ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink); }
	}

	State GetState()
	{
		State s;
		s.attached = g_attached.load();
		s.levelUps = g_levelUps.load();
		s.lastChoice = g_lastChoice.load();
		s.lastGain = g_lastGain.load();
		s.skipResolved = g_choose != 0;
		s.picksSkipped = g_skipped.load();
		return s;
	}
}
