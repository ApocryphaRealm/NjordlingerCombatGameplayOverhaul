#include "PCH.h"

#include "Lockouts.h"

#include "ClassData.h"
#include "Classes.h"
#include "Settings.h"

#include "utils/Logger.h"
#include "utils/Notify.h"
#include "utils/Strings.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <regex>
#include <unordered_set>

namespace Lockouts
{
	namespace
	{
		std::atomic<std::uint32_t> g_perks{ 0 };
		std::atomic<std::uint32_t> g_entries{ 0 };
		std::atomic<std::uint32_t> g_abilities{ 0 };
		std::atomic<std::uint32_t> g_refusals{ 0 };
		std::mutex g_lock;
		std::string g_lastRefusal;

		// Puts (GetIsReference(Player) == 0 OR GetGlobalValue(a_global) == 1) in front of a condition list. The game reads
		// a list as OR-runs joined by AND, so the pair is one OR-run ANDed with whatever the list already held.
		bool Prepend(RE::TESCondition& a_cond, RE::TESGlobal* a_global, RE::PlayerCharacter* a_player)
		{
			auto* notPlayer = new RE::TESConditionItem();
			auto* open = new RE::TESConditionItem();
			if (!notPlayer || !open) { return false; }

			notPlayer->data.object = RE::CONDITIONITEMOBJECT::kSelf;
			notPlayer->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetIsReference;
			notPlayer->data.functionData.params[0] = a_player;
			notPlayer->data.comparisonValue.f = 0.0F;
			notPlayer->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kEqualTo;
			notPlayer->data.flags.isOR = true;

			open->data.object = RE::CONDITIONITEMOBJECT::kSelf;
			open->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetGlobalValue;
			open->data.functionData.params[0] = a_global;
			open->data.comparisonValue.f = 1.0F;
			open->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kEqualTo;
			open->data.flags.isOR = false;

			open->next = a_cond.head;
			notPlayer->next = open;
			a_cond.head = notPlayer;
			return true;
		}

		void ConditionPerk(RE::BGSPerk* a_perk, RE::TESGlobal* a_global, RE::PlayerCharacter* a_player)
		{
			if (Prepend(a_perk->perkConditions, a_global, a_player)) { ++g_perks; }
			for (auto* entry : a_perk->perkEntries)
			{
				if (!entry) { continue; }
				const auto type = entry->GetType();
				if (type == RE::PERK_ENTRY_TYPE::kEntryPoint)
				{
					auto* ep = static_cast<RE::BGSEntryPointPerkEntry*>(entry);
					if (ep->conditions.size() > 0 && Prepend(ep->conditions[0], a_global, a_player)) { ++g_entries; }
				}
				else if (type == RE::PERK_ENTRY_TYPE::kAbility)
				{
					++g_abilities;
				}
			}
		}

