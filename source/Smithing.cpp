#include "PCH.h"

#include "Smithing.h"

#include "ClassData.h"
#include "Classes.h"
#include "Settings.h"

#include "utils/Logger.h"
#include "utils/Notify.h"
#include "utils/Strings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

// The game code this module touches was read from the decrypted 1.5.97 and 1.7.104 .text dumps (D:\Claude output\.tools
// \dumps, .MD\scripts\gamecode.py), 2026-11-10:
//  - the smithing menu's list builder, 1.5.97 id 50296 (+0x153) and 1.7.104 id 51360 (+0x325), is the only caller of
//    BGSEntryPoint::HandleEntryPoint (23073 / 23526) with entry 0x4C, Mod Tempering Health: it passes the improvement
//    above fHealthDataValue1 (the computed result minus that setting), adds the setting back, rounds to 0.1 and stores
//    the result in the list entry (+0x1C) - the value the menu shows and applies;
//  - HitData::Populate (42832 / 44001) calls 42844, which multiplies the hit's physical damage by the aggressor's
//    actor value 0x9A, AttackDamageMult;
//  - fHealthDataValue1..6 default to 1.1 / 1.2 / 1.3 / 1.4 / 1.5 / 1.6 (Skyrim.esm sets fHealthDataValue1 = 1.1), and the
//    temper formula (25914) runs from fHealthDataValue1 to fHealthDataValue6.

namespace Smithing
{
	namespace
	{
		constexpr float kNormal = 1.0F;
		constexpr float kEps = 0.0001F;

		std::atomic<bool> g_temperHook{ false };
		std::atomic<bool> g_furnitureHook{ false };
		std::atomic<std::uint32_t> g_sharpens{ 0 };
		std::atomic<std::uint32_t> g_hitsWorn{ 0 };
		std::atomic<std::uint32_t> g_edgesLost{ 0 };
		std::atomic<std::uint32_t> g_capped{ 0 };             // improvements held at Fine, all session
		std::atomic<std::uint32_t> g_cappedThisMenu{ 0 };     // since the crafting menu last closed
		RE::SpellItem* g_sharpened = nullptr;
		float g_appliedMagnitude = -1.0F;                      // what the ability carried when it was last added
		RE::BGSKeyword* g_material[settings::smithing::kMaterialCount][2]{};

		std::mutex g_snapLock;
		Snapshot g_snap;

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

		float GameFloat(const char* a_name, float a_fallback)
		{
			auto* gmst = RE::GameSettingCollection::GetSingleton();
			auto* s = gmst ? gmst->GetSetting(a_name) : nullptr;
			if (!s)
			{
				static std::atomic<bool> warned{ false };
				if (!warned.exchange(true)) { logger::warn("smithing: game setting {} not found - using {}", a_name, a_fallback); }
				return a_fallback;
			}
			return s->GetFloat();
		}

		// The health the game names Fine (the first quality) and one quality level's width.
		float Fine() { return GameFloat("fHealthDataValue1", 1.1F); }
		float Step()
		{
			const float s = GameFloat("fHealthDataValue2", 1.2F) - Fine();
			return s > kEps ? s : 0.1F;   // a list that set the two equal: one tenth, the game's own rounding grid
		}

		bool IsMelee(const RE::TESObjectWEAP* a_w)
		{
			if (!a_w) { return false; }
			using T = RE::WEAPON_TYPE;
			switch (a_w->GetWeaponType())
			{
			case T::kOneHandSword:
			case T::kOneHandDagger:
			case T::kOneHandAxe:
			case T::kOneHandMace:
			case T::kTwoHandSword:
			case T::kTwoHandAxe:
				return true;
			default:
				return false;
			}
		}

		RE::TESObjectWEAP* HandWeapon(bool a_left)
		{
			auto* p = Player();
			auto* f = p ? p->GetEquippedObject(a_left) : nullptr;
			return f ? f->As<RE::TESObjectWEAP>() : nullptr;
		}

