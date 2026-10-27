#include "PCH.h"

#include "Classes.h"

#include "ClassData.h"
#include "GuildRoutes.h"
#include "Settings.h"
#include "Skills.h"

#include "utils/Logger.h"
#include "utils/Notify.h"
#include "utils/Strings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <unordered_map>

namespace Classes
{
	namespace
	{
		constexpr std::uint32_t kRecordVersion = 1;

		struct Record
		{
			std::string classId;                          // "" = not chosen yet
			int tier = 0;                                 // 0 Novice .. 5 Legendary
			int level = 0;                                // class level
			float xp = 0.0F;                              // progress toward the next class level
			std::array<float, skilllist::kCount> startSkill{};
			std::vector<std::string> history;             // class ids in the order they were taken
			int rankPath[3]{ -1, -1, -1 };                // Expert, Master, Legendary perk: 0 = A, 1 = B, -1 = not taken
			std::string title;
			std::uint32_t increases = 0;                  // skill increases counted toward class experience
		};

		std::mutex g_lock;
		Record g_rec;
		bool g_notifiedDue = false;
		std::unordered_map<std::string, RE::TESQuest*> g_quests;   // editor ID -> quest

		std::string TrS(const char* a_key, const char* a_english) { return strings::TR(a_key, a_english); }

		// A translated format string applied safely: a translation with the wrong placeholders shows the text unformatted
		// instead of throwing.
		template <class... A>
		std::string Fmt(const std::string& a_format, A&&... a_args)
		{
			try { return std::vformat(a_format, std::make_format_args(a_args...)); }
			catch (...) { return a_format; }
		}

		std::string ClassName(const ClassData::ClassNode& a_n)
		{
			const auto key = "NJ_ClassName_" + a_n.id;
			return TrS(key.c_str(), a_n.name.c_str());
		}

		std::string ClassDesc(const ClassData::ClassNode& a_n)
		{
			const auto key = "NJ_ClassDesc_" + a_n.id;
			return TrS(key.c_str(), a_n.description.c_str());
		}

		std::string TitleName(const ClassData::Title& a_t)
		{
			const auto key = "NJ_TitleName_" + a_t.id;
			return TrS(key.c_str(), a_t.name.c_str());
		}

		std::string TitleDesc(const ClassData::Title& a_t)
		{
			const auto key = "NJ_TitleDesc_" + a_t.id;
			return TrS(key.c_str(), a_t.description.c_str());
		}

		std::string TierName(int a_tier)
		{
			static constexpr const char* kKeys[ClassData::kTierCount] = { "NJ_Tier_Novice", "NJ_Tier_Apprentice", "NJ_Tier_Adept", "NJ_Tier_Expert", "NJ_Tier_Master", "NJ_Tier_Legendary" };
			const int t = std::clamp(a_tier, 0, ClassData::kTierCount - 1);
			return TrS(kKeys[t], ClassData::kTierName[t]);
		}

		float CostOfLevel(int a_level)
		{
			return std::max(0.1F, settings::classes::xpBase + settings::classes::xpPerLevel * static_cast<float>(a_level));
		}

		int UpgradeLevelFor(int a_tier) { return settings::classes::levelsPerTier * (a_tier + 1); }

		bool QuestDone(const std::string& a_editorId)
		{
			const auto it = g_quests.find(a_editorId);
			return it != g_quests.end() && it->second && it->second->IsCompleted();
		}

		std::string RankName(const ClassData::ClassNode& a_n, int a_step, int a_ab)
		{
			static constexpr const char* kSlot[3][2] = { { "A1", "B1" }, { "A2", "B2" }, { "A3", "B3" } };
			const auto key = "NJ_RankName_" + a_n.id + "_" + kSlot[a_step][a_ab];
			return TrS(key.c_str(), a_n.ranks[a_step][a_ab].name.c_str());
		}

		std::string RankDesc(const ClassData::ClassNode& a_n, int a_step, int a_ab)
		{
			static constexpr const char* kSlot[3][2] = { { "A1", "B1" }, { "A2", "B2" }, { "A3", "B3" } };
			const auto key = "NJ_RankDesc_" + a_n.id + "_" + kSlot[a_step][a_ab];
			return TrS(key.c_str(), a_n.ranks[a_step][a_ab].description.c_str());
		}

