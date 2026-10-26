#include "PCH.h"

#include "ClassData.h"

#include "utils/Logger.h"

#include <nlohmann/json.hpp>

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
				for (const auto& s : c.value("titles", nlohmann::json::array())) { n.titles.push_back(s.get<std::string>()); }
				n.ability = Resolve<RE::SpellItem>(c, "ability", n.id);
				n.global = Resolve<RE::TESGlobal>(c, "global", n.id);
				n.keyword = Resolve<RE::BGSKeyword>(c, "keyword", n.id);
				if (c.contains("rankPerks"))
				{
					for (int p = 0; p < 2 && p < static_cast<int>(c["rankPerks"].size()); ++p)
					{
						for (const auto& v : c["rankPerks"][p])
						{
							auto* handler = RE::TESDataHandler::GetSingleton();
							const auto local = ParseLocalId(v);
							if (auto* perk = (handler && local) ? handler->LookupForm<RE::BGSPerk>(local, kPlugin) : nullptr) { n.rankPerks[p].push_back(perk); }
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
				g_table.titles.push_back(std::move(ti));
			}
			if (j.contains("ranks")) { g_table.rankPaths = std::clamp(j["ranks"].value("paths", 2), 1, 2); }
		}
		catch (const std::exception& e)
		{
			g_table = Table{};
			g_table.error = std::string("classes.json is not in the expected shape: ") + e.what();
			logger::error("class table: {} - the Classes module is off", g_table.error);
			return;
		}

		// Check the lineage: every option names a class, every title a title.
		for (const auto& n : g_table.classes)
		{
			for (const auto& o : n.next)
			{
				if (!Find(o)) { logger::warn("class table: {} offers \"{}\", which is not a class", n.id, o); }
			}
			for (const auto& t : n.titles)
			{
				if (!FindTitle(t)) { logger::warn("class table: {} offers the title \"{}\", which is not a title", n.id, t); }
			}
		}
		for (int g = 0; g < kGroupCount; ++g)
		{
			if (!g_table.groupGlobal[g]) { g_table.groupGlobal[g] = MakeRuntimeGlobal(kGroupId[g]); }
		}
		g_table.loaded = true;
		int starts = 0;
		for (const auto& n : g_table.classes) { starts += n.start ? 1 : 0; }
		logger::info("class table: {} classes ({} starting), {} titles, {} rank paths read from {}", g_table.classes.size(), starts, g_table.titles.size(), g_table.rankPaths, path.string());
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

	const Title* FindTitle(const std::string& a_id)
	{
		for (const auto& t : g_table.titles)
		{
			if (t.id == a_id) { return &t; }
		}
		return nullptr;
	}
}
