#include "PCH.h"

#include "QuestRewards.h"

#include "Settings.h"

#include "utils/Logger.h"
#include "utils/Notify.h"
#include "utils/Strings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

namespace QuestRewards
{
	namespace
	{
		constexpr std::uint32_t kRecordVersion = 1;

		struct Shipped
		{
			std::string quest;
			std::string name;
			int points = 0;
		};

		std::mutex g_lock;
		std::vector<Shipped> g_rows;
		int g_default = 0;
		std::set<std::string> g_paid;   // editor IDs paid in this save
		std::set<std::string> g_inLoadOrder;

		int PointsLocked(const Shipped& a_row)
		{
			for (const auto& [id, pts] : settings::questrewards::overrides)
			{
				if (_stricmp(id.c_str(), a_row.quest.c_str()) == 0) { return pts; }
			}
			return a_row.points;
		}

		const Shipped* FindLocked(const std::string& a_quest)
		{
			for (const auto& r : g_rows)
			{
				if (_stricmp(r.quest.c_str(), a_quest.c_str()) == 0) { return &r; }
			}
			return nullptr;
		}

		void Pay(const std::string& a_quest, const char* a_how)
		{
			if (!settings::questrewards::enabled) { logger::debug("quest rewards: {} completed - quest rewards are off", a_quest); return; }
			int points = 0;
			std::string name;
			{
				std::scoped_lock l(g_lock);
				const auto* row = FindLocked(a_quest);
				if (!row) { return; }
				if (g_paid.contains(row->quest)) { logger::info("quest rewards: {} already paid in this save - nothing granted", row->quest); return; }
				points = PointsLocked(*row);
				name = row->name;
				g_paid.insert(row->quest);
			}
			if (points <= 0) { logger::info("quest rewards: {} completed ({}) - its row pays 0", a_quest, a_how); return; }
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) { return; }
			auto& stats = player->GetGameStatsData();
			const int before = static_cast<int>(stats.perkCount);
			const int after = std::clamp(before + points, 0, 127);
			stats.perkCount = static_cast<decltype(stats.perkCount)>(after);
			logger::info("quest rewards: {} completed ({}) - perk points {} -> {} (+{})", a_quest, a_how, before, after, points);
			std::string text = strings::TR("NJ_NoticeQuestPoints", "Perk points earned: {} ({})");
			try { text = std::vformat(text, std::make_format_args(points, name)); } catch (...) {}
			notify::Show(text.c_str());
		}

		class QuestSink : public RE::BSTEventSink<RE::QuestStatus::Event>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::QuestStatus::Event* a_event, RE::BSTEventSource<RE::QuestStatus::Event>*) override
			{
				if (!a_event || !a_event->quest || a_event->status != RE::QuestStatus::kCompleted) { return RE::BSEventNotifyControl::kContinue; }
				const char* edid = a_event->quest->GetFormEditorID();
				if (edid && *edid) { Pay(edid, "quest completed"); }
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		QuestSink g_sink;
	}

	void Install()
	{
		const auto path = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "NjordlingerCombatGameplayOverhaul" / "quest-rewards.json";
		std::ifstream in(path);
		if (!in) { logger::warn("quest rewards: {} not found - no quest pays perk points", path.string()); }
		else
		{
			try
			{
				const auto j = nlohmann::json::parse(in, nullptr, true, true);
				std::scoped_lock l(g_lock);
				g_default = std::clamp(j.value("default", 0), 0, 20);
				for (const auto& r : j.value("rows", nlohmann::json::array()))
				{
					Shipped s;
					s.quest = r.at("quest").get<std::string>();
					s.name = r.value("name", s.quest);
					s.points = std::clamp(r.value("points", g_default), 0, 20);
					g_rows.push_back(std::move(s));
				}
			}
			catch (const std::exception& e)
			{
				logger::error("quest rewards: {} does not parse ({}) - no quest pays perk points", path.string(), e.what());
			}
		}
		if (auto* handler = RE::TESDataHandler::GetSingleton())
		{
			std::scoped_lock l(g_lock);
			for (auto* q : handler->GetFormArray<RE::TESQuest>())
			{
				const char* edid = q ? q->GetFormEditorID() : nullptr;
				if (!edid || !*edid) { continue; }
				for (auto& r : g_rows)
				{
					if (_stricmp(r.quest.c_str(), edid) == 0)
					{
						g_inLoadOrder.insert(r.quest);
						const char* full = q->GetFullName();
						if (full && *full) { r.name = full; }
					}
				}
			}
		}
		if (auto* src = RE::QuestStatus::GetEventSource()) { src->AddEventSink(&g_sink); }
		else { logger::error("quest rewards: no quest-status event source - quests cannot pay"); }
		logger::info("quest rewards: {} rows read (default {}), {} of them in the load order", g_rows.size(), g_default, g_inLoadOrder.size());
	}

	std::vector<Row> Rows()
	{
		std::vector<Row> out;
		std::scoped_lock l(g_lock);
		for (const auto& r : g_rows)
		{
			out.push_back({ r.quest, r.name, r.points, PointsLocked(r), g_inLoadOrder.contains(r.quest), g_paid.contains(r.quest) });
		}
		return out;
	}

	int DefaultPoints() { return g_default; }

	void SetPoints(const std::string& a_quest, int a_points)
	{
		std::scoped_lock l(g_lock);
		const auto* row = FindLocked(a_quest);
		if (!row) { return; }
		settings::questrewards::overrides[row->quest] = std::clamp(a_points, 0, 20);
	}

	void RequestSimulate(const std::string& a_quest)
	{
		if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([q = a_quest]() { Pay(q, "simulated"); }); }
	}

	std::string StatusJson()
	{
		nlohmann::json j;
		j["enabled"] = settings::questrewards::enabled;
		j["default"] = g_default;
		auto& rows = j["rows"] = nlohmann::json::array();
		for (const auto& r : Rows()) { rows.push_back({ { "quest", r.quest }, { "points", r.points }, { "shipped", r.shipped }, { "inLoadOrder", r.inLoadOrder }, { "paid", r.paid } }); }
		return j.dump();
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		nlohmann::json j;
		{
			std::scoped_lock l(g_lock);
			j["paid"] = g_paid;
		}
		const std::string text = j.dump();
		const auto size = static_cast<std::uint32_t>(text.size());
		if (!a_intfc || !a_intfc->OpenRecord(kRecord, kRecordVersion)) { logger::error("quest rewards: could not open the co-save record"); return; }
		a_intfc->WriteRecordData(&size, sizeof(size));
		a_intfc->WriteRecordData(text.data(), size);
	}

	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
	{
		OnRevert();
		std::uint32_t size = 0;
		if (!a_intfc || a_version != kRecordVersion || a_length < sizeof(size) || !a_intfc->ReadRecordData(&size, sizeof(size)) || size != a_length - sizeof(size))
		{
			logger::warn("quest rewards: co-save record not understood; no quest counts as paid");
			return;
		}
		std::string text(size, '\0');
		if (size && !a_intfc->ReadRecordData(text.data(), size)) { return; }
		try
		{
			const auto j = nlohmann::json::parse(text);
			std::scoped_lock l(g_lock);
			for (const auto& q : j.value("paid", nlohmann::json::array())) { g_paid.insert(q.get<std::string>()); }
			logger::info("quest rewards: co-save loaded - {} quests already paid", g_paid.size());
		}
		catch (...) { logger::warn("quest rewards: co-save record does not parse; no quest counts as paid"); }
	}

	void OnRevert()
	{
		std::scoped_lock l(g_lock);
		g_paid.clear();
	}
}