		void PerkPass()
		{
			const auto& table = ClassData::Get();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!table.loaded || !player) { logger::error("perk lock: no class table or no player object - perks are not locked"); return; }
			std::unordered_set<RE::BGSPerk*> seen;
			for (int i = 0; i < skilllist::kCount; ++i)
			{
				const int group = ClassData::GroupOfSkill(i);
				RE::TESGlobal* global = group >= 0 ? table.groupGlobal[group] : nullptr;
				if (!global) { logger::warn("perk lock: {} has no group global - its perks are not locked", skilllist::kDisplayName[i]); continue; }
				const auto av = static_cast<RE::ActorValue>(i + 6);
#if RUNTIME_LINE == 17
				auto* info = RE::ActorValueList::GetActorValueInfo(av);
#else
				auto* list = RE::ActorValueList::GetSingleton();
				auto* info = list ? list->GetActorValue(av) : nullptr;
#endif
				if (!info || !info->perkTree) { logger::warn("perk lock: {} has no perk tree", skilllist::kDisplayName[i]); continue; }
				std::uint32_t before = g_perks.load();
				std::vector<RE::BGSSkillPerkTreeNode*> stack{ info->perkTree };
				std::unordered_set<RE::BGSSkillPerkTreeNode*> visited;
				while (!stack.empty())
				{
					auto* node = stack.back();
					stack.pop_back();
					if (!node || !visited.insert(node).second) { continue; }
					// Every rank of the perk: the tree names the first, the ranks chain through nextPerk.
					for (auto* perk = node->perk; perk && seen.insert(perk).second; perk = perk->nextPerk) { ConditionPerk(perk, global, player); }
					for (auto* child : node->children) { stack.push_back(child); }
				}
				logger::debug("perk lock: {} - {} perks conditioned on the {} group", skilllist::kDisplayName[i], g_perks.load() - before, ClassData::kGroupId[group]);
			}
			// Custom Skills Framework trees (PLAN.md phase 2 step 9; classes.json "customTrees"): every perk the tree's plugin
			// defines is gated on the group the tree belongs to, the same way. The tree's own perk points are its own
			// globals - a class without the group can still earn them, but no perk of the tree takes or works for it.
			try
			{
				std::ifstream in(std::filesystem::path("Data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/classes.json"));
				const auto j = in ? nlohmann::json::parse(in) : nlohmann::json::object();
				auto* handler = RE::TESDataHandler::GetSingleton();
				for (const auto& t : j.value("customTrees", nlohmann::json::array()))
				{
					const auto plugin = t.value("plugin", std::string());
					const auto groupId = t.value("group", std::string());
					int group = -1;
					for (int g = 0; g < ClassData::kGroupCount; ++g) { if (groupId == ClassData::kGroupId[g]) { group = g; } }
					auto* file = handler ? handler->LookupModByName(plugin) : nullptr;
					RE::TESGlobal* global = group >= 0 ? table.groupGlobal[group] : nullptr;
					if (!file || !global) { logger::info("perk lock: custom tree {} ({}) - {}", t.value("skill", std::string()), plugin, !file ? "plugin not loaded" : "unknown group"); continue; }
					std::uint32_t n = 0;
					// ed4e: the Necromancer tree's perktree.json is not valid JSON (a brace closes the object early), so a JSON
					// parser reads nothing. The file is read as text and every "perk": "<plugin>|<id>" in it is taken - the tree's
					// nodes, in any shape the framework accepts.
					std::ifstream tf(std::filesystem::path("Data") / t.value("perkTree", std::string()), std::ios::binary);
					const std::string text = tf ? std::string((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>()) : std::string();
					static const std::regex kPerk(R"re("perk"\s*:\s*"([^"|]+)\|([0-9A-Fa-fxX]+)")re");
					for (auto it = std::sregex_iterator(text.begin(), text.end(), kPerk); it != std::sregex_iterator(); ++it)
					{
						std::uint32_t local = 0;
						try { local = static_cast<std::uint32_t>(std::stoul((*it)[2].str(), nullptr, 16)); } catch (...) { continue; }
						auto* first = handler->LookupForm<RE::BGSPerk>(local & 0xFFFFFF, (*it)[1].str());
						if (!first) { logger::warn("perk lock: custom tree {} - perk {}|{} not found", t.value("skill", std::string()), (*it)[1].str(), (*it)[2].str()); }
						for (auto* perk = first; perk && seen.insert(perk).second; perk = perk->nextPerk) { ConditionPerk(perk, global, player); ++n; }
					}
					if (text.empty()) { logger::warn("perk lock: custom tree {} - its tree file {} could not be read; nothing gated", t.value("skill", std::string()), t.value("perkTree", std::string())); }
					logger::info("perk lock: custom tree {} - {} perks of {} conditioned on the {} group", t.value("skill", std::string()), n, plugin, groupId);
				}
			}
			catch (const std::exception& e) { logger::warn("perk lock: the custom trees in classes.json could not be read ({})", e.what()); }
			logger::info("perk lock: {} perks and {} entry points conditioned on their group's global; {} ability entries left as they are",
						 g_perks.load(), g_entries.load(), g_abilities.load());
		}

		// The group a crafting station belongs to, -1 for one that stays open.
		int StationGroup(RE::TESFurniture* a_furniture)
		{
			using B = RE::TESFurniture::WorkBenchData::BenchType;
			const auto bench = a_furniture->workBenchData.benchType.get();
			switch (bench)
			{
			case B::kSmithingWeapon:
			case B::kSmithingArmor:
				return ClassData::kWarrior;
			case B::kEnchanting:
			case B::kEnchantingExperiment:
				return ClassData::kMagic;
			case B::kAlchemy:
			case B::kAlchemyExperiment:
				return ClassData::kCovert;
			case B::kCreateObject:
				// the forge and the Skyforge; smelters, tanning racks and cooking pots share the bench type and stay open
				return (a_furniture->HasKeywordString("CraftingSmithingForge") || a_furniture->HasKeywordString("CraftingSmithingSkyforge")) ? ClassData::kWarrior : -1;
			default:
				return -1;
			}
		}

		class CraftingSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || !a_event->opening || a_event->menuName != RE::CraftingMenu::MENU_NAME) { return RE::BSEventNotifyControl::kContinue; }
				if (!settings::classes::enabled || !settings::classes::craftingGate) { return RE::BSEventNotifyControl::kContinue; }
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player) { return RE::BSEventNotifyControl::kContinue; }
				const auto handle = player->GetOccupiedFurniture();
				const auto ref = handle.get();
				auto* base = ref ? ref->GetBaseObject() : nullptr;
				auto* furniture = base ? base->As<RE::TESFurniture>() : nullptr;
				if (!furniture) { logger::debug("crafting gate: the crafting menu opened with no furniture in use - left open"); return RE::BSEventNotifyControl::kContinue; }
				const int group = StationGroup(furniture);
				if (group < 0 || Classes::HoldsGroup(group))
				{
					logger::debug("crafting gate: station {:08X} (group {}) - open", furniture->GetFormID(), group);
					return RE::BSEventNotifyControl::kContinue;
				}
				++g_refusals;
				const auto& table = ClassData::Get();
				const int craft = table.craftSkill[group];
				const char* craftName = craft >= 0 ? skilllist::kDisplayName[craft] : ClassData::kGroupId[group];
				{
					std::scoped_lock l(g_lock);
					g_lastRefusal = std::format("{:08X} ({})", furniture->GetFormID(), craftName);
				}
				logger::info("crafting gate: refused station {:08X} - the class does not hold the {} group ({})", furniture->GetFormID(), ClassData::kGroupId[group], craftName);
				if (auto* queue = RE::UIMessageQueue::GetSingleton()) { queue->AddMessage(RE::CraftingMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr); }
				static constexpr const char* kKeys[3] = { "NJ_NoticeNoSmithing", "NJ_NoticeNoEnchanting", "NJ_NoticeNoAlchemy" };
				static constexpr const char* kText[3] = { "Your class does not know the smith's craft.", "Your class does not know the enchanter's craft.", "Your class does not know the alchemist's craft." };
				const char* notice = strings::TR(kKeys[group], kText[group]);
				notify::Show(notice);
				logger::info("notice: {}", notice);
				// Combined run 78d7: with the menu refused the player stayed seated at the forge, hammering, until a move key.
				// Leave the station once the menu has gone: a main-thread task a moment later (the hide is queued first).
				std::thread([]() {
					std::this_thread::sleep_for(std::chrono::milliseconds(300));
					if (auto* tasks = SKSE::GetTaskInterface())
					{
						tasks->AddTask([]() {
							auto* pl = RE::PlayerCharacter::GetSingleton();
							if (!pl || !pl->GetOccupiedFurniture()) { return; }
							pl->StopInteractingQuick(false);
							logger::info("crafting gate: the player was taken out of the refused station");
						});
					}
				}).detach();
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		CraftingSink g_crafting;
	}

	void Install()
	{
		PerkPass();
		if (auto* ui = RE::UI::GetSingleton())
		{
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_crafting);
			logger::info("crafting gate: menu sink registered");
		}
	}

	State GetState()
	{
		State s;
		s.perksConditioned = g_perks.load();
		s.entriesConditioned = g_entries.load();
		s.abilityEntriesSkipped = g_abilities.load();
		s.craftingRefusals = g_refusals.load();
		std::scoped_lock l(g_lock);
		s.lastRefusal = g_lastRefusal;
		return s;
	}
}
