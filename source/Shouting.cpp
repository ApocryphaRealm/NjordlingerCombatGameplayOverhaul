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
		constexpr std::uint32_t kRecordVersion = 2;   // 1 = D26's {enabled}; 2 = D27's {mode}

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
		std::atomic<int> g_mode{ kAuto };       // this save
		std::atomic<bool> g_holdsMagic{ true };  // published by the class module
		std::atomic<bool> g_haveRecord{ false };
		std::atomic<std::uint32_t> g_souls{ 0 }, g_points{ 0 }, g_refusals{ 0 };
		std::atomic<bool> g_reserved{ false };
		std::atomic<bool> g_toldThisSession{ false };
		std::atomic<bool> g_running{ false };
		int g_soulsPerPoint = 1;
		std::string g_error;

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

		bool Allowed()
		{
			const int m = g_mode.load();
			return m == kOn || (m == kAuto && g_holdsMagic.load());
		}

		const char* ModeName(int a_mode) { return a_mode == kOn ? "On" : a_mode == kOff ? "Off" : "Auto"; }

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
					logger::info("shouting: the shout key refused for {} (Shouting {} for this save)", shout->GetName(), ModeName(g_mode.load()));
				}
				return false;
			}
			static inline REL::Relocation<decltype(CanProcess)> _original;
		};

		int ReadSoulsPerPoint()
		{
			std::ifstream in(std::filesystem::path("Data/SKSE/Plugins/SoulsToPerks.ini"));
			std::string line;
			while (in && std::getline(in, line))
			{
				const auto eq = line.find('=');
				if (eq == std::string::npos) { continue; }
				std::string key = line.substr(0, eq);
				key.erase(0, key.find_first_not_of(" \t"));
				key.erase(key.find_last_not_of(" \t") + 1);
				if (_stricmp(key.c_str(), "uSoulsPerPoint") == 0)
				{
					try { return std::max(1, std::stoi(line.substr(eq + 1))); } catch (...) { return 1; }
				}
			}
			return 1;
		}

		// Main thread: while Shouting is off, the player's dragon souls become perk points.
		void Convert()
		{
			auto* p = Player();
			if (!p || Allowed()) { g_reserved = false; return; }
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
			const int points = souls / g_soulsPerPoint;
			if (points <= 0) { return; }
			const int take = points * g_soulsPerPoint;
			avo->SetActorValue(RE::ActorValue::kDragonSouls, static_cast<float>(souls - take));   // the soul count is a base value; the same call on both build lines
			auto& stats = p->GetGameStatsData();
			const int before = static_cast<int>(stats.perkCount);
			stats.perkCount = static_cast<decltype(stats.perkCount)>(std::clamp(before + points, 0, 127));
			g_souls += static_cast<std::uint32_t>(take);
			g_points += static_cast<std::uint32_t>(points);
			logger::info("shouting: {} dragon soul(s) became {} perk point(s) ({} -> {})", take, points, before, static_cast<int>(stats.perkCount));
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
		g_soulsPerPoint = ReadSoulsPerPoint();
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_ShoutHandler[0] };
		ShoutHook::_original = vtbl.write_vfunc(0x1, ShoutHook::CanProcess);
		g_loaded = true;
		if (!g_running.exchange(true)) { std::thread(Worker).detach(); }
		logger::info("shouting: the shout key is guarded; {} quest row(s); {} soul(s) per perk point; new saves start with Shouting {}", g_rows.size(), g_soulsPerPoint, ModeName(settings::shouting::mode));
	}

	void OnGameLoaded()
	{
		g_toldThisSession = false;
		if (!g_haveRecord.load()) { g_mode = settings::shouting::mode; }
		logger::info("shouting: this save - Shouting {} (the class holds magic: {}; resolved {}); {} soul(s) converted so far", ModeName(g_mode.load()), g_holdsMagic.load(), Allowed() ? "on" : "off", g_souls.load());
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		const std::string text = nlohmann::json{ { "mode", g_mode.load() }, { "souls", g_souls.load() }, { "points", g_points.load() } }.dump();
		const auto size = static_cast<std::uint32_t>(text.size());
		if (!a_intfc || !a_intfc->OpenRecord(kRecord, kRecordVersion)) { logger::error("shouting: could not open the co-save record"); return; }
		a_intfc->WriteRecordData(&size, sizeof(size));
		a_intfc->WriteRecordData(text.data(), size);
	}

	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
	{
		if (!a_intfc || (a_version != 1 && a_version != kRecordVersion) || a_length < sizeof(std::uint32_t)) { return; }
		std::uint32_t size = 0;
		if (!a_intfc->ReadRecordData(&size, sizeof(size)) || size != a_length - sizeof(size)) { return; }
		std::string text(size, '\0');
		if (size && !a_intfc->ReadRecordData(text.data(), size)) { return; }
		try
		{
			const auto j = nlohmann::json::parse(text);
			if (j.contains("mode")) { g_mode = std::clamp(j.value("mode", 0), 0, 2); }
			else { g_mode = j.value("enabled", false) ? kOn : kAuto; }   // a D26 save: on stays On, off becomes Auto
			g_souls = j.value("souls", 0u);
			g_points = j.value("points", 0u);
			g_haveRecord = true;
		}
		catch (...) { logger::warn("shouting: the co-save record is not valid - this save uses the default"); }
	}

	void OnRevert()
	{
		g_haveRecord = false;
		g_mode = settings::shouting::mode;
		g_holdsMagic = true;
		g_souls = 0;
		g_points = 0;
	}

	State GetState()
	{
		State s;
		s.loaded = g_loaded.load();
		s.error = g_error;
		s.mode = g_mode.load();
		s.holdsMagic = g_holdsMagic.load();
		s.enabled = Allowed();
		s.soulsConverted = g_souls.load();
		s.pointsGiven = g_points.load();
		s.soulsPerPoint = g_soulsPerPoint;
		s.reserved = g_reserved.load();
		s.refusals = g_refusals.load();
		s.rows = static_cast<std::uint32_t>(g_rows.size());
		return s;
	}

	void RequestSetMode(int a_mode)
	{
		g_mode = std::clamp(a_mode, 0, 2);
		g_haveRecord = true;
		logger::info("shouting: this save - Shouting set to {} (resolved {})", ModeName(g_mode.load()), Allowed() ? "on" : "off");
	}

	void SetHoldsMagic(bool a_holds)
	{
		const bool was = g_holdsMagic.exchange(a_holds);
		if (was != a_holds) { logger::info("shouting: the class {} the magic group - Auto resolves {}", a_holds ? "holds" : "does not hold", a_holds ? "on" : "off"); }
	}

	std::string StatusJson()
	{
		const auto s = GetState();
		return nlohmann::json{ { "loaded", s.loaded }, { "error", s.error }, { "mode", ModeName(s.mode) }, { "holdsMagic", s.holdsMagic }, { "enabled", s.enabled }, { "soulsConverted", s.soulsConverted },
			{ "pointsGiven", s.pointsGiven }, { "soulsPerPoint", s.soulsPerPoint }, { "reserved", s.reserved }, { "refusals", s.refusals }, { "rows", s.rows } }.dump();
	}
}