		// The titles this character has earned that fit its lineage: the questline is finished and the title's rule
		// (the lineage's start, the current class) matches.
		std::vector<const ClassData::Title*> EarnedTitlesLocked(const Record& a_r)
		{
			std::vector<const ClassData::Title*> out;
			if (a_r.classId.empty() || a_r.history.empty()) { return out; }
			for (const auto& ti : ClassData::Get().titles)
			{
				if (ti.Fits(a_r.history.front(), a_r.classId) && QuestDone(ti.quest)) { out.push_back(&ti); }
			}
			return out;
		}

		// Whether the record (locked by the caller) holds a skill.
		bool HoldsLocked(const Record& a_r, int a_skill)
		{
			if (!settings::classes::enabled || a_r.classId.empty()) { return true; }
			const auto* n = ClassData::Find(a_r.classId);
			if (!n) { return true; }
			if (a_skill < 0 || a_skill >= skilllist::kCount) { return true; }
			// Extra skills are inherited along the lineage (CLASS-CONTENT.md 1: the Archer's Archery stays with the Scout or
			// Witchhunter it becomes).
			for (const auto& id : a_r.history)
			{
				if (const auto* h = ClassData::Find(id); h && h->extraSkills[a_skill]) { return true; }
			}
			const int g = ClassData::GroupOfSkill(a_skill);
			return g < 0 || n->groups[g];
		}

		bool HoldsGroupLocked(const Record& a_r, int a_group)
		{
			if (!settings::classes::enabled || a_r.classId.empty()) { return true; }
			const auto* n = ClassData::Find(a_r.classId);
			return !n || a_group < 0 || a_group >= ClassData::kGroupCount || n->groups[a_group];
		}

		bool UpgradeDueLocked(const Record& a_r)
		{
			return settings::classes::enabled && !a_r.classId.empty() && a_r.tier < ClassData::kTierCount - 1 && a_r.level >= UpgradeLevelFor(a_r.tier);
		}

		std::vector<Option> OptionsLocked(const Record& a_r)
		{
			std::vector<Option> out;
			if (!UpgradeDueLocked(a_r)) { return out; }
			const auto* n = ClassData::Find(a_r.classId);
			if (!n) { return out; }
			if (a_r.tier <= 1)
			{
				for (const auto& id : n->next)
				{
					const auto* o = ClassData::Find(id);
					if (!o) { continue; }
					Option op{ o->id, ClassName(*o), ClassDesc(*o), true, {} };
					out.push_back(std::move(op));
				}
			}
			else if (a_r.tier <= 3 || EarnedTitlesLocked(a_r).empty())
			{
				// Expert (tier 2 -> 3) and Master (3 -> 4): the class's A/B rank perks of that step; Legendary (4 -> 5) with no
				// earned title that fits the lineage: the A3/B3 pair.
				const int step = a_r.tier - 2;
				for (int ab = 0; ab < 2; ++ab)
				{
					const auto& rank = n->ranks[step][ab];
					Option op;
					op.id = ab == 0 ? "rank:A" : "rank:B";
					op.label = RankName(*n, step, ab);
					op.description = RankDesc(*n, step, ab);
					op.available = n->hasRanks;
					if (!op.available) { op.why = TrS("NJ_NoRanks", "This class has no rank perks."); }
					out.push_back(std::move(op));
					(void)rank;
				}
			}
			else
			{
				// Legendary with a title earned: each earned title that fits the lineage is an option; the one chosen is
				// shown on the class line, and every earned title applies (CLASS-CONTENT.md 1).
				for (const auto* ti : EarnedTitlesLocked(a_r))
				{
					Option op{ ti->id, TitleName(*ti), TitleDesc(*ti), true, {} };
					out.push_back(std::move(op));
				}
			}
			return out;
		}

