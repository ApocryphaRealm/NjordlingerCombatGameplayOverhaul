#include "PCH.h"

#include "Settings.h"

#include "utils/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// The reader and the in-place writer are Character Progression Control's (ApocryphaRealm, GPL-3.0-or-later): the
// file is read and rewritten with ordinary file I/O so comments and unknown keys survive, which Mod Organizer 2's
// virtual file system handles (rule 16).

namespace settings
{
	namespace
	{
		std::string iniPath;
		constexpr float kVanillaSkillCap = 100.0F;

		std::string SkillKey(const char* a_prefix, int a_index) { return std::string(a_prefix) + skilllist::kIniName[a_index]; }

		std::string Lower(std::string a_s)
		{
			for (char& c : a_s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
			return a_s;
		}

		std::string Trim(const std::string& a_s)
		{
			const auto b = a_s.find_first_not_of(" \t\r\n");
			if (b == std::string::npos) { return {}; }
			const auto e = a_s.find_last_not_of(" \t\r\n");
			return a_s.substr(b, e - b + 1);
		}

		bool ParseBool(const std::string& a_text, bool& a_out)
		{
			const std::string v = Lower(Trim(a_text));
			if (v == "1" || v == "true" || v == "yes") { a_out = true; return true; }
			if (v == "0" || v == "false" || v == "no") { a_out = false; return true; }
			return false;
		}

		bool ParseFloat(const std::string& a_text, float& a_out)
		{
			try { a_out = std::stof(Trim(a_text)); return true; } catch (...) { return false; }
		}

		bool ParseInt(const std::string& a_text, int& a_out)
		{
			try { a_out = std::stoi(Trim(a_text), nullptr, 0); return true; } catch (...) { return false; }
		}

		bool ParseUInt(const std::string& a_text, std::uint32_t& a_out)
		{
			try { a_out = static_cast<std::uint32_t>(std::stoull(Trim(a_text), nullptr, 0)); return true; } catch (...) { return false; }
		}

		bool ParseText(const std::string& a_text, std::string& a_out) { a_out = Trim(a_text); return true; }

		// key:section (both lower case) -> value; plus the [QuestRewardRows] rows with their keys as written.
		struct FileKeys
		{
			std::map<std::string, std::string> keys;
			std::map<std::string, std::string> rows;
		};

		FileKeys ReadKeys(const std::string& a_path)
		{
			FileKeys out;
			std::ifstream in(a_path);
			if (!in) { return out; }
			std::string line, section;
			while (std::getline(in, line))
			{
				const std::string t = Trim(line);
				if (t.empty() || t[0] == ';' || t[0] == '#') { continue; }
				if (t.front() == '[' && t.back() == ']') { section = Lower(t.substr(1, t.size() - 2)); continue; }
				const auto eq = t.find('=');
				if (eq == std::string::npos) { continue; }
				const std::string key = Trim(t.substr(0, eq));
				const std::string value = Trim(t.substr(eq + 1));
				if (section == "questrewardrows") { out.rows[key] = value; continue; }
				out.keys[Lower(key) + ":" + section] = value;
			}
			return out;
		}

		// The compiled defaults - the same values as the shipped INI (rule 16).
		void SetDefaults()
		{
			debug::logLevel = 2;
			skills::overrideCaps = false;
			skillexp::overrideRates = false;
			skillexp::toLevelMult = 1.0F;
			for (int i = 0; i < skilllist::kCount; ++i)
			{
				skills::cap[i] = kVanillaSkillCap;
				skillexp::mult[i] = 1.0F;
			}
			levelup::overrideRewards = true;
			levelup::perksByLevel = { { 1, 1 } };
			levelup::healthPerLevel = 0.0F;
			levelup::magickaPerLevel = 0.0F;
			levelup::staminaPerLevel = 0.0F;
			classes::enabled = true;
			classes::levelsPerTier = 10;
			classes::xpPerIncrease = 1.0F;
			classes::xpBase = 2.0F;
			classes::xpPerLevel = 0.2F;
			classes::lockPerks = true;
			classes::lockSkills = true;
			classes::craftingGate = true;
			classes::raceMenuClass = true;
			classes::hideLockedTrees = true;
			questrewards::enabled = true;
			questrewards::overrides.clear();
			gear::sets = true;
			gear::uniques = true;
			gear::setStrength = 1.0F;
			gear::uniqueStrength = 1.0F;
			stones::enabled = true;
			stones::nameSlots = true;
			stones::nameDragonLetters = false;
			stones::weaponGlow = true;
		}

		void Clamp()
		{
			for (int i = 0; i < skilllist::kCount; ++i)
			{
				skills::cap[i] = std::clamp(skills::cap[i], 0.0F, 1000.0F);
				skillexp::mult[i] = std::clamp(skillexp::mult[i], 0.0F, 10.0F);
			}
			skillexp::toLevelMult = std::clamp(skillexp::toLevelMult, 0.0F, 10.0F);
			levelup::healthPerLevel = std::clamp(levelup::healthPerLevel, 0.0F, 100.0F);
			levelup::magickaPerLevel = std::clamp(levelup::magickaPerLevel, 0.0F, 100.0F);
			levelup::staminaPerLevel = std::clamp(levelup::staminaPerLevel, 0.0F, 100.0F);
			classes::levelsPerTier = std::clamp(classes::levelsPerTier, 1, 50);
			classes::xpPerIncrease = std::clamp(classes::xpPerIncrease, 0.0F, 10.0F);
			classes::xpBase = std::clamp(classes::xpBase, 0.1F, 50.0F);
			classes::xpPerLevel = std::clamp(classes::xpPerLevel, 0.0F, 10.0F);
			gear::setStrength = std::clamp(gear::setStrength, 0.25F, 2.0F);
			gear::uniqueStrength = std::clamp(gear::uniqueStrength, 0.25F, 2.0F);
		}

		bool LoadFileValues(const std::string& a_path)
		{
			if (!std::filesystem::exists(a_path))
			{
				logger::warn("settings file not found at {}; keeping the values in hand", a_path);
				return false;
			}
			const auto f = ReadKeys(a_path);
			auto get = [&](const std::string& a_key, auto& a_out, auto a_parse) {
				const auto it = f.keys.find(a_key);
				if (it == f.keys.end()) { logger::debug("INI key {} missing; keeping current value", a_key); return; }
				if (!a_parse(it->second, a_out)) { logger::warn("INI value \"{}\" for {} is not valid; keeping current value", it->second, a_key); }
			};
			get("uloglevel:debug", debug::logLevel, ParseUInt);

			get("boverrideskillcaps:skills", skills::overrideCaps, ParseBool);
			get("boverrideskillexp:skillexperience", skillexp::overrideRates, ParseBool);
			get("fskilltolevelmult:skillexperience", skillexp::toLevelMult, ParseFloat);
			for (int i = 0; i < skilllist::kCount; ++i)
			{
				get(Lower(SkillKey("fCap", i)) + ":skills", skills::cap[i], ParseFloat);
				get(Lower(SkillKey("fMult", i)) + ":skillexperience", skillexp::mult[i], ParseFloat);
			}

			get("boverrideleveluprewards:levelup", levelup::overrideRewards, ParseBool);
			{
				std::string table;
				get("sperksbylevel:levelup", table, ParseText);
				if (!table.empty() && !levelup::ParsePerksTable(table)) { logger::warn("INI sPerksByLevel \"{}\" is not valid; keeping the table in hand", table); }
			}
			get("fhealthperlevel:levelup", levelup::healthPerLevel, ParseFloat);
			get("fmagickaperlevel:levelup", levelup::magickaPerLevel, ParseFloat);
			get("fstaminaperlevel:levelup", levelup::staminaPerLevel, ParseFloat);

			get("benableclasses:classes", classes::enabled, ParseBool);
			get("iclasslevelspertier:classes", classes::levelsPerTier, ParseInt);
			get("fclassxpperskillincrease:classes", classes::xpPerIncrease, ParseFloat);
			get("fclassxpbase:classes", classes::xpBase, ParseFloat);
			get("fclassxpperlevel:classes", classes::xpPerLevel, ParseFloat);
			get("blockperks:classes", classes::lockPerks, ParseBool);
			get("blockskills:classes", classes::lockSkills, ParseBool);
			get("bcraftinggate:classes", classes::craftingGate, ParseBool);
			get("bracemenuclass:classes", classes::raceMenuClass, ParseBool);
			get("bhidelockedtrees:classes", classes::hideLockedTrees, ParseBool);

			get("benablequestrewards:questrewards", questrewards::enabled, ParseBool);
			get("bsetbonuses:gear", gear::sets, ParseBool);
			get("buniqueeffects:gear", gear::uniques, ParseBool);
			get("fsetstrength:gear", gear::setStrength, ParseFloat);
			get("funiquestrength:gear", gear::uniqueStrength, ParseFloat);
			get("benablesoulstones:soulstones", stones::enabled, ParseBool);
			get("bnameslots:soulstones", stones::nameSlots, ParseBool);
			get("bnamedragonletters:soulstones", stones::nameDragonLetters, ParseBool);
			get("bweaponglow:soulstones", stones::weaponGlow, ParseBool);
			questrewards::overrides.clear();
			for (const auto& [id, value] : f.rows)
			{
				int points = 0;
				if (ParseInt(value, points)) { questrewards::overrides[id] = std::clamp(points, 0, 20); }
				else { logger::warn("INI [QuestRewardRows] {}=\"{}\" is not a whole number; row ignored", id, value); }
			}
			Clamp();
			logger::debug("settings read from {}: caps {}, rates {}, level-up control {}, perks \"{}\", gains {:.0f}/{:.0f}/{:.0f}, classes {}, N {}, quest rows overridden {}",
						  a_path, skills::overrideCaps, skillexp::overrideRates, levelup::overrideRewards, levelup::PerksTableText(),
						  levelup::healthPerLevel, levelup::magickaPerLevel, levelup::staminaPerLevel, classes::enabled, classes::levelsPerTier,
						  questrewards::overrides.size());
			return true;
		}

		bool WriteKey(std::vector<std::string>& a_lines, const char* a_section, const char* a_key, const std::string& a_value)
		{
			const std::string wantSection = Lower(a_section);
			const std::string wantKey = Lower(a_key);
			std::string section;
			std::size_t sectionEnd = 0;
			bool sawSection = false;
			for (std::size_t i = 0; i < a_lines.size(); ++i)
			{
				const std::string t = Trim(a_lines[i]);
				if (!t.empty() && t.front() == '[' && t.back() == ']')
				{
					section = Lower(t.substr(1, t.size() - 2));
					if (section == wantSection) { sawSection = true; sectionEnd = i + 1; }
					continue;
				}
				if (section != wantSection) { continue; }
				sectionEnd = i + 1;
				const auto eq = t.find('=');
				if (eq == std::string::npos) { continue; }
				if (Lower(Trim(t.substr(0, eq))) == wantKey)
				{
					a_lines[i] = std::string(a_key) + "=" + a_value;
					return true;
				}
			}
			const std::string entry = std::string(a_key) + "=" + a_value;
			if (sawSection) { a_lines.insert(a_lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd), entry); }
			else
			{
				if (!a_lines.empty() && !Trim(a_lines.back()).empty()) { a_lines.push_back(""); }
				a_lines.push_back("[" + std::string(a_section) + "]");
				a_lines.push_back(entry);
			}
			logger::debug("Save: added {} to [{}]", a_key, a_section);
			return true;
		}

