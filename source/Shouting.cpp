#include "PCH.h"

#include "Shouting.h"

#include "Settings.h"
#include "SoulStones.h"

#include "utils/Logger.h"
#include "utils/Notify.h"
#include "utils/Strings.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace Shouting
{
	namespace
	{
		constexpr std::uint32_t kRecordVersion = 3;   // 1 = D26's {enabled}; 2 = D27's {mode}; 3 = {enabled, autoConvert}

		struct Row
		{
			RE::TESQuest* quest = nullptr;
			std::uint16_t from = 0, to = 0;
			RE::TESShout* shout = nullptr;
			bool reserveSoul = false;
			std::string name;
		};
		std::vector<Row> g_rows;

		std::atomic<bool> g_loaded{ false };
		std::atomic<bool> g_enabled{ false };   // this save can shout
		std::atomic<bool> g_autoConvert{ false };   // this save converts souls as they arrive
		std::atomic<bool> g_stpConverts{ false };   // Souls to Perks' bAutoConvert=1: it is the converter
		std::atomic<bool> g_haveRecord{ false };
		std::atomic<std::uint32_t> g_souls{ 0 }, g_points{ 0 }, g_refusals{ 0 };
		std::atomic<bool> g_reserved{ false };
		std::atomic<bool> g_toldThisSession{ false };
		std::atomic<bool> g_running{ false };
		int g_soulsPerPoint = 1;
		std::string g_error;

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

		bool Allowed() { return g_enabled.load(); }

		RE::TESForm* Resolve(const std::string& a_ref)
		{
			const auto bar = a_ref.find('|');
			auto* handler = RE::TESDataHandler::GetSingleton();
			if (bar == std::string::npos || !handler) { return nullptr; }
			try { return handler->LookupForm(static_cast<RE::FormID>(std::stoul(a_ref.substr(bar + 1), nullptr, 0)) & 0xFFFFFF, a_ref.substr(0, bar)); }
			catch (...) { return nullptr; }
		}

		// Is the Shout on the player's shout key one Shouting-off still lets through?
		bool Exempt(RE::TESShout* a_shout)
		{
			if (!a_shout) { return true; }   // a power, or nothing: not ours to refuse
			if (SoulStones::IsStoneShout(a_shout)) { return true; }
			for (const auto& r : g_rows)
			{
				if (r.shout != a_shout || !r.quest || !r.quest->IsRunning() || r.quest->IsCompleted()) { continue; }
				const auto stage = r.quest->GetCurrentStageID();
				if (stage >= r.from && stage <= r.to) { return true; }
			}
			return false;
		}

		struct ShoutHook
		{
			static bool CanProcess(RE::ShoutHandler* a_this, RE::InputEvent* a_event)
			{
				if (!_original(a_this, a_event)) { return false; }
				if (Allowed() || !g_loaded.load()) { return true; }
				auto* p = Player();
				auto* power = p ? p->GetActorRuntimeData().selectedPower : nullptr;
				auto* shout = power ? power->As<RE::TESShout>() : nullptr;
				if (Exempt(shout)) { return true; }
				++g_refusals;
				if (!g_toldThisSession.exchange(true))
				{
					notify::Show(strings::TR("NJ_ShoutingSilent", "Your Voice is silent; the words live in your stones."));
					logger::info("shouting: the shout key refused for {} (Shouting is off for this save)", shout->GetName());
				}
				return false;
			}
			static inline REL::Relocation<decltype(CanProcess)> _original;
		};

		// Souls to Perks' INI: its rate, and whether it converts by itself.
		int ReadSoulsPerPoint(bool& a_stpAuto)
		{
			a_stpAuto = false;
			int rate = 1;
			std::ifstream in(std::filesystem::path("Data/SKSE/Plugins/SoulsToPerks.ini"));
			std::string line;
			while (in && std::getline(in, line))
			{
				const auto eq = line.find('=');
				if (eq == std::string::npos || line.find(';') < eq) { continue; }
				std::string key = line.substr(0, eq);
				key.erase(0, key.find_first_not_of(" \t"));
				key.erase(key.find_last_not_of(" \t") + 1);
				std::string v = line.substr(eq + 1);
				v.erase(0, v.find_first_not_of(" \t"));
				if (_stricmp(key.c_str(), "uSoulsPerPoint") == 0) { try { rate = std::max(1, std::stoi(v)); } catch (...) {} }
				if (_stricmp(key.c_str(), "bAutoConvert") == 0) { a_stpAuto = v.rfind("1", 0) == 0 || v.rfind("true", 0) == 0; }
			}
			return rate;
		}

		// Main thread: with this save's auto-convert on (and Souls to Perks not converting itself), the player's dragon souls
		// become perk points as they arrive.
		void Convert()
		{
			auto* p = Player();
			if (!p || !g_autoConvert.load() || g_stpConverts.load()) { g_reserved = false; return; }
			bool reserved = false;
			for (const auto& r : g_rows)
			{
				if (r.reserveSoul && r.quest && !r.quest->IsCompleted()) { reserved = true; }
			}
			g_reserved = reserved;
			if (reserved) { return; }
			auto* avo = p->AsActorValueOwner();
			if (!avo) { return; }
			const int souls = static_cast<int>(avo->GetActorValue(RE::ActorValue::kDragonSouls));
			int points = souls / g_soulsPerPoint;
			if (points <= 0) { return; }
			const int take = points * g_soulsPerPoint;
			// 6616: souls given by modav (and by absorption) sit in the PERMANENT MODIFIER, not the base, so lowering the base
			// left the total unchanged and the same souls were paid out every second. Take them off the permanent modifier (the
			// same vfunc on both build lines) and pay only what the total really lost.
#if RUNTIME_LINE == 17
			avo->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kDragonSouls, -static_cast<float>(take));
#else
			avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kDragonSouls, -static_cast<float>(take));
#endif
			int after = static_cast<int>(avo->GetActorValue(RE::ActorValue::kDragonSouls));
			if (souls - after < take)
			{
				// the modifier could not hold the whole take (souls in the base): take the rest from the base
				const float base = avo->GetBaseActorValue(RE::ActorValue::kDragonSouls);
				avo->SetBaseActorValue(RE::ActorValue::kDragonSouls, std::max(0.0F, base - static_cast<float>(take - (souls - after))));
				after = static_cast<int>(avo->GetActorValue(RE::ActorValue::kDragonSouls));
			}
			const int lost = std::clamp(souls - after, 0, take);
			points = lost / g_soulsPerPoint;
			if (points <= 0)
			{
				logger::warn("shouting: the dragon soul count did not go down ({} -> {}) - no perk points given", souls, after);
				return;
			}
			auto& stats = p->GetGameStatsData();
			const int before = static_cast<int>(stats.perkCount);
			stats.perkCount = static_cast<decltype(stats.perkCount)>(std::clamp(before + points, 0, 127));
			g_souls += static_cast<std::uint32_t>(points * g_soulsPerPoint);
			g_points += static_cast<std::uint32_t>(points);
			logger::info("shouting: {} dragon soul(s) became {} perk point(s) ({} -> {})", points * g_soulsPerPoint, points, before, static_cast<int>(stats.perkCount));
			std::string text = strings::TR("NJ_NoticeSoulPoints", "Dragon souls became perk points: {}");
			try { text = std::vformat(text, std::make_format_args(points)); } catch (...) {}
			notify::Show(text.c_str());
		}

		void Worker()
		{
			while (g_running)
			{
				std::this_thread::sleep_for(std::chrono::seconds(1));
				if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { Convert(); }); }
			}
		}
	}

	void Install()
	{
		try
		{
			std::ifstream in(std::filesystem::path("Data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/shouting.json"));
			if (!in) { g_error = "shouting.json is missing - no quest keeps its Shout while Shouting is off"; }
			else
			{
				const auto j = nlohmann::json::parse(in);
				for (const auto& r : j.value("rows", nlohmann::json::array()))
				{
					Row row;
					row.quest = Resolve(r.value("quest", std::string())) ? Resolve(r.value("quest", std::string()))->As<RE::TESQuest>() : nullptr;
					row.shout = Resolve(r.value("shout", std::string())) ? Resolve(r.value("shout", std::string()))->As<RE::TESShout>() : nullptr;
					row.from = static_cast<std::uint16_t>(r.value("from", 0));
					row.to = static_cast<std::uint16_t>(r.value("to", 0));
					row.reserveSoul = r.value("reserveSoul", false);
					row.name = r.value("shoutName", std::string()) + " / " + r.value("questName", std::string());
					if (!row.quest || !row.shout) { logger::warn("shouting: row {} does not resolve - skipped", row.name); continue; }
					g_rows.push_back(row);
				}
			}
		}
		catch (const std::exception& e) { g_error = std::string("shouting.json is not valid: ") + e.what(); }
		if (!g_error.empty()) { logger::warn("shouting: {}", g_error); }
		bool stpAuto = false;
		g_soulsPerPoint = ReadSoulsPerPoint(stpAuto);
		g_stpConverts = stpAuto;
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_ShoutHandler[0] };
		ShoutHook::_original = vtbl.write_vfunc(0x1, ShoutHook::CanProcess);
		g_loaded = true;
		if (!g_running.exchange(true)) { std::thread(Worker).detach(); }
		logger::info("shouting: the shout key is guarded; {} quest row(s); {} soul(s) per perk point; new saves start with Shouting {} and auto-convert {}; Souls to Perks converts by itself: {}", g_rows.size(), g_soulsPerPoint, settings::shouting::enabledDefault ? "on" : "off", settings::shouting::autoConvertDefault ? "on" : "off", g_stpConverts.load());
	}

	void OnGameLoaded()
	{
		g_toldThisSession = false;
		if (!g_haveRecord.load()) { g_enabled = settings::shouting::enabledDefault; g_autoConvert = settings::shouting::autoConvertDefault; }
		logger::info("shouting: this save - Shouting {}, auto-convert {}; {} soul(s) converted so far (kept)", g_enabled.load() ? "on" : "off", g_autoConvert.load() ? "on" : "off", g_souls.load());
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		const std::string text = nlohmann::json{ { "enabled", g_enabled.load() }, { "autoConvert", g_autoConvert.load() }, { "souls", g_souls.load() }, { "points", g_points.load() } }.dump();
		const auto size = static_cast<std::uint32_t>(text.size());
		if (!a_intfc || !a_intfc->OpenRecord(kRecord, kRecordVersion)) { logger::error("shouting: could not open the co-save record"); return; }
		a_intfc->WriteRecordData(&size, sizeof(size));
		a_intfc->WriteRecordData(text.data(), size);
	}

	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
	{
		if (!a_intfc || a_version < 1 || a_version > kRecordVersion || a_length < sizeof(std::uint32_t)) { return; }
		std::uint32_t size = 0;
		if (!a_intfc->ReadRecordData(&size, sizeof(size)) || size != a_length - sizeof(size)) { return; }
		std::string text(size, '\0');
		if (size && !a_intfc->ReadRecordData(text.data(), size)) { return; }
		try
		{
			const auto j = nlohmann::json::parse(text);
			// v2 (D27): mode 1 = On; Auto (0) and Off (2) read as Off. v1 (D26) converted while Off, so it keeps converting.
			if (j.contains("mode")) { g_enabled = j.value("mode", 0) == 1; }
			else { g_enabled = j.value("enabled", false); }
			g_autoConvert = j.contains("autoConvert") ? j.value("autoConvert", false) : (a_version <= 2 ? !g_enabled.load() && settings::shouting::autoConvertDefault : false);
			g_souls = j.value("souls", 0u);
			g_points = j.value("points", 0u);
			g_haveRecord = true;
		}
		catch (...) { logger::warn("shouting: the co-save record is not valid - this save uses the default"); }
	}

	void OnRevert()
	{
		g_haveRecord = false;
		g_enabled = settings::shouting::enabledDefault;
		g_autoConvert = settings::shouting::autoConvertDefault;
		g_souls = 0;
		g_points = 0;
	}

	State GetState()
	{
		State s;
		s.loaded = g_loaded.load();
		s.error = g_error;
		s.enabled = g_enabled.load();
		s.autoConvert = g_autoConvert.load();
		s.stpConverts = g_stpConverts.load();
		s.soulsConverted = g_souls.load();
		s.pointsGiven = g_points.load();
		s.soulsPerPoint = g_soulsPerPoint;
		s.reserved = g_reserved.load();
		s.refusals = g_refusals.load();
		s.rows = static_cast<std::uint32_t>(g_rows.size());
		return s;
	}

	void RequestSetEnabled(bool a_on)
	{
		g_enabled = a_on;
		g_haveRecord = true;
		logger::info("shouting: this save - Shouting switched {}", a_on ? "on" : "off");
	}

	void RequestSetAutoConvert(bool a_on)
	{
		g_autoConvert = a_on;
		g_haveRecord = true;
		logger::info("shouting: this save - auto-convert dragon souls switched {}{}", a_on ? "on" : "off", g_stpConverts.load() ? " (Souls to Perks converts by itself - this one stands aside)" : "");
	}

	std::string StatusJson()
	{
		const auto s = GetState();
		return nlohmann::json{ { "loaded", s.loaded }, { "error", s.error }, { "enabled", s.enabled }, { "autoConvert", s.autoConvert }, { "stpConverts", s.stpConverts }, { "soulsConverted", s.soulsConverted },
			{ "pointsGiven", s.pointsGiven }, { "soulsPerPoint", s.soulsPerPoint }, { "reserved", s.reserved }, { "refusals", s.refusals }, { "rows", s.rows } }.dump();
	}
}
