#include "PCH.h"

// Njordlinger Combat Gameplay Overhaul - guild and faction routes by class (PLAN.md D30, GUILD-LOCKS.md).
//
// The joining topics' refusals live in our ESP (tools/PluginBuild/GuildBuild.cs): each speaks while the class lacks the
// route's group (our group globals, kept by the Classes module) and NJCGO_RoutesOn is 1. This module keeps that global
// in step with the "Routes by class" switch and the class master switch, stops DB02 ("With Friends Like These...") as it
// starts for a class without the covert group (the Brotherhood never comes), and gives the Classes tab its list.

#include "GuildRoutes.h"

#include "ClassData.h"
#include "Classes.h"
#include "Settings.h"

#include "utils/Logger.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace GuildRoutes
{
	namespace
	{
		RE::TESGlobal* g_on = nullptr;
		std::vector<Route> g_routes;
		// bfbb: our refusal responses were never picked - the game keeps the vanilla first response ahead of ours (both have
		// no previous response; no later plugin in Test overrides these topics - scanned). So the vanilla responses of each
		// joining topic get, at run time and in memory only, a condition in front: "the class holds the group OR routes are
		// off". When it fails, the game falls through to our refusal. No record is overridden on disk.
		struct GateTopic { RE::TESTopic* topic = nullptr; RE::TESGlobal* group = nullptr; };
		std::vector<GateTopic> g_topics;
		std::uint32_t g_gated = 0;

		bool Gated(const RE::TESCondition& a_cond, RE::TESGlobal* a_group)
		{
			auto* head = a_cond.head;
			return head && head->data.functionData.function == RE::FUNCTION_DATA::FunctionID::kGetGlobalValue && head->data.functionData.params[0] == a_group;
		}

		void Gate(RE::TESTopicInfo* a_info, RE::TESGlobal* a_group)
		{
			if (!a_info || !g_on || Gated(a_info->objConditions, a_group)) { return; }
			auto* holds = new RE::TESConditionItem();
			auto* off = new RE::TESConditionItem();
			holds->data.object = RE::CONDITIONITEMOBJECT::kSelf;
			holds->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetGlobalValue;
			holds->data.functionData.params[0] = a_group;
			holds->data.comparisonValue.f = 1.0F;
			holds->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kEqualTo;
			holds->data.flags.isOR = true;
			off->data.object = RE::CONDITIONITEMOBJECT::kSelf;
			off->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetGlobalValue;
			off->data.functionData.params[0] = g_on;
			off->data.comparisonValue.f = 0.0F;
			off->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kEqualTo;
			off->data.flags.isOR = false;
			off->next = a_info->objConditions.head;
			holds->next = off;
			a_info->objConditions.head = holds;
			++g_gated;
		}

		void GateLoadedTopics()
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			auto* ours = dh ? dh->LookupModByName("NjordlingerCombatGameplayOverhaul.esp") : nullptr;
			const auto before = g_gated;
			for (const auto& gt : g_topics)
			{
				if (!gt.topic || !gt.topic->topicInfos) { continue; }
				for (std::uint32_t i = 0; i < gt.topic->numTopicInfos; ++i)
				{
					auto* info = gt.topic->topicInfos[i];
					if (!info || (ours && info->GetFile(0) == ours)) { continue; }   // our refusal stays as it is
					Gate(info, gt.group);
				}
			}
			if (g_gated != before) { logger::info("guild routes: {} vanilla joining response(s) now wait for the class's group", g_gated - before); }
		}

		class DialogueSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_e && a_e->opening && a_e->menuName == RE::DialogueMenu::MENU_NAME)
				{
					if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { GateLoadedTopics(); }); }
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		DialogueSink g_dialogue;
		std::vector<std::pair<RE::TESQuest*, int>> g_stops;   // quest -> group

		RE::TESForm* Resolve(const std::string& a_ref)
		{
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh || a_ref.empty()) { return nullptr; }
			try
			{
				const auto bar = a_ref.find('|');
				if (bar == std::string::npos) { return dh->LookupForm(static_cast<RE::FormID>(std::stoul(a_ref, nullptr, 0)), "NjordlingerCombatGameplayOverhaul.esp"); }
				return dh->LookupForm(static_cast<RE::FormID>(std::stoul(a_ref.substr(bar + 1), nullptr, 0)), a_ref.substr(0, bar));
			}
			catch (...) { return nullptr; }
		}

		int GroupIndex(const std::string& a_id)
		{
			for (int g = 0; g < ClassData::kGroupCount; ++g) { if (a_id == ClassData::kGroupId[g]) { return g; } }
			return -1;
		}

		class QuestSink : public RE::BSTEventSink<RE::TESQuestStartStopEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStartStopEvent* a_e, RE::BSTEventSource<RE::TESQuestStartStopEvent>*) override
			{
				if (!a_e || !a_e->started || !Active()) { return RE::BSEventNotifyControl::kContinue; }
				for (const auto& [q, g] : g_stops)
				{
					if (!q || q->GetFormID() != a_e->formID || Classes::HoldsGroup(g)) { continue; }
					if (auto* tasks = SKSE::GetTaskInterface())
					{
						tasks->AddTask([q]() {
							q->Stop();
							logger::info("guild routes: {} stopped as it started - the class does not hold the covert group", q->GetFormEditorID());
						});
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		QuestSink g_sink;
	}

	bool Active() { return settings::classes::enabled && settings::classes::routes; }

	void Install()
	{
		try
		{
			std::ifstream in(std::filesystem::path("Data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/guild-routes.json"));
			if (!in) { logger::warn("guild routes: guild-routes.json is missing - every route stays open"); return; }
			const auto j = nlohmann::json::parse(in);
			g_on = Resolve(j.value("routesOnGlobal", std::string())) ? Resolve(j.value("routesOnGlobal", std::string()))->As<RE::TESGlobal>() : nullptr;
			for (const auto& r : j.value("routes", nlohmann::json::array()))
			{
				Route route;
				route.id = r.value("id", std::string());
				route.name = r.value("name", std::string());
				route.group = GroupIndex(r.value("group", std::string()));
				g_routes.push_back(route);
				RE::TESGlobal* groupGlobal = nullptr;
				if (route.group >= 0) { groupGlobal = ClassData::Get().groupGlobal[route.group]; }
				for (const auto& ref : r.value("refusals", nlohmann::json::array()))
				{
					auto* t = Resolve(ref.value("topicForm", std::string())) ? Resolve(ref.value("topicForm", std::string()))->As<RE::TESTopic>() : nullptr;
					if (t && groupGlobal) { g_topics.push_back({ t, groupGlobal }); }
				}
				if (r.contains("stopQuest"))
				{
					if (auto* q = Resolve(r["stopQuest"].get<std::string>()) ? Resolve(r["stopQuest"].get<std::string>())->As<RE::TESQuest>() : nullptr) { g_stops.emplace_back(q, route.group); }
				}
			}
		}
		catch (const std::exception& e) { logger::warn("guild routes: guild-routes.json is not valid ({}) - every route stays open", e.what()); return; }
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) { holder->AddEventSink<RE::TESQuestStartStopEvent>(&g_sink); }
		if (auto* ui = RE::UI::GetSingleton()) { ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_dialogue); }
		GateLoadedTopics();   // whatever is loaded already
		logger::info("guild routes: {} routes, global {}, {} joining topic(s), {} quest(s) stopped for a class without the group", g_routes.size(), g_on ? "found" : "MISSING", g_topics.size(), g_stops.size());
	}

	void Apply()
	{
		if (g_on) { g_on->value = Active() ? 1.0F : 0.0F; }
	}

	std::vector<Route> Routes() { return g_routes; }
}
