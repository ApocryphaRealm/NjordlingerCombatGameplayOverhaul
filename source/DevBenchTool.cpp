#include "PCH.h"

#include "DevBenchTool.h"

#include "ClassEffects.h"
#include "Classes.h"
#include "DevBench/DevBenchAPI.h"
#include "LevelUpGains.h"
#include "Lockouts.h"
#include "Patches.h"
#include "QuestRewards.h"
#include "RaceMenuClass.h"
#include "StatsMenuClass.h"
#include "Settings.h"
#include "UI.h"

#include "utils/Logger.h"
#include "utils/Strings.h"

#include <nlohmann/json.hpp>

#include <string>

// rules 31 and 64: the live state of every module, and the buttons of the Classes tab, driven over DevBench.
// The handler runs on DevBench's listener thread: it reads snapshots (mutex-guarded) and queues every game change
// to the main thread through the modules' Request* calls.
namespace DevBenchTool
{
	namespace
	{
		void Write(void* a_sink, DevBenchAPI::WriteFn a_write, const nlohmann::json& a_j)
		{
			const auto text = a_j.dump();
			a_write(a_sink, text.c_str());
		}

		void ControlTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			nlohmann::json args = nlohmann::json::object();
			try { if (a_argsJson && *a_argsJson) { args = nlohmann::json::parse(a_argsJson); } }
			catch (...) { Write(a_sink, a_write, { { "ok", false }, { "error", "arguments are not JSON" } }); return; }
			const std::string op = args.value("op", std::string("status"));

			if (op == "choose")
			{
				std::string why;
				const bool ok = Classes::RequestChooseStart(args.value("class", std::string()), why);
				Write(a_sink, a_write, { { "ok", ok }, { "op", op }, { "why", why } });
				return;
			}
			if (op == "upgrade")
			{
				std::string why;
				const bool ok = Classes::RequestChooseUpgrade(args.value("index", -1), why);
				Write(a_sink, a_write, { { "ok", ok }, { "op", op }, { "why", why } });
				return;
			}
			if (op == "evolvebox")
			{
				// presses button <index> of the Stats Menu's open evolution box (the last button is Later)
				const int index = args.value("index", -1);
				if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([index]() { StatsMenuClass::RequestPick(index); }); }
				Write(a_sink, a_write, { { "ok", true }, { "op", op }, { "queued", true } });
				return;
			}
			if (op == "xp")
			{
				Classes::RequestGrantXp(args.value("amount", 10.0F));
				Write(a_sink, a_write, { { "ok", true }, { "op", op }, { "queued", true } });
				return;
			}
			if (op == "quest")
			{
				QuestRewards::RequestSimulate(args.value("quest", std::string()));
				Write(a_sink, a_write, { { "ok", true }, { "op", op }, { "queued", true } });
				return;
			}
			if (op == "reload")
			{
				const bool ok = settings::Reload();
				Classes::RequestApply();
				Write(a_sink, a_write, { { "ok", ok }, { "op", op } });
				return;
			}
			if (op == "strings")
			{
				Write(a_sink, a_write, { { "ok", true }, { "op", op }, { "strings", nlohmann::json::parse(strings::StatusJson(), nullptr, false) } });
				return;
			}

			nlohmann::json j;
			j["ok"] = true;
			j["classes"] = nlohmann::json::parse(Classes::StatusJson(), nullptr, false);
			j["questRewards"] = nlohmann::json::parse(QuestRewards::StatusJson(), nullptr, false);
			j["raceMenu"] = nlohmann::json::parse(RaceMenuClass::StatusJson(), nullptr, false);
			j["statsMenu"] = nlohmann::json::parse(StatsMenuClass::StatusJson(), nullptr, false);
			j["effects"] = nlohmann::json::parse(ClassEffects::StatusJson(), nullptr, false);
			const auto lug = LevelUpGains::GetState();
			j["levelUp"] = { { "control", settings::levelup::overrideRewards }, { "perks", settings::levelup::PerksTableText() },
							 { "gains", { settings::levelup::healthPerLevel, settings::levelup::magickaPerLevel, settings::levelup::staminaPerLevel } },
							 { "hookAttached", lug.attached }, { "levelUps", lug.levelUps }, { "skipResolved", lug.skipResolved }, { "picksSkipped", lug.picksSkipped } };
			const auto lk = Lockouts::GetState();
			j["lockouts"] = { { "perksConditioned", lk.perksConditioned }, { "entriesConditioned", lk.entriesConditioned },
							  { "abilityEntriesSkipped", lk.abilityEntriesSkipped }, { "craftingRefusals", lk.craftingRefusals }, { "lastRefusal", lk.lastRefusal } };
			auto& patches = j["patches"] = nlohmann::json::array();
			for (const auto& g : Patches::All()) { patches.push_back({ { "name", g.name }, { "installed", g.installed }, { "status", g.status } }); }
			j["page"] = { { "status", UI::LastStatus() } };
			j["iniPath"] = settings::GetIniPath();
			Write(a_sink, a_write, j);
		}
	}

	void Init(bool a_lastAttempt)
	{
		static bool registered = false;
		if (registered) { return; }
		DevBenchAPI::IDevBenchInterface001* devBench = DevBenchAPI::GetDevBenchInterface001();
		if (!devBench)
		{
			if (a_lastAttempt) { logger::info("DevBench not detected; skipping the \"njcgo.control\" tool"); }
			else { logger::debug("DevBench not detected yet; will retry at the next message"); }
			return;
		}
		constexpr const char* descriptor =
			"{"
			"\"description\":\"Njordlinger Combat Gameplay Overhaul live state: the class (line, tier, level, experience, the upgrade on offer, held and locked skills), "
			"quest-reward rows, the race-menu class category, level-up gains and the attribute-pick skip, the perk lock and crafting gate counts, the engine patches. "
			"op=choose class=<id> picks the starting class; op=upgrade index=<n> takes option n of the upgrade on offer; op=evolvebox index=<n> presses button n of the Stats Menu evolution box (the last is Later); op=xp amount=<n> adds class experience; "
			"op=quest quest=<editorID> pays that quest's row as if it had completed; op=reload re-reads the INI; op=strings reports the active language.\","
			"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\"},\"class\":{\"type\":\"string\"},\"index\":{\"type\":\"integer\"},"
			"\"amount\":{\"type\":\"number\"},\"quest\":{\"type\":\"string\"}}},"
			"\"readOnly\":false"
			"}";
		if (devBench->RegisterTool("njcgo.control", descriptor, &ControlTool, nullptr))
		{
			logger::info("Registered \"njcgo.control\" with DevBench (build {})", devBench->GetBuildNumber());
			registered = true;
		}
	}
}