		std::string FormatFloat(float a_v)
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.2f", a_v);
			return buf;
		}
	}

	void Init(const std::string& a_iniFileName)
	{
		iniPath = (std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / a_iniFileName).string();
		SetDefaults();
		LoadFileValues(iniPath);
	}

	bool Reload()
	{
		const bool ok = LoadFileValues(iniPath);
		ApplyLogLevel();
		return ok;
	}

	bool Save()
	{
		std::vector<std::string> lines;
		{
			std::ifstream in(iniPath);
			std::string line;
			while (in && std::getline(in, line)) { lines.push_back(line); }
		}
		bool ok = true;
		ok &= WriteKey(lines, "Debug", "uLogLevel", std::to_string(debug::logLevel));
		ok &= WriteKey(lines, "Skills", "bOverrideSkillCaps", skills::overrideCaps ? "1" : "0");
		for (int i = 0; i < skilllist::kCount; ++i) { ok &= WriteKey(lines, "Skills", SkillKey("fCap", i).c_str(), FormatFloat(skills::cap[i])); }
		ok &= WriteKey(lines, "SkillExperience", "bOverrideSkillExp", skillexp::overrideRates ? "1" : "0");
		ok &= WriteKey(lines, "SkillExperience", "fSkillToLevelMult", FormatFloat(skillexp::toLevelMult));
		for (int i = 0; i < skilllist::kCount; ++i) { ok &= WriteKey(lines, "SkillExperience", SkillKey("fMult", i).c_str(), FormatFloat(skillexp::mult[i])); }
		ok &= WriteKey(lines, "LevelUp", "bOverrideLevelUpRewards", levelup::overrideRewards ? "1" : "0");
		ok &= WriteKey(lines, "LevelUp", "sPerksByLevel", levelup::PerksTableText());
		ok &= WriteKey(lines, "LevelUp", "fHealthPerLevel", FormatFloat(levelup::healthPerLevel));
		ok &= WriteKey(lines, "LevelUp", "fMagickaPerLevel", FormatFloat(levelup::magickaPerLevel));
		ok &= WriteKey(lines, "LevelUp", "fStaminaPerLevel", FormatFloat(levelup::staminaPerLevel));
		ok &= WriteKey(lines, "Classes", "bEnableClasses", classes::enabled ? "1" : "0");
		ok &= WriteKey(lines, "Classes", "iClassLevelsPerTier", std::to_string(classes::levelsPerTier));
		ok &= WriteKey(lines, "Classes", "fClassXpPerSkillIncrease", FormatFloat(classes::xpPerIncrease));
		ok &= WriteKey(lines, "Classes", "fClassXpBase", FormatFloat(classes::xpBase));
		ok &= WriteKey(lines, "Classes", "fClassXpPerLevel", FormatFloat(classes::xpPerLevel));
		ok &= WriteKey(lines, "Classes", "bLockPerks", classes::lockPerks ? "1" : "0");
		ok &= WriteKey(lines, "Classes", "bLockSkills", classes::lockSkills ? "1" : "0");
		ok &= WriteKey(lines, "Classes", "bCraftingGate", classes::craftingGate ? "1" : "0");
		ok &= WriteKey(lines, "Classes", "bRaceMenuClass", classes::raceMenuClass ? "1" : "0");
		ok &= WriteKey(lines, "Classes", "bHideLockedTrees", classes::hideLockedTrees ? "1" : "0");
		ok &= WriteKey(lines, "QuestRewards", "bEnableQuestRewards", questrewards::enabled ? "1" : "0");
		ok &= WriteKey(lines, "Gear", "bSetBonuses", gear::sets ? "1" : "0");
		ok &= WriteKey(lines, "Gear", "bUniqueEffects", gear::uniques ? "1" : "0");
		ok &= WriteKey(lines, "Gear", "fSetStrength", FormatFloat(gear::setStrength));
		ok &= WriteKey(lines, "Gear", "fUniqueStrength", FormatFloat(gear::uniqueStrength));
		ok &= WriteKey(lines, "SoulStones", "bEnableSoulStones", stones::enabled ? "1" : "0");
		ok &= WriteKey(lines, "SoulStones", "bNameSlots", stones::nameSlots ? "1" : "0");
		ok &= WriteKey(lines, "SoulStones", "bNameDragonLetters", stones::nameDragonLetters ? "1" : "0");
		ok &= WriteKey(lines, "SoulStones", "bWeaponGlow", stones::weaponGlow ? "1" : "0");
		for (const auto& [id, points] : questrewards::overrides) { ok &= WriteKey(lines, "QuestRewardRows", id.c_str(), std::to_string(points)); }

		std::ofstream out(iniPath, std::ios::trunc);
		if (!out)
		{
			logger::error("Save: could not open {} for writing", iniPath);
			return false;
		}
		for (const auto& l : lines) { out << l << '\n'; }
		out.close();
		if (!out) { logger::error("Save: writing {} failed", iniPath); return false; }
		logger::info("settings saved to {}", iniPath);
		return ok;
	}

	void RestoreDefaults()
	{
		SetDefaults();
		ApplyLogLevel();
		logger::info("settings: compiled defaults restored (not written until Save)");
	}

	void ApplyLogLevel()
	{
		const auto lvl = static_cast<spdlog::level::level_enum>(std::clamp<std::uint32_t>(debug::logLevel, 0u, 6u));
		SKSE::log::set_level(lvl, lvl);
	}

	const std::string& GetIniPath() { return iniPath; }

	namespace levelup
	{
		int PerksAtLevel(std::uint16_t a_level)
		{
			int perks = 1;
			for (const auto& row : perksByLevel)
			{
				if (row.fromLevel <= a_level) { perks = row.perks; } else { break; }
			}
			return perks;
		}

		std::string PerksTableText()
		{
			std::string out;
			for (const auto& row : perksByLevel)
			{
				if (!out.empty()) { out += ","; }
				out += std::to_string(row.fromLevel) + ":" + std::to_string(row.perks);
			}
			return out;
		}

		bool ParsePerksTable(const std::string& a_text)
		{
			std::vector<PerkRow> rows;
			std::size_t pos = 0;
			while (pos <= a_text.size())
			{
				const auto comma = a_text.find(',', pos);
				const std::string item = a_text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
				pos = comma == std::string::npos ? a_text.size() + 1 : comma + 1;
				const auto colon = item.find(':');
				if (colon == std::string::npos) { if (item.find_first_not_of(" \t") == std::string::npos) { continue; } return false; }
				char* end = nullptr;
				const long lvl = std::strtol(item.c_str(), &end, 10);
				if (end == item.c_str() || lvl < 1 || lvl > 1000) { return false; }
				const long perks = std::strtol(item.c_str() + colon + 1, &end, 10);
				if (end == item.c_str() + colon + 1 || perks < 0 || perks > 20) { return false; }
				rows.push_back({ static_cast<std::uint16_t>(lvl), static_cast<std::uint8_t>(perks) });
			}
			if (rows.empty()) { return false; }
			std::sort(rows.begin(), rows.end(), [](const PerkRow& a, const PerkRow& b) { return a.fromLevel < b.fromLevel; });
			perksByLevel = std::move(rows);
			return true;
		}
	}
}