		// The extra list of the equipped instance of a_w in that hand (a worn item always has one: it carries ExtraWorn).
		RE::ExtraDataList* WornList(RE::TESObjectWEAP* a_w, bool a_left)
		{
			auto* p = Player();
			auto* changes = p ? p->GetInventoryChanges() : nullptr;
			if (!a_w || !changes || !changes->entryList) { return nullptr; }
			const auto want = a_left ? RE::ExtraDataType::kWornLeft : RE::ExtraDataType::kWorn;
			for (auto* entry : *changes->entryList)
			{
				if (!entry || entry->object != a_w || !entry->extraLists) { continue; }
				for (auto* x : *entry->extraLists)
				{
					if (x && x->HasType(want)) { return x; }
				}
			}
			return nullptr;
		}

		float HealthOf(RE::ExtraDataList* a_x)
		{
			auto* h = a_x ? a_x->GetByType<RE::ExtraHealth>() : nullptr;
			return h ? h->health : kNormal;
		}

		void SetHealth(RE::ExtraDataList* a_x, float a_health)
		{
			if (!a_x) { return; }
			if (auto* h = a_x->GetByType<RE::ExtraHealth>()) { h->health = a_health; }
			else { a_x->Add(new RE::ExtraHealth(a_health)); }
		}

		int MaterialRow(const RE::TESObjectWEAP* a_w)
		{
			if (!a_w) { return -1; }
			for (int i = 0; i < settings::smithing::kMaterialCount - 1; ++i)
			{
				for (auto* kw : g_material[i])
				{
					if (kw && a_w->HasKeyword(kw)) { return i; }
				}
			}
			return settings::smithing::kMaterialCount - 1;   // Other
		}

		void ResolveMaterials()
		{
			struct Row { const char* plugin; RE::FormID id; };
			// The vanilla weapon material keywords (looked up in the load order 2026-11-10, Skyrim.esm / Dawnguard.esm /
			// Dragonborn.esm), in the table's order. The honed Draugr and Falmer weapons carry their own keywords.
			static constexpr Row kRows[settings::smithing::kMaterialCount - 1][2] = {
				{ { "Skyrim.esm", 0x01E717 }, {} },                              // WeapMaterialWood
				{ { "Skyrim.esm", 0x01E718 }, {} },                              // WeapMaterialIron
				{ { "Skyrim.esm", 0x01E719 }, {} },                              // WeapMaterialSteel
				{ { "Skyrim.esm", 0x10AA1A }, {} },                              // WeapMaterialSilver
				{ { "Skyrim.esm", 0x0C5C00 }, {} },                              // WeapMaterialImperial
				{ { "Skyrim.esm", 0x0C5C01 }, { "Skyrim.esm", 0x0C5C02 } },      // WeapMaterialDraugr, WeapMaterialDraugrHoned
				{ { "Skyrim.esm", 0x01E71C }, {} },                              // WeapMaterialOrcish
				{ { "Skyrim.esm", 0x01E71A }, {} },                              // WeapMaterialDwarven
				{ { "Skyrim.esm", 0x01E71B }, {} },                              // WeapMaterialElven
				{ { "Dragonborn.esm", 0x026230 }, {} },                          // DLC2WeaponMaterialNordic
				{ { "Skyrim.esm", 0x0C5C03 }, { "Skyrim.esm", 0x0C5C04 } },      // WeapMaterialFalmer, WeapMaterialFalmerHoned
				{ { "Skyrim.esm", 0x01E71D }, {} },                              // WeapMaterialGlass
				{ { "Skyrim.esm", 0x01E71E }, {} },                              // WeapMaterialEbony
				{ { "Dragonborn.esm", 0x02622F }, {} },                          // DLC2WeaponMaterialStalhrim
				{ { "Dawnguard.esm", 0x019822 }, {} },                           // DLC1WeapMaterialDragonbone
				{ { "Skyrim.esm", 0x01E71F }, {} },                              // WeapMaterialDaedric
			};
			auto* handler = RE::TESDataHandler::GetSingleton();
			int found = 0;
			for (int i = 0; i < settings::smithing::kMaterialCount - 1; ++i)
			{
				for (int k = 0; k < 2; ++k)
				{
					const auto& r = kRows[i][k];
					if (!r.plugin) { continue; }
					g_material[i][k] = handler ? handler->LookupForm<RE::BGSKeyword>(r.id, r.plugin) : nullptr;
					if (g_material[i][k]) { ++found; }
					else { logger::warn("smithing: material keyword {}|{:06X} ({}) not found - that row never matches", r.plugin, r.id, settings::smithing::kMaterialIni[i]); }
				}
			}
			logger::info("smithing: {} weapon material keywords resolved for the wear table", found);
		}