		std::string LineLocked(const Record& a_r)
		{
			if (a_r.classId.empty()) { return TrS("NJ_NoClass", "No class chosen yet"); }
			const auto* n = ClassData::Find(a_r.classId);
			std::string name = n ? ClassName(*n) : a_r.classId;
			if (a_r.tier >= ClassData::kTierCount - 1 && !a_r.title.empty())
			{
				if (const auto* t = ClassData::FindTitle(a_r.title)) { name = TitleName(*t); }
			}
			if (a_r.tier >= ClassData::kTierCount - 1)
			{
				return Fmt(TrS("NJ_ClassLineTop", "{} {} - class level {}"), TierName(a_r.tier), name, a_r.level);
			}
			const int next = UpgradeLevelFor(a_r.tier);
			// Combined run 78d7: "Novice Mage - class level 18 of 10" - past the tier's mark the line says the class can
			// evolve instead of a count it has already passed.
			if (a_r.level >= next) { return Fmt(TrS("NJ_ClassLineReady", "{} {} - class level {} - ready to evolve"), TierName(a_r.tier), name, a_r.level); }
			return Fmt(TrS("NJ_ClassLine", "{} {} - class level {} of {}"), TierName(a_r.tier), name, a_r.level, next);
		}

		void Notify(const std::string& a_text)
		{
			notify::Show(a_text.c_str());
			logger::info("notice: {}", a_text);
		}

		// Applies everything the record decides to the running game (main thread).
		void Apply()
		{
			Record r;
			{
				std::scoped_lock l(g_lock);
				r = g_rec;
			}
			const auto& table = ClassData::Get();
			// Skill locks: a locked skill is capped at the value it had when the class was chosen.
			for (int i = 0; i < skilllist::kCount; ++i)
			{
				const bool held = HoldsLocked(r, i);
				Skills::SetLockedCap(i, held ? -1.0F : r.startSkill[i]);
			}
			// Group globals: 1 = the perks of that group's trees may be taken and apply.
			for (int g = 0; g < ClassData::kGroupCount; ++g)
			{
				if (auto* glob = table.groupGlobal[g])
				{
					const bool open = !settings::classes::lockPerks || HoldsGroupLocked(r, g);
					glob->value = open ? 1.0F : 0.0F;
				}
			}

			GuildRoutes::Apply();   // D30: the routes' refusals follow the switches
			// Class globals: 1 for every class this character has been (a Crusader is still a Knight for a condition).
			for (const auto& n : table.classes)
			{
				if (!n.global) { continue; }
				n.global->value = std::find(r.history.begin(), r.history.end(), n.id) != r.history.end() ? 1.0F : 0.0F;
			}
			// Abilities, rank perks and the title: cumulative, re-added when missing (a plugin re-enabled, a console removal).
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && settings::classes::enabled)
			{
				const auto give = [player](RE::BGSPerk* a_perk, const std::string& a_what) {
					if (a_perk && !player->HasPerk(a_perk))
					{
						player->AddPerk(a_perk);
						logger::info("classes: {} added ({:08X})", a_what, a_perk->GetFormID());
					}
				};
				for (const auto& id : r.history)
				{
					if (const auto* n = ClassData::Find(id)) { give(n->ability, "ability of " + n->id); }
				}
				if (const auto* cur = ClassData::Find(r.classId))
				{
					for (int step = 0; step < 3; ++step)
					{
						if (r.rankPath[step] < 0 || r.rankPath[step] > 1) { continue; }
						give(cur->ranks[step][r.rankPath[step]].perk, std::format("rank perk {} step {} path {}", cur->id, step, r.rankPath[step] ? "B" : "A"));
					}
				}
				if (!r.title.empty())
				{
					// every earned title applies once the character holds one (CLASS-CONTENT.md 1)
					for (const auto* ti : EarnedTitlesLocked(r)) { give(ti->perk, "title " + ti->id); }
					if (const auto* ti = ClassData::FindTitle(r.title)) { give(ti->perk, "title " + ti->id); }
				}
			}
			logger::debug("classes: applied - class \"{}\" tier {} level {} xp {:.2f}", r.classId, r.tier, r.level, r.xp);
		}

