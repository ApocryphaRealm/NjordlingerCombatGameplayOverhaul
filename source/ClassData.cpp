#include "PCH.h"

#include "ClassData.h"

#include "utils/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace ClassData
{
	namespace
	{
		constexpr const char* kPlugin = "NjordlingerCombatGameplayOverhaul.esp";
		Table g_table;

		std::uint32_t ParseLocalId(const nlohmann::json& a_v)
		{
			try
			{
				if (a_v.is_string()) { return static_cast<std::uint32_t>(std::stoul(a_v.get<std::string>(), nullptr, 0)); }
				if (a_v.is_number_unsigned() || a_v.is_number_integer()) { return a_v.get<std::uint32_t>(); }
			}
			catch (...) {}
			return 0;
		}

		template <class T>
		T* Resolve(const nlohmann::json& a_obj, const char* a_key, const std::string& a_owner)
		{
			if (!a_obj.contains(a_key)) { return nullptr; }
			const auto local = ParseLocalId(a_obj[a_key]);
			if (!local) { return nullptr; }
			auto* handler = RE::TESDataHandler::GetSingleton();
			T* form = handler ? handler->LookupForm<T>(local, kPlugin) : nullptr;
			if (!form) { logger::warn("class table: {} {} 0x{:X} not found in {} (plugin missing or record absent) - that part is skipped", a_owner, a_key, local, kPlugin); }
			else { logger::debug("class table: {} {} -> {:08X}", a_owner, a_key, form->GetFormID()); }
			return form;
		}

		RE::TESGlobal* MakeRuntimeGlobal(const char* a_group)
		{
			auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESGlobal>();
			auto* g = factory ? factory->Create() : nullptr;
			if (!g) { logger::error("class table: no global for the {} group and none could be created - the perk lock is off for that group", a_group); return nullptr; }
			g->value = 1.0F;
			logger::info("class table: the {} group's global was created at run time (the plugin did not supply one)", a_group);
			return g;
		}
	}

	int SkillIndex(const std::string& a_iniName)
	{
		for (int i = 0; i < skilllist::kCount; ++i)
		{
			if (_stricmp(a_iniName.c_str(), skilllist::kIniName[i]) == 0) { return i; }
		}
		return -1;
	}

	int GroupOfSkill(int a_skillIndex)
	{
		if (a_skillIndex < 0 || a_skillIndex >= skilllist::kCount) { return -1; }
		for (int g = 0; g < kGroupCount; ++g)
		{
			if (g_table.groupSkills[g][a_skillIndex]) { return g; }
		}
		return -1;
	}

	void Load()
	{
		g_table = Table{};
		const auto path = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "NjordlingerCombatGameplayOverhaul" / "classes.json";
		std::ifstream in(path);
		if (!in)
		{
			g_table.error = "classes.json was not found at " + path.string();
			logger::error("class table: {} - the Classes module is off", g_table.error);
			return;
		}
		nlohmann::json j;
		try { j = nlohmann::json::parse(in, nullptr, true, true); }
		catch (const std::exception& e)
		{
			g_table.error = std::string("classes.json does not parse: ") + e.what();
			logger::error("class table: {} - the Classes module is off", g_table.error);
			return;
		}

		try
		{
			for (int g = 0; g < kGroupCount; ++g)
			{
				for (const auto& s : j.at("groups").at(kGroupId[g]))
				{
					const int idx = SkillIndex(s.get<std::string>());
					if (idx < 0) { logger::warn("class table: group {} names an unknown skill \"{}\"", kGroupId[g], s.get<std::string>()); continue; }
					g_table.groupSkills[g][idx] = true;
				}
				if (j.contains("crafts") && j["crafts"].contains(kGroupId[g])) { g_table.craftSkill[g] = SkillIndex(j["crafts"][kGroupId[g]].get<std::string>()); }
				if (j.contains("groupGlobals")) { g_table.groupGlobal[g] = Resolve<RE::TESGlobal>(j["groupGlobals"], kGroupId[g], "group"); }
			}
			for (int i = 0; i < skilllist::kCount; ++i)
			{
				if (GroupOfSkill(i) < 0) { logger::warn("class table: {} belongs to no group - it is never locked", skilllist::kDisplayName[i]); }
			}

			for (const auto& c : j.at("classes"))
			{
				ClassNode n;
				n.id = c.at("id").get<std::string>();
				n.name = c.value("name", n.id);
				n.description = c.value("description", std::string());
				n.tier = std::clamp(c.value("tier", 0), 0, 2);
				n.start = c.value("start", false);
				n.reachable = c.value("reachable", true);
				for (const auto& g : c.value("groups", nlohmann::json::array()))
				{
					for (int k = 0; k < kGroupCount; ++k)
					{
						if (g.get<std::string>() == kGroupId[k]) { n.groups[k] = true; }
					}
				}
				for (const auto& s : c.value("extraSkills", nlohmann::json::array()))
				{
					const int idx = SkillIndex(s.get<std::string>());
					if (idx >= 0) { n.extraSkills[idx] = true; }
				}
				for (const auto& s : c.value("next", nlohmann::json::array())) { n.next.push_back(s.get<std::string>()); }
				n.ability = Resolve<RE::BGSPerk>(c, "ability", n.id);
				n.global = Resolve<RE::TESGlobal>(c, "global", n.id);
				n.keyword = Resolve<RE::BGSKeyword>(c, "keyword", n.id);
				if (c.contains("ranks") && c["ranks"].is_object())
				{
					static constexpr const char* kSlot[3][2] = { { "A1", "B1" }, { "A2", "B2" }, { "A3", "B3" } };
					for (int step = 0; step < 3; ++step)
					{
						for (int ab = 0; ab < 2; ++ab)
						{
							if (!c["ranks"].contains(kSlot[step][ab])) { continue; }
							const auto& r = c["ranks"][kSlot[step][ab]];
							auto& rank = n.ranks[step][ab];
							rank.name = r.value("name", std::string(kSlot[step][ab]));
							rank.description = r.value("description", std::string());
							rank.perk = Resolve<RE::BGSPerk>(r, "perk", n.id + " rank " + kSlot[step][ab]);
							n.hasRanks = true;
						}
					}
				}
				g_table.classes.push_back(std::move(n));
			}
			for (const auto& t : j.value("titles", nlohmann::json::array()))
			{
				Title ti;
				ti.id = t.at("id").get<std::string>();
				ti.name = t.value("name", ti.id);
				ti.description = t.value("description", std::string());
				ti.quest = t.value("quest", std::string());
				ti.perk = Resolve<RE::BGSPerk>(t, "perk", ti.id);
				for (const auto& s : t.value("starts", nlohmann::json::array())) { ti.starts.push_back(s.get<std::string>()); }
				for (const auto& s : t.value("classes", nlohmann::json::array())) { ti.classes.push_back(s.get<std::string>()); }
				for (const auto& s : t.value("excludeClasses", nlohmann::json::array())) { ti.excludeClasses.push_back(s.get<std::string>()); }
				g_table.titles.push_back(std::move(ti));
			}
		}
		catch (const std::exception& e)
		{
			g_table = Table{};
			g_table.error = std::string("classes.json is not in the expected shape: ") + e.what();
			logger::error("class table: {} - the Classes module is off", g_table.error);
			return;
		}

		if (j.contains("helpers") && j["helpers"].is_object())
		{
			auto* handler = RE::TESDataHandler::GetSingleton();
			for (const auto& [name, v] : j["helpers"].items())
			{
				const auto local = ParseLocalId(v);
				auto* form = (handler && local) ? handler->LookupForm(local, kPlugin) : nullptr;
				if (!form) { logger::warn("class table: helper {} (0x{:X}) not found in {} - the effects that use it do nothing", name, local, kPlugin); continue; }
				g_table.helpers[name] = form;
			}
			logger::info("class table: {} helper records resolved", g_table.helpers.size());
		}

		// Check the lineage: every option names a class, every title a title.
		for (const auto& n : g_table.classes)
		{
			for (const auto& o : n.next)
			{
				if (!Find(o)) { logger::warn("class table: {} offers \"{}\", which is not a class", n.id, o); }
			}
			if (n.tier == 2 && !n.hasRanks) { logger::warn("class table: the Adept class {} has no rank perks - its Expert and Master upgrades offer nothing", n.id); }
		}
		for (int g = 0; g < kGroupCount; ++g)
		{
			if (!g_table.groupGlobal[g]) { g_table.groupGlobal[g] = MakeRuntimeGlobal(kGroupId[g]); }
		}
		g_table.loaded = true;
		int starts = 0;
		for (const auto& n : g_table.classes) { starts += n.start ? 1 : 0; }
		logger::info("class table: {} classes ({} starting), {} titles read from {}", g_table.classes.size(), starts, g_table.titles.size(), path.string());
	}

	const Table& Get() { return g_table; }

	const ClassNode* Find(const std::string& a_id)
	{
		for (const auto& n : g_table.classes)
		{
			if (n.id == a_id) { return &n; }
		}
		return nullptr;
	}

	RE::TESForm* Helper(const std::string& a_name)
	{
		const auto it = g_table.helpers.find(a_name);
		return it == g_table.helpers.end() ? nullptr : it->second;
	}

	const Title* FindTitle(const std::string& a_id)
	{
		for (const auto& t : g_table.titles)
		{
			if (t.id == a_id) { return &t; }
		}
		return nullptr;
	}

	bool Title::Fits(const std::string& a_start, const std::string& a_class) const
	{
		const auto in = [](const std::vector<std::string>& a_list, const std::string& a_v) { return std::find(a_list.begin(), a_list.end(), a_v) != a_list.end(); };
		if (in(excludeClasses, a_class)) { return false; }
		if (!starts.empty() && !in(starts, a_start)) { return false; }
		if (!classes.empty() && !in(classes, a_class)) { return false; }
		return true;
	}
}