		void ResolveSpell()
		{
			if (g_sharpened) { return; }
			try
			{
				std::ifstream in(std::filesystem::path("Data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/smithing.json"));
				const auto j = in ? nlohmann::json::parse(in) : nlohmann::json::object();
				const auto ref = j.value("sharpenedSpell", std::string());
				const auto bar = ref.find('|');
				if (bar == std::string::npos) { logger::error("smithing: smithing.json has no sharpenedSpell - the sharpening bonus is off"); return; }
				const auto plugin = ref.substr(0, bar);
				const auto id = static_cast<RE::FormID>(std::stoul(ref.substr(bar + 1), nullptr, 16));
				auto* handler = RE::TESDataHandler::GetSingleton();
				g_sharpened = handler ? handler->LookupForm<RE::SpellItem>(id, plugin) : nullptr;
				if (!g_sharpened) { logger::error("smithing: the Sharpened spell {} is not loaded - enable NjordlingerCombatGameplayOverhaul.esp", ref); }
				else if (g_sharpened->effects.empty() || !g_sharpened->effects[0]) { logger::error("smithing: the Sharpened spell has no effect"); g_sharpened = nullptr; }
				else { logger::info("smithing: Sharpened spell {:08X} resolved", g_sharpened->GetFormID()); }
			}
			catch (const std::exception& e) { logger::error("smithing: smithing.json could not be read ({})", e.what()); }
		}

		void UpdateSnapshot()
		{
			Snapshot s;
			auto* w = HandWeapon(false);
			if (w)
			{
				s.weapon = w->GetName() ? w->GetName() : "";
				s.melee = IsMelee(w);
				s.health = HealthOf(WornList(w, false));
				s.material = MaterialRow(w);
			}
			auto* p = Player();
			s.sharpenedActive = p && g_sharpened && p->HasSpell(g_sharpened);
			s.fine = Fine();
			s.step = Step();
			s.temperHook = g_temperHook.load();
			s.furnitureHook = g_furnitureHook.load();
			s.sharpens = g_sharpens.load();
			s.hitsWorn = g_hitsWorn.load();
			s.edgesLost = g_edgesLost.load();
			s.capped = g_capped.load();
			std::scoped_lock l(g_snapLock);
			g_snap = std::move(s);
		}

		// Main thread: the Sharpened ability is carried exactly while Sharpening mode is on and the right-hand melee
		// weapon's tempering is above normal.
		void Reconcile()
		{
			auto* p = Player();
			if (!p) { return; }
			ResolveSpell();
			if (!g_sharpened) { UpdateSnapshot(); return; }
			auto* w = HandWeapon(false);
			const float bonus = settings::smithing::sharpenBonus;
			const bool want = settings::smithing::temperingMode == 2 && bonus > 0.0F && IsMelee(w) && HealthOf(WornList(w, false)) > kNormal + kEps;
			const bool has = p->HasSpell(g_sharpened);
			if (want)
			{
				if (!has || std::fabs(g_appliedMagnitude - bonus) > kEps)
				{
					if (has) { p->RemoveSpell(g_sharpened); }
					g_sharpened->effects[0]->effectItem.magnitude = bonus;
					p->AddSpell(g_sharpened);
					g_appliedMagnitude = bonus;
					logger::debug("smithing: Sharpened on (+{:.2f} attack damage multiplier, {})", bonus, w && w->GetName() ? w->GetName() : "?");
				}
			}
			else if (has)
			{
				p->RemoveSpell(g_sharpened);
				logger::debug("smithing: Sharpened off (mode {}, weapon {}, sharp {})", settings::smithing::temperingMode, w && w->GetName() ? w->GetName() : "none",
					w ? HealthOf(WornList(w, false)) : 0.0F);
			}
			UpdateSnapshot();
		}