		void CheckDue()
		{
			bool notify = false;
			{
				std::scoped_lock l(g_lock);
				const bool due = UpgradeDueLocked(g_rec);
				if (due && !g_notifiedDue) { g_notifiedDue = true; notify = true; }
				if (!due) { g_notifiedDue = false; }
			}
			if (notify) { Notify(TrS("NJ_NoticeUpgrade", "Your class can evolve. Open the Classes tab of Njordlinger Combat Gameplay Overhaul to choose.")); }
		}

		void AddXpLocked(float a_amount)
		{
			g_rec.xp += a_amount;
			int guard = 0;
			while (g_rec.xp >= CostOfLevel(g_rec.level) && guard++ < 200)
			{
				g_rec.xp -= CostOfLevel(g_rec.level);
				++g_rec.level;
				logger::info("classes: class level {} reached ({})", g_rec.level, g_rec.classId);
			}
		}

		class SkillSink : public RE::BSTEventSink<RE::SkillIncrease::Event>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::SkillIncrease::Event* a_event, RE::BSTEventSource<RE::SkillIncrease::Event>*) override
			{
				if (!a_event || !settings::classes::enabled) { return RE::BSEventNotifyControl::kContinue; }
				const auto av = static_cast<std::uint32_t>(a_event->actorValue);
				if (av < 6 || av > 23) { return RE::BSEventNotifyControl::kContinue; }
				const int skill = static_cast<int>(av - 6);
				bool counted = false;
				{
					std::scoped_lock l(g_lock);
					if (!g_rec.classId.empty() && HoldsLocked(g_rec, skill))
					{
						AddXpLocked(settings::classes::xpPerIncrease);
						++g_rec.increases;
						counted = true;
					}
				}
				logger::debug("classes: {} increased - {}", skilllist::kDisplayName[skill], counted ? "class experience added" : "not a skill of the class (or no class yet)");
				if (counted) { CheckDue(); }
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || a_event->opening || a_event->menuName != RE::RaceSexMenu::MENU_NAME) { return RE::BSEventNotifyControl::kContinue; }
				bool none = false;
				{
					std::scoped_lock l(g_lock);
					none = settings::classes::enabled && ClassData::Get().loaded && g_rec.classId.empty();
				}
				if (none)
				{
					if (auto* tasks = SKSE::GetTaskInterface())
					{
						tasks->AddTask([]() { Notify(strings::TR("NJ_NoticeChooseClass", "Choose your starting class: open the Classes tab of Njordlinger Combat Gameplay Overhaul.")); });
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class QuestSink : public RE::BSTEventSink<RE::QuestStatus::Event>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::QuestStatus::Event* a_event, RE::BSTEventSource<RE::QuestStatus::Event>*) override
			{
				if (!a_event || !a_event->quest || a_event->status != RE::QuestStatus::kCompleted) { return RE::BSEventNotifyControl::kContinue; }
				const char* edid = a_event->quest->GetFormEditorID();
				if (!edid || !*edid) { return RE::BSEventNotifyControl::kContinue; }
				for (const auto& ti : ClassData::Get().titles)
				{
					if (_stricmp(ti.quest.c_str(), edid) == 0)
					{
						logger::info("classes: {} completed - the title {} can now be earned by a class it fits", edid, ti.id);
						CheckDue();
						RequestApply();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		SkillSink g_skillSink;
		QuestSink g_questSink;
		MenuSink g_menuSink;

		void ChooseStartNow(const std::string& a_id)
		{
			const auto* n = ClassData::Find(a_id);
			if (!n || !n->start) { logger::warn("classes: \"{}\" is not a starting class; nothing chosen", a_id); return; }
			const auto s = Skills::GetState();
			{
				std::scoped_lock l(g_lock);
				if (!g_rec.classId.empty()) { logger::warn("classes: a class is already chosen ({}); the start cannot change", g_rec.classId); return; }
				g_rec = Record{};
				g_rec.classId = n->id;
				g_rec.history.push_back(n->id);
				for (int i = 0; i < skilllist::kCount; ++i) { g_rec.startSkill[i] = s.readable ? s.skill[i].level : 15.0F; }
			}
			logger::info("classes: starting class {} chosen; skills recorded as the locked values ({})", n->id, s.readable ? "read from the character" : "the character's skills were not readable - 15 used");
			Notify(Fmt(TrS("NJ_NoticeChosen", "You are now a {}."), ClassName(*n)));
			Apply();
		}

		void ChooseUpgradeNow(int a_index)
		{
			std::string notice;
			{
				std::scoped_lock l(g_lock);
				const auto options = OptionsLocked(g_rec);
				if (a_index < 0 || a_index >= static_cast<int>(options.size())) { logger::warn("classes: upgrade option {} is not on offer", a_index); return; }
				const auto& op = options[a_index];
				if (!op.available) { logger::warn("classes: upgrade option {} ({}) is not available: {}", a_index, op.id, op.why); return; }
				const int from = g_rec.tier;
				if (from <= 1)
				{
					g_rec.classId = op.id;
					g_rec.history.push_back(op.id);
				}
				else if (op.id.rfind("rank:", 0) == 0)
				{
					g_rec.rankPath[from - 2] = op.id == "rank:B" ? 1 : 0;
				}
				else
				{
					g_rec.title = op.id;
				}
				g_rec.tier = from + 1;
				g_notifiedDue = false;
				logger::info("classes: upgrade {} -> {} chose {} ({})", ClassData::kTierName[from], ClassData::kTierName[g_rec.tier], op.id, op.label);
				notice = Fmt(TrS("NJ_NoticeEvolved", "Your class evolved: {}."), LineLocked(g_rec));
			}
			Notify(notice);
			Apply();
			CheckDue();
		}
	}

	void Install()
	{
		ClassData::Load();
		if (auto* handler = RE::TESDataHandler::GetSingleton())
		{
			for (auto* q : handler->GetFormArray<RE::TESQuest>())
			{
				if (!q) { continue; }
				const char* edid = q->GetFormEditorID();
				if (edid && *edid) { g_quests[edid] = q; }
			}
		}
		logger::info("classes: {} quests indexed by editor ID for the title questlines", g_quests.size());
		for (const auto& t : ClassData::Get().titles)
		{
			if (!g_quests.contains(t.quest)) { logger::warn("classes: the title {}'s quest \"{}\" is not in the load order - the title can never be offered", t.id, t.quest); }
		}
		if (auto* src = RE::SkillIncrease::GetEventSource()) { src->AddEventSink(&g_skillSink); logger::info("classes: skill-increase sink registered"); }
		else { logger::error("classes: no skill-increase event source - class experience cannot be earned"); }
		if (auto* ui = RE::UI::GetSingleton()) { ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink); }
		if (auto* qs = RE::QuestStatus::GetEventSource()) { qs->AddEventSink(&g_questSink); }
	}

	void OnGameLoaded()
	{
		CheckDue();
		RequestApply();
	}

	void RequestApply()
	{
		if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { Apply(); }); }
	}

	bool RequestChooseStart(const std::string& a_classId, std::string& a_why)
	{
		{
			std::scoped_lock l(g_lock);
			if (!settings::classes::enabled) { a_why = TrS("NJ_WhyDisabled", "Classes are switched off."); return false; }
			if (!ClassData::Get().loaded) { a_why = TrS("NJ_WhyNoTable", "The class table could not be read - see the log."); return false; }
			if (!g_rec.classId.empty()) { a_why = TrS("NJ_WhyAlreadyChosen", "A class is already chosen for this character."); return false; }
		}
		const auto* n = ClassData::Find(a_classId);
		if (!n || !n->start) { a_why = TrS("NJ_WhyNotStart", "That is not a starting class."); return false; }
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell()) { a_why = TrS("NJ_WhyNoGame", "Load a game first."); return false; }
		if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([id = a_classId]() { ChooseStartNow(id); }); }
		return true;
	}

	bool RequestChooseUpgrade(int a_index, std::string& a_why)
	{
		{
			std::scoped_lock l(g_lock);
			const auto options = OptionsLocked(g_rec);
			if (options.empty()) { a_why = TrS("NJ_WhyNoUpgrade", "No evolution is on offer now."); return false; }
			if (a_index < 0 || a_index >= static_cast<int>(options.size())) { a_why = TrS("NJ_WhyNoOption", "That option does not exist."); return false; }
			if (!options[a_index].available) { a_why = options[a_index].why; return false; }
		}
		if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([a_index]() { ChooseUpgradeNow(a_index); }); }
		return true;
	}

	void RequestGrantXp(float a_amount)
	{
		if (auto* tasks = SKSE::GetTaskInterface())
		{
			tasks->AddTask([a_amount]() {
				{
					std::scoped_lock l(g_lock);
					if (g_rec.classId.empty()) { logger::warn("classes: test experience refused - no class chosen"); return; }
					AddXpLocked(a_amount);
				}
				logger::info("classes: {:.1f} class experience granted for testing", a_amount);
				CheckDue();
			});
		}
	}

	Snapshot GetSnapshot()
	{
		Snapshot s;
		const auto& table = ClassData::Get();
		s.tableLoaded = table.loaded;
		s.tableError = table.error;
		s.enabled = settings::classes::enabled;
		std::scoped_lock l(g_lock);
		s.chosen = !g_rec.classId.empty();
		s.classId = g_rec.classId;
		if (const auto* n = ClassData::Find(g_rec.classId)) { s.className = ClassName(*n); }
		s.line = LineLocked(g_rec);
		s.tier = g_rec.tier;
		s.level = g_rec.level;
		s.xp = g_rec.xp;
		s.xpToNext = CostOfLevel(g_rec.level);
		s.nextUpgradeLevel = g_rec.tier < ClassData::kTierCount - 1 ? UpgradeLevelFor(g_rec.tier) : 0;
		s.upgradeDue = UpgradeDueLocked(g_rec);
		s.options = OptionsLocked(g_rec);
		for (int i = 0; i < skilllist::kCount; ++i)
		{
			s.held[i] = HoldsLocked(g_rec, i);
			s.lockedCap[i] = Skills::LockedCap(i);
		}
		for (int g = 0; g < 3; ++g) { s.groupsHeld[g] = HoldsGroupLocked(g_rec, g); }
		s.history = g_rec.history;
		s.rankPath[0] = g_rec.rankPath[0];
		s.rankPath[1] = g_rec.rankPath[1];
		s.rankPath[2] = g_rec.rankPath[2];
		for (const auto* ti : EarnedTitlesLocked(g_rec)) { s.titlesEarned.push_back(ti->id); }
		s.title = g_rec.title;
		s.increasesCounted = g_rec.increases;
		return s;
	}

	std::string StatusJson()
	{
		const auto s = GetSnapshot();
		nlohmann::json j;
		j["tableLoaded"] = s.tableLoaded;
		j["tableError"] = s.tableError;
		j["enabled"] = s.enabled;
		j["chosen"] = s.chosen;
		j["classId"] = s.classId;
		j["line"] = s.line;
		j["tier"] = s.tier;
		j["level"] = s.level;
		j["xp"] = s.xp;
		j["xpToNext"] = s.xpToNext;
		j["nextUpgradeLevel"] = s.nextUpgradeLevel;
		j["upgradeDue"] = s.upgradeDue;
		auto& opts = j["options"] = nlohmann::json::array();
		for (const auto& o : s.options) { opts.push_back({ { "id", o.id }, { "label", o.label }, { "available", o.available }, { "why", o.why } }); }
		auto& skills = j["skills"] = nlohmann::json::object();
		for (int i = 0; i < skilllist::kCount; ++i) { skills[skilllist::kIniName[i]] = { { "held", s.held[i] }, { "lockedCap", s.lockedCap[i] }, { "cap", Skills::EffectiveCap(i) } }; }
		j["groupsHeld"] = { { "warrior", s.groupsHeld[0] }, { "magic", s.groupsHeld[1] }, { "covert", s.groupsHeld[2] } };
		j["history"] = s.history;
		j["rankPath"] = { s.rankPath[0], s.rankPath[1], s.rankPath[2] };
		j["titlesEarned"] = s.titlesEarned;
		j["title"] = s.title;
		j["increasesCounted"] = s.increasesCounted;
		return j.dump();
	}

	std::vector<StartClass> StartClasses()
	{
		std::vector<StartClass> out;
		for (const auto& n : ClassData::Get().classes)
		{
			if (n.start) { out.push_back({ n.id, ClassName(n), ClassDesc(n) }); }
		}
		return out;
	}

	bool Holds(int a_skillIndex)
	{
		std::scoped_lock l(g_lock);
		return HoldsLocked(g_rec, a_skillIndex);
	}

	bool HoldsGroup(int a_group)
	{
		std::scoped_lock l(g_lock);
		return HoldsGroupLocked(g_rec, a_group);
	}

	// The record is one JSON document: easy to extend without a record-version dance, and readable in a dump.
	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		nlohmann::json j;
		{
			std::scoped_lock l(g_lock);
			j["classId"] = g_rec.classId;
			j["tier"] = g_rec.tier;
			j["level"] = g_rec.level;
			j["xp"] = g_rec.xp;
			j["startSkill"] = g_rec.startSkill;
			j["history"] = g_rec.history;
			j["rankPath"] = { g_rec.rankPath[0], g_rec.rankPath[1], g_rec.rankPath[2] };
			j["title"] = g_rec.title;
			j["increases"] = g_rec.increases;
		}
		const std::string text = j.dump();
		const auto size = static_cast<std::uint32_t>(text.size());
		if (!a_intfc || !a_intfc->OpenRecord(kRecord, kRecordVersion)) { logger::error("classes: could not open the co-save record; the class was not saved"); return; }
		if (!a_intfc->WriteRecordData(&size, sizeof(size)) || !a_intfc->WriteRecordData(text.data(), size)) { logger::error("classes: writing the co-save record failed"); return; }
		logger::debug("classes: co-save written ({} bytes)", size);
	}

	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
	{
		OnRevert();
		if (!a_intfc || a_version != kRecordVersion || a_length < sizeof(std::uint32_t))
		{
			logger::warn("classes: co-save record version {} length {} not understood; no class loaded", a_version, a_length);
			return;
		}
		std::uint32_t size = 0;
		if (!a_intfc->ReadRecordData(&size, sizeof(size)) || size != a_length - sizeof(size)) { logger::warn("classes: co-save record is short; no class loaded"); return; }
		std::string text(size, '\0');
		if (size && !a_intfc->ReadRecordData(text.data(), size)) { logger::warn("classes: co-save record could not be read; no class loaded"); return; }
		try
		{
			const auto j = nlohmann::json::parse(text);
			Record r;
			r.classId = j.value("classId", std::string());
			r.tier = std::clamp(j.value("tier", 0), 0, ClassData::kTierCount - 1);
			r.level = std::max(0, j.value("level", 0));
			r.xp = std::max(0.0F, j.value("xp", 0.0F));
			if (j.contains("startSkill") && j["startSkill"].is_array())
			{
				for (int i = 0; i < skilllist::kCount && i < static_cast<int>(j["startSkill"].size()); ++i) { r.startSkill[i] = j["startSkill"][i].get<float>(); }
			}
			r.history = j.value("history", std::vector<std::string>{});
			if (j.contains("rankPath") && j["rankPath"].is_array())
			{
				for (std::size_t k = 0; k < 3 && k < j["rankPath"].size(); ++k) { r.rankPath[k] = j["rankPath"][k].get<int>(); }
			}
			r.title = j.value("title", std::string());
			r.increases = j.value("increases", 0u);
			if (!r.classId.empty() && !ClassData::Find(r.classId)) { logger::warn("classes: the saved class \"{}\" is not in the class table any more - the record is kept, locks are off until it returns", r.classId); }
			std::scoped_lock l(g_lock);
			g_rec = std::move(r);
			logger::info("classes: co-save loaded - class \"{}\", tier {}, level {}, xp {:.2f}", g_rec.classId, g_rec.tier, g_rec.level, g_rec.xp);
		}
		catch (const std::exception& e)
		{
			logger::warn("classes: co-save record does not parse ({}); no class loaded", e.what());
		}
	}

	void OnRevert()
	{
		std::scoped_lock l(g_lock);
		g_rec = Record{};
		g_notifiedDue = false;
		for (int i = 0; i < skilllist::kCount; ++i) { Skills::SetLockedCap(i, -1.0F); }
	}
}