		void Queue(std::function<void()> a_task)
		{
			if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask(std::move(a_task)); }
		}

		std::string Format(const char* a_fmt, const char* a_name, float a_pct = 0.0F)
		{
			char buf[512];
			if (std::strstr(a_fmt, "%.0f")) { std::snprintf(buf, sizeof(buf), a_fmt, a_name, a_pct); }
			else { std::snprintf(buf, sizeof(buf), a_fmt, a_name); }
			return buf;
		}

		// Main thread: the grindstone in Sharpening mode (or op=smithing sharpen).
		void Sharpen()
		{
			auto* p = Player();
			if (!p) { return; }
			if (settings::classes::enabled && settings::classes::craftingGate && !Classes::HoldsGroup(ClassData::kWarrior))
			{
				const char* notice = strings::TR("NJ_NoticeNoSmithing", "Your class does not know the smith's craft.");
				notify::Show(notice);
				logger::info("sharpening: refused - the class does not hold the warrior group");
				return;
			}
			auto* w = HandWeapon(false);
			auto* x = IsMelee(w) ? WornList(w, false) : nullptr;
			if (!x)
			{
				notify::Show(strings::TR("NJ_NoticeNoBlade", "You have no blade to sharpen."));
				logger::info("sharpening: no melee weapon in the right hand ({})", w && w->GetName() ? w->GetName() : "none");
				return;
			}
			const char* name = w->GetName() ? w->GetName() : "";
			const float fine = Fine();
			const float before = HealthOf(x);
			if (before >= fine - kEps)
			{
				const auto text = Format(strings::TR("NJ_NoticeAlreadySharp", "Your %s is already sharp."), name);
				notify::Show(text.c_str());
				logger::info("sharpening: {} is already at {:.3f} (Fine is {:.2f}) - nothing changed", name, before, fine);
				Reconcile();
				return;
			}
			SetHealth(x, fine);
			RE::PlaySound("UISmithingImproveWeapon");
			++g_sharpens;
			const auto text = Format(strings::TR("NJ_NoticeSharpened", "Your %s is sharpened: +%.0f%% to damage until the edge wears down."), name, settings::smithing::sharpenBonus * 100.0F);
			notify::Show(text.c_str());
			logger::info("sharpening: {} {:.3f} -> {:.3f} (Fine)", name, before, fine);
			Reconcile();
		}

		// Main thread: one landed hit with a_w in that hand.
		void Wear(RE::TESObjectWEAP* a_w, bool a_left)
		{
			auto* x = WornList(a_w, a_left);
			const float h = HealthOf(x);
			if (!x || h <= kNormal + kEps) { return; }
			const int row = MaterialRow(a_w);
			const int hits = row >= 0 ? std::max(1, settings::smithing::hits[row]) : 30;
			const float next = std::max(kNormal, h - Step() / static_cast<float>(hits));
			SetHealth(x, next);
			++g_hitsWorn;
			const char* name = a_w->GetName() ? a_w->GetName() : "";
			logger::debug("wear: {} ({} hand, {}, {} hits a level) {:.4f} -> {:.4f}", name, a_left ? "left" : "right", row >= 0 ? settings::smithing::kMaterialIni[row] : "?", hits, h, next);
			if (next <= kNormal + kEps)
			{
				++g_edgesLost;
				const auto text = Format(strings::TR("NJ_NoticeEdgeLost", "Your %s has lost its edge."), name);
				notify::Show(text.c_str());
				logger::info("wear: {} is back to normal", name);
			}
			Reconcile();
		}

		// ---- mode 1: the improvement held at Fine -----------------------------------------------------------------
		using HandleEntryPoint_t = void (*)(std::uint32_t, RE::Actor*, RE::TESForm*, float*);
		HandleEntryPoint_t g_handleEntryPoint = nullptr;

		void TemperingHealth(std::uint32_t a_entry, RE::Actor* a_owner, RE::TESForm* a_item, float* a_delta)
		{
			g_handleEntryPoint(a_entry, a_owner, a_item, a_delta);
			if (settings::smithing::temperingMode != 1 || !a_delta) { return; }
			// The menu adds fHealthDataValue1 back and rounds to 0.1, so the cap is the highest tenth below the second
			// quality: Fine itself with the game's values.
			const float fine = Fine();
			float cap = std::floor((GameFloat("fHealthDataValue2", 1.2F) - 0.001F) * 10.0F + 0.0001F) / 10.0F;
			if (cap < fine) { cap = fine; }
			const float most = cap - fine;
			if (*a_delta > most + kEps)
			{
				logger::debug("first level only: {} would temper to {:.2f}, held at {:.2f}", a_item && a_item->GetName() ? a_item->GetName() : "?", fine + *a_delta, cap);
				*a_delta = most;
				++g_capped;
				++g_cappedThisMenu;
			}
		}

		void InstallTemperCap()
		{
			const auto fn = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(50296, 51360) }.address();
			const auto hep = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(23073, 23526) }.address();
			const auto base = REL::Module::get().base();
			// mov ecx, 0x4C ; call HandleEntryPoint - found by shape inside the list builder rather than at a fixed offset
			// (1.5.97 +0x153, 1.7.104 +0x325), so a build whose layout moved still attaches or refuses cleanly.
			std::uintptr_t site = 0;
			for (std::uintptr_t a = fn; a < fn + 0x600; ++a)
			{
				const auto* b = reinterpret_cast<const std::uint8_t*>(a);
				if (b[0] == 0xB9 && b[1] == 0x4C && b[2] == 0 && b[3] == 0 && b[4] == 0 && b[5] == 0xE8)
				{
					const auto target = a + 10 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(b + 6));
					if (target == hep) { site = a + 5; break; }
					logger::warn("first level only: the Mod Tempering Health call at 0x{:X} goes to 0x{:X}, not the game's 0x{:X} - another plugin holds it; the cap is not attached",
						a + 5 - base, target - base, hep - base);
					return;
				}
			}
			if (!site) { logger::warn("first level only: the Mod Tempering Health call was not found in the smithing list builder (0x{:X}) - the cap is not attached", fn - base); return; }
			g_handleEntryPoint = reinterpret_cast<HandleEntryPoint_t>(hep);
			SKSE::GetTrampoline().write_call<5>(site, reinterpret_cast<std::uintptr_t>(&TemperingHealth));
			g_temperHook = true;
			logger::info("first level only: attached at game offset 0x{:X} (the smithing list's Mod Tempering Health call)", site - base);
		}

		// ---- mode 2: the grindstone ----------------------------------------------------------------------------------
		struct FurnitureHook
		{
			static bool Activate(RE::TESFurniture* a_this, RE::TESObjectREFR* a_target, RE::TESObjectREFR* a_activator, std::uint8_t a_arg3, RE::TESBoundObject* a_object, std::int32_t a_count)
			{
				if (settings::smithing::temperingMode == 2 && a_this && a_activator && a_activator->IsPlayerRef() &&
					a_this->workBenchData.benchType.get() == RE::TESFurniture::WorkBenchData::BenchType::kSmithingWeapon)
				{
					logger::debug("sharpening: grindstone {:08X} activated", a_target ? a_target->GetFormID() : 0);
					Sharpen();
					return true;
				}
				return _original(a_this, a_target, a_activator, a_arg3, a_object, a_count);
			}
			static inline REL::Relocation<decltype(Activate)> _original;
		};

		// ---- sinks ----------------------------------------------------------------------------------------------------
		class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_e, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!settings::smithing::edgeWear || !a_e || !a_e->cause || !a_e->cause->IsPlayerRef() || a_e->projectile != 0) { return RE::BSEventNotifyControl::kContinue; }
				if (a_e->flags.any(RE::TESHitEvent::Flag::kBashAttack)) { return RE::BSEventNotifyControl::kContinue; }
				const auto source = a_e->source;
				Queue([source]() {
					auto* w = RE::TESForm::LookupByID<RE::TESObjectWEAP>(source);
					if (!IsMelee(w)) { return; }
					// the hand that holds it: right first (the same weapon in both hands wears the right one)
					if (HandWeapon(false) == w) { Wear(w, false); }
					else if (HandWeapon(true) == w) { Wear(w, true); }
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		HitSink g_hits;

		class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_e, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (a_e && a_e->actor && a_e->actor->IsPlayerRef()) { Queue([]() { Reconcile(); }); }
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		EquipSink g_equip;

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_e || a_e->menuName != RE::CraftingMenu::MENU_NAME) { return RE::BSEventNotifyControl::kContinue; }
				if (!a_e->opening) { g_cappedThisMenu = 0; return RE::BSEventNotifyControl::kContinue; }
				if (settings::smithing::temperingMode != 1) { return RE::BSEventNotifyControl::kContinue; }
				// the list is built around the open; read the count a moment later on the main thread
				std::thread([]() {
					std::this_thread::sleep_for(std::chrono::milliseconds(300));
					Queue([]() {
						const auto n = g_cappedThisMenu.load();
						if (n == 0) { return; }
						notify::Show(strings::TR("NJ_NoticeFirstLevelOnly", "Tempering stops at Fine: an item already Fine or better cannot be improved further."));
						logger::info("first level only: {} item(s) in this smithing list held at Fine", n);
					});
				}).detach();
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MenuSink g_menu;
	}

	void Install()
	{
		ResolveMaterials();
		ResolveSpell();
		InstallTemperCap();
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_TESFurniture[0] };
			FurnitureHook::_original = vtbl.write_vfunc(0x37, FurnitureHook::Activate);
			g_furnitureHook = true;
			logger::info("sharpening: TESFurniture::Activate hooked (vtable slot 0x37)");
		}
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton())
		{
			holder->AddEventSink<RE::TESHitEvent>(&g_hits);
			holder->AddEventSink<RE::TESEquipEvent>(&g_equip);
			logger::info("smithing: hit and equip sinks registered");
		}
		else { logger::error("smithing: no script event source - edges do not wear and the bonus is not re-checked on equip"); }
		if (auto* ui = RE::UI::GetSingleton()) { ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menu); }
		logger::info("smithing: tempering mode {}, sharpening bonus {:.2f}, edge wear {}; Fine = {:.2f}, one level = {:.2f}",
			settings::smithing::temperingMode, settings::smithing::sharpenBonus, settings::smithing::edgeWear, Fine(), Step());
	}

	void OnGameLoaded()
	{
		g_appliedMagnitude = -1.0F;   // the ability came back from the save with the record's magnitude: add it afresh
		Queue([]() {
			auto* p = Player();
			if (p && g_sharpened && p->HasSpell(g_sharpened)) { p->RemoveSpell(g_sharpened); }
			Reconcile();
		});
	}

	void RequestApply() { Queue([]() { Reconcile(); }); }

	void RequestSharpen() { Queue([]() { Sharpen(); }); }

	void RequestWearHits(int a_hits)
	{
		Queue([a_hits]() {
			auto* w = HandWeapon(false);
			if (!IsMelee(w)) { logger::info("wear (DevBench): no melee weapon in the right hand"); return; }
			for (int i = 0; i < a_hits; ++i) { Wear(w, false); }
		});
	}

	Snapshot GetSnapshot()
	{
		std::scoped_lock l(g_snapLock);
		return g_snap;
	}

	std::string StatusJson()
	{
		const auto s = GetSnapshot();
		nlohmann::json hits = nlohmann::json::object();
		for (int i = 0; i < settings::smithing::kMaterialCount; ++i) { hits[settings::smithing::kMaterialIni[i]] = settings::smithing::hits[i]; }
		nlohmann::json j = {
			{ "mode", settings::smithing::temperingMode }, { "bonus", settings::smithing::sharpenBonus }, { "edgeWear", settings::smithing::edgeWear },
			{ "weapon", s.weapon }, { "melee", s.melee }, { "health", s.health },
			{ "material", s.material >= 0 ? settings::smithing::kMaterialIni[s.material] : "" },
			{ "sharpenedActive", s.sharpenedActive }, { "fine", s.fine }, { "step", s.step },
			{ "temperHook", s.temperHook }, { "furnitureHook", s.furnitureHook },
			{ "sharpens", s.sharpens }, { "hitsWorn", s.hitsWorn }, { "edgesLost", s.edgesLost }, { "capped", s.capped },
			{ "hitsPerLevel", hits }
		};
		return j.dump();
	}
}
