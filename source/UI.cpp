#include "PCH.h"

#include "UI.h"

#include "SKSEMenuFramework.h"

#include "ClassData.h"
#include "Classes.h"
#include "Compat.h"
#include "Gear.h"
#include "LevelUpGains.h"
#include "Lockouts.h"
#include "Patches.h"
#include "PreciseSlider.h"
#include "QuestRewards.h"
#include "Settings.h"
#include "SkillList.h"
#include "Skills.h"
#include "SoulStones.h"

#include "utils/Logger.h"
#include "utils/Strings.h"
#include "utils/Toggle.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace UI
{
	namespace
	{
		std::mutex g_statusLock;
		std::string g_status;
		std::string g_statusCopy;
		std::string g_selectedSlider;
		char g_perksTable[128]{};
		bool g_inRunesmithWindow = false;               // the Soul Stones page drawn inside the runesmith's window
		SKSEMenuFramework::Model::WindowInterface* g_runesmithWindow = nullptr;
		bool g_perksTableSeeded = false;

		void SetStatus(std::string a_text)
		{
			std::scoped_lock l(g_statusLock);
			g_status = std::move(a_text);
		}

		std::string Status()
		{
			std::scoped_lock l(g_statusLock);
			return g_status;
		}

		constexpr const char* const kLogLevelKeys[] = { "NJ_Log_Trace", "NJ_Log_Debug", "NJ_Log_Info", "NJ_Log_Warning", "NJ_Log_Error", "NJ_Log_Critical", "NJ_Log_Off" };
		constexpr const char* const kLogLevelLabels[] = { "Trace", "Debug", "Info", "Warning", "Error", "Critical", "Off" };
		constexpr int kLogLevelCount = 7;

		constexpr const char* const kSkillKeys[] = {
			"NJ_Skill_OneHanded", "NJ_Skill_TwoHanded", "NJ_Skill_Archery", "NJ_Skill_Block", "NJ_Skill_Smithing", "NJ_Skill_HeavyArmor",
			"NJ_Skill_LightArmor", "NJ_Skill_Pickpocket", "NJ_Skill_Lockpicking", "NJ_Skill_Sneak", "NJ_Skill_Alchemy", "NJ_Skill_Speech",
			"NJ_Skill_Alteration", "NJ_Skill_Conjuration", "NJ_Skill_Destruction", "NJ_Skill_Illusion", "NJ_Skill_Restoration", "NJ_Skill_Enchanting"
		};
		constexpr const char* const kSkillLabels[] = {
			"One-handed", "Two-handed", "Archery", "Block", "Smithing", "Heavy Armor",
			"Light Armor", "Pickpocket", "Lockpicking", "Sneak", "Alchemy", "Speech",
			"Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Enchanting"
		};
		static_assert(sizeof(kSkillKeys) / sizeof(kSkillKeys[0]) == static_cast<std::size_t>(skilllist::kCount));
		constexpr const char* const kGroupKeys[] = { "NJ_Group_Warrior", "NJ_Group_Magic", "NJ_Group_Covert" };
		constexpr const char* const kGroupLabels[] = { "Warrior", "Magic", "Covert" };

		// Game work goes to the main thread; nothing queued here touches Scaleform (gate rule
		// scaleform-calls-only-on-the-ui-thread).
		void OnMainThread(std::function<void()> a_task)
		{
			if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask(std::move(a_task)); }
		}

		bool HasRequiredExports()
		{
			constexpr const char* required[] = {
				"AddSectionItem",
				"igTextV",
				"igTextDisabledV",
				"igTextWrappedV",
				"igSetTooltipV",
				"igSeparatorText",
				"igCombo_Str_arr",
				"igSliderFloat",
				"igSliderInt",
				"igIsKeyDown_Nil",       // PreciseSlider.h
				"igIsMouseDown_Nil",     // PreciseSlider.h
				"igIsKeyPressed_Bool",
				"igIsItemClicked",
				"igIsItemActive",
				"igIsItemHovered",
				"igButton",
				"igSameLine",
				"igSpacing",
				"igPushItemWidth",
				"igPopItemWidth",
				"igPushID_Str",
				"igPopID",
				"igInputText",
				"igBeginDisabled",
				"igEndDisabled",
				"igGetCursorScreenPos",
				"igGetWindowDrawList",
				"igGetFrameHeight",
				"igInvisibleButton",
				"ImDrawList_AddRectFilled",
				"ImDrawList_AddCircleFilled",
				"igSelectable_Bool",     // the Soul Stones page
				"igBeginChild_Str",
				"igEndChild",
				"igTextColoredV",
				"igSmallButton",
				"igBegin",               // the runesmith's window
				"igEnd",
				"AddWindow"
			};
			for (const char* name : required)
			{
				if (!GetMenuFrameworkFunction<void*>(name))
				{
					logger::warn("The menu framework does not export \"{}\"", name);
					return false;
				}
			}
			return true;
		}

		void HelpMarker(const char* a_description)
		{
			ImGuiMCP::SameLine();
			ImGuiMCP::TextDisabled("%s", strings::TR("NJ_HelpMark", "(?)"));
			if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::SetTooltip("%s", a_description); }
		}

		bool NudgeableSlider(const char* a_label, float* a_value, float a_min, float a_max, const char* a_format, float a_step)
		{
			// rule 68: precise::SliderFloat holds a nudge to one unit of the shown digit and clamps a typed value
			bool changed = precise::SliderFloat(a_label, a_value, a_min, a_max, a_format);
			if (ImGuiMCP::IsItemClicked() || ImGuiMCP::IsItemActive()) { g_selectedSlider = a_label; }
			if (g_selectedSlider == a_label)
			{
				float nudge = 0.0F;
				if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_LeftArrow) || ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_DownArrow)) { nudge -= a_step; }
				if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_RightArrow) || ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_UpArrow)) { nudge += a_step; }
				if (nudge != 0.0F)
				{
					*a_value = std::clamp(*a_value + nudge, a_min, a_max);
					changed = true;
				}
			}
			return changed;
		}

		bool NudgeableSliderInt(const char* a_label, int* a_value, int a_min, int a_max)
		{
			bool changed = precise::SliderInt(a_label, a_value, a_min, a_max, "%d");
			if (ImGuiMCP::IsItemClicked() || ImGuiMCP::IsItemActive()) { g_selectedSlider = a_label; }
			if (g_selectedSlider == a_label)
			{
				int nudge = 0;
				if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_LeftArrow) || ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_DownArrow)) { nudge -= 1; }
				if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_RightArrow) || ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_UpArrow)) { nudge += 1; }
				if (nudge != 0)
				{
					*a_value = std::clamp(*a_value + nudge, a_min, a_max);
					changed = true;
				}
			}
			return changed;
		}

		void RenderButtons()
		{
			ImGuiMCP::SeparatorText("");
			if (ImGuiMCP::Button(strings::TR("NJ_SaveBtn", "Save")))
			{
				SetStatus(strings::TR("NJ_StatusSaving", "Saving..."));
				OnMainThread([]() {
					SetStatus(settings::Save() ? strings::TR("NJ_StatusSaved", "Settings saved.") : strings::TR("NJ_StatusSaveFail", "Could not write the INI. See the log for why."));
				});
			}
			HelpMarker(strings::TR("NJ_HelpSave", "Writes every setting of every tab to the plugin's INI so it survives a restart."));
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button(strings::TR("NJ_ReloadBtn", "Reload from INI")))
			{
				SetStatus(strings::TR("NJ_StatusReloading", "Reloading..."));
				OnMainThread([]() {
					const bool ok = settings::Reload();
					g_perksTableSeeded = false;
					Classes::RequestApply();
					Gear::RequestApply();
					SetStatus(ok ? strings::TR("NJ_StatusReloaded", "Settings reloaded from the INI.") : strings::TR("NJ_StatusReloadFail", "Could not read the INI. See the log for why."));
				});
			}
			HelpMarker(strings::TR("NJ_HelpReload", "Throws away any change made here since the last save and re-reads the INI from disk."));
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button(strings::TR("NJ_RestoreBtn", "Restore defaults")))
			{
				OnMainThread([]() {
					settings::RestoreDefaults();
					g_perksTableSeeded = false;
					Classes::RequestApply();
					Gear::RequestApply();
				});
				SetStatus(strings::TR("NJ_StatusRestored", "Defaults restored. Press Save to keep them."));
			}
			HelpMarker(strings::TR("NJ_HelpRestore", "Puts every setting back to its fresh-install value. Nothing is written until you press Save."));
			const auto status = Status();
			if (!status.empty()) { ImGuiMCP::TextWrapped("%s", status.c_str()); }
		}

		void RenderPatches()
		{
			ImGuiMCP::SeparatorText(strings::TR("NJ_PatchesTitle", "Engine patches"));
			for (const auto& g : Patches::All())
			{
				ImGuiMCP::Text("%s: %s", g.name.c_str(), g.installed ? strings::TR("NJ_PatchOn", "attached") : strings::TR("NJ_PatchOff", "not attached"));
				if (!g.status.empty()) { HelpMarker(g.status.c_str()); }
			}
			for (const auto& d : Compat::All())
			{
				if (d.present && d.name == "Character Progression Control")
				{
					ImGuiMCP::TextWrapped("%s", strings::TR("NJ_CompatCpc", "Character Progression Control is loaded beside this mod. Both hook the same game code - disable it; this mod replaces it."));
				}
				if (!d.present && d.name == "NjordlingerCombatGameplayOverhaul.esp")
				{
					ImGuiMCP::TextWrapped("%s", strings::TR("NJ_CompatNoEsp", "NjordlingerCombatGameplayOverhaul.esp is not loaded: class abilities are missing. Enable the plugin."));
				}
			}
		}
	}

	const char* LastStatus()
	{
		std::scoped_lock l(g_statusLock);
		g_statusCopy = g_status;
		return g_statusCopy.c_str();
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled())
		{
			logger::info("No menu framework is installed; settings will be read from the INI only, and the starting class cannot be chosen outside the race menu");
			return;
		}
		if (!HasRequiredExports())
		{
			logger::warn("The installed menu framework is older than this plugin's settings menu needs. Update the Apocrypha Menu Framework.");
			return;
		}
		SKSEMenuFramework::SetSection("Njordlinger Combat Gameplay Overhaul");
		SKSEMenuFramework::AddSectionItem("Progression", Pages::Progression);
		SKSEMenuFramework::AddSectionItem("Classes", Pages::Classes);
		SKSEMenuFramework::AddSectionItem("Quest Rewards", Pages::QuestRewards);
		SKSEMenuFramework::AddSectionItem("Gear", Pages::Gear);
		SKSEMenuFramework::AddSectionItem("Soul Stones", Pages::SoulStones);
		// the runesmith's window: the same socketing page, opened when his "Set my soul stones." line ends
		g_runesmithWindow = SKSEMenuFramework::AddWindow(RunesmithWindow, true);
		::SoulStones::SetWindowOpener([](bool a_open) { if (g_runesmithWindow) { g_runesmithWindow->IsOpen = a_open; } });
		logger::info("Registered the settings pages with the menu framework");
	}

	void __stdcall Pages::Progression()
	{
		strings::Tick();
		using namespace settings;
		ImGuiMCP::TextWrapped("%s", strings::TR("NJ_Intro", "Changes apply as soon as you make them. Press Save to keep them for the next time you play."));
		ImGuiMCP::PushItemWidth(260.0F);

		ImGuiMCP::SeparatorText(strings::TR("NJ_LevelUpTitle", "What a level up grants"));
		ImGuiMCP::Toggle(strings::TR("NJ_ControlLevelUp", "Control what a level up grants"), &levelup::overrideRewards);
		HelpMarker(strings::TR("NJ_HelpControlLevelUp", "On: the perk points come from the table below and the attribute gains from the three sliders. Off: exactly what the game grants."));
		if (!g_perksTableSeeded)
		{
			const auto text = levelup::PerksTableText();
			strncpy_s(g_perksTable, text.c_str(), sizeof(g_perksTable) - 1);
			g_perksTableSeeded = true;
		}
		if (ImGuiMCP::InputText(strings::TR("NJ_PerksByLevel", "Perks by level"), g_perksTable, sizeof(g_perksTable)))
		{
			const std::string text = g_perksTable;
			OnMainThread([text]() {
				if (!levelup::ParsePerksTable(text)) { SetStatus(strings::TR("NJ_StatusBadTable", "That perk table is not valid - write level:perks pairs, for example 1:1,20:2.")); }
			});
		}
		HelpMarker(strings::TR("NJ_HelpPerksByLevel", "From each listed level on, that many perk points per level up. 1:1 is vanilla; 1:1,30:2 grants two from level 30."));
		NudgeableSlider(strings::TR("NJ_HealthPerLevel", "Health per level"), &levelup::healthPerLevel, 0.0F, 100.0F, "%.0f", 1.0F);
		NudgeableSlider(strings::TR("NJ_MagickaPerLevel", "Magicka per level"), &levelup::magickaPerLevel, 0.0F, 100.0F, "%.0f", 1.0F);
		NudgeableSlider(strings::TR("NJ_StaminaPerLevel", "Stamina per level"), &levelup::staminaPerLevel, 0.0F, 100.0F, "%.0f", 1.0F);
		HelpMarker(strings::TR("NJ_HelpGains", "At 0, 0 and 0 the attribute choice is skipped and a level up simply grants its perk points: attributes grow from what you do."));
		const auto lug = LevelUpGains::GetState();
		ImGuiMCP::TextDisabled(strings::TR("NJ_LevelUpReadout", "Level ups this session: %u - attribute picks skipped: %u"), lug.levelUps, lug.picksSkipped);

		ImGuiMCP::SeparatorText(strings::TR("NJ_SkillsTitle", "Skills"));
		ImGuiMCP::Toggle(strings::TR("NJ_ControlCaps", "Control skill caps"), &skills::overrideCaps);
		HelpMarker(strings::TR("NJ_HelpCaps", "On: each skill stops at its cap below. Off: 100, as in vanilla. A skill your class does not hold stays at its starting value either way."));
		ImGuiMCP::Toggle(strings::TR("NJ_ControlRates", "Control skill experience rates"), &skillexp::overrideRates);
		HelpMarker(strings::TR("NJ_HelpRates", "On: each use of a skill pays vanilla times its rate below, and a skill increase pays vanilla times the level multiplier."));
		NudgeableSlider(strings::TR("NJ_ToLevelMult", "Skill increase -> level multiplier"), &skillexp::toLevelMult, 0.0F, 5.0F, "%.2f", 0.01F);
		const auto ss = Skills::GetState();
		for (int i = 0; i < skilllist::kCount; ++i)
		{
			ImGuiMCP::PushID(kSkillKeys[i]);
			const char* name = strings::TR(kSkillKeys[i], kSkillLabels[i]);
			ImGuiMCP::Text("%s", name);
			if (ss.readable)
			{
				ImGuiMCP::SameLine();
				ImGuiMCP::TextDisabled(strings::TR("NJ_SkillReadout", "level %.0f - stops at %.0f"), ss.skill[i].level, Skills::EffectiveCap(i));
			}
			NudgeableSlider(strings::TR("NJ_Cap", "Cap"), &skills::cap[i], 0.0F, 1000.0F, "%.0f", 1.0F);
			NudgeableSlider(strings::TR("NJ_Rate", "Rate"), &skillexp::mult[i], 0.0F, 10.0F, "%.2f", 0.01F);
			ImGuiMCP::PopID();
		}

		RenderPatches();

		ImGuiMCP::SeparatorText(strings::TR("NJ_Debug", "Debug"));
		int level = std::clamp(static_cast<int>(debug::logLevel), 0, kLogLevelCount - 1);
		std::vector<std::string> store;
		for (int i = 0; i < kLogLevelCount; ++i) { store.emplace_back(strings::TR(kLogLevelKeys[i], kLogLevelLabels[i])); }
		std::vector<const char*> items;
		for (const auto& s : store) { items.push_back(s.c_str()); }
		if (ImGuiMCP::Combo(strings::TR("NJ_LogLevel", "Log level"), &level, items.data(), kLogLevelCount))
		{
			debug::logLevel = static_cast<std::uint32_t>(level);
			ApplyLogLevel();
		}
		HelpMarker(strings::TR("NJ_HelpLogLevel", "Applies immediately. The log is NjordlingerCombatGameplayOverhaul.log in the SKSE folder under My Games."));
		ImGuiMCP::PopItemWidth();
		RenderButtons();
	}

	void __stdcall Pages::Classes()
	{
		strings::Tick();
		using namespace settings;
		ImGuiMCP::PushItemWidth(260.0F);
		const auto s = Classes::GetSnapshot();
		if (!s.tableLoaded)
		{
			ImGuiMCP::TextWrapped("%s", strings::TR("NJ_NoTable", "The class table could not be read, so classes are off. See the log."));
			ImGuiMCP::TextWrapped("%s", s.tableError.c_str());
		}

		ImGuiMCP::SeparatorText(strings::TR("NJ_YourClass", "Your class"));
		ImGuiMCP::Text("%s", s.line.c_str());
		if (s.chosen)
		{
			ImGuiMCP::TextDisabled(strings::TR("NJ_ClassXp", "Class experience %.1f of %.1f to the next class level"), s.xp, s.xpToNext);
		}
		if (s.tableLoaded && s.enabled && !s.chosen)
		{
			ImGuiMCP::TextWrapped("%s", strings::TR("NJ_ChooseStart", "Choose your starting class. The choice is permanent: the skills of the other groups stay at their starting values."));
			for (const auto& c : Classes::StartClasses())
			{
				ImGuiMCP::PushID(c.id.c_str());
				if (ImGuiMCP::Button(c.name.c_str()))
				{
					std::string why;
					SetStatus(Classes::RequestChooseStart(c.id, why) ? std::string(strings::TR("NJ_StatusChoosing", "Choosing...")) : why);
				}
				ImGuiMCP::SameLine();
				ImGuiMCP::TextWrapped("%s", c.description.c_str());
				ImGuiMCP::PopID();
			}
		}
		if (s.upgradeDue)
		{
			ImGuiMCP::SeparatorText(strings::TR("NJ_Evolve", "Evolve your class - choose one; the other is gone for good"));
			for (int i = 0; i < static_cast<int>(s.options.size()); ++i)
			{
				const auto& o = s.options[i];
				ImGuiMCP::PushID(i);
				if (!o.available) { ImGuiMCP::BeginDisabled(true); }
				if (ImGuiMCP::Button(o.label.c_str()))
				{
					std::string why;
					SetStatus(Classes::RequestChooseUpgrade(i, why) ? std::string(strings::TR("NJ_StatusChoosing", "Choosing...")) : why);
				}
				if (!o.available) { ImGuiMCP::EndDisabled(); }
				ImGuiMCP::SameLine();
				ImGuiMCP::TextWrapped("%s%s%s", o.description.c_str(), o.available ? "" : " - ", o.why.c_str());
				ImGuiMCP::PopID();
			}
		}
		if (s.chosen)
		{
			ImGuiMCP::SeparatorText(strings::TR("NJ_GroupsTitle", "Skill groups"));
			for (int g = 0; g < 3; ++g)
			{
				ImGuiMCP::Text("%s: %s", strings::TR(kGroupKeys[g], kGroupLabels[g]), s.groupsHeld[g] ? strings::TR("NJ_Held", "held") : strings::TR("NJ_Locked", "locked"));
			}
			for (int i = 0; i < skilllist::kCount; ++i)
			{
				if (s.held[i]) { continue; }
				ImGuiMCP::TextDisabled(strings::TR("NJ_LockedSkill", "%s - locked at %.0f"), strings::TR(kSkillKeys[i], kSkillLabels[i]), s.lockedCap[i]);
			}
		}

		ImGuiMCP::SeparatorText(strings::TR("NJ_ClassSettings", "Class settings"));
		if (ImGuiMCP::Toggle(strings::TR("NJ_EnableClasses", "Classes"), &classes::enabled)) { Classes::RequestApply(); }
		HelpMarker(strings::TR("NJ_HelpEnableClasses", "Off: no skill, perk or craft is locked and no class experience is earned. The character's class is kept for when it is switched on again."));
		NudgeableSliderInt(strings::TR("NJ_LevelsPerTier", "Class levels per tier"), &classes::levelsPerTier, 1, 50);
		HelpMarker(strings::TR("NJ_HelpLevelsPerTier", "How many class levels between two evolutions. 10: Apprentice at class level 10, Adept at 20, and so on."));
		NudgeableSlider(strings::TR("NJ_XpPerIncrease", "Class experience per skill increase"), &classes::xpPerIncrease, 0.0F, 10.0F, "%.1f", 0.1F);
		NudgeableSlider(strings::TR("NJ_XpBase", "Class level cost"), &classes::xpBase, 0.1F, 50.0F, "%.1f", 0.1F);
		NudgeableSlider(strings::TR("NJ_XpPerLevel", "Cost added per class level"), &classes::xpPerLevel, 0.0F, 10.0F, "%.2f", 0.01F);
		HelpMarker(strings::TR("NJ_HelpCurve", "A class level costs the level cost plus the added cost times the class level reached: the curve's steepness."));
		if (ImGuiMCP::Toggle(strings::TR("NJ_LockPerks", "Lock the perks of other groups"), &classes::lockPerks)) { Classes::RequestApply(); }
		if (ImGuiMCP::Toggle(strings::TR("NJ_LockSkills", "Lock the skills of other groups"), &classes::lockSkills)) { Classes::RequestApply(); }
		ImGuiMCP::Toggle(strings::TR("NJ_CraftingGate", "Crafting follows the group"), &classes::craftingGate);
		HelpMarker(strings::TR("NJ_HelpCraftingGate", "The forge, grindstone and workbench need the warrior group, the enchanting table magic, the alchemy lab covert."));
		ImGuiMCP::Toggle(strings::TR("NJ_RaceMenuClass", "Class category in the race menu"), &classes::raceMenuClass);
		ImGuiMCP::Toggle(strings::TR("NJ_HideLockedTrees", "Hide locked trees in the skills menu"), &classes::hideLockedTrees);
		HelpMarker(strings::TR("NJ_HelpHideLockedTrees", "The skills menu's ring of skill names leaves out the trees your class locks. Their perks cannot be taken either way."));
		const auto lk = Lockouts::GetState();
		ImGuiMCP::TextDisabled(strings::TR("NJ_LockReadout", "Perks conditioned: %u (entry points %u) - crafting refusals this session: %u"), lk.perksConditioned, lk.entriesConditioned, lk.craftingRefusals);
		ImGuiMCP::PopItemWidth();
		RenderButtons();
	}

	void __stdcall Pages::QuestRewards()
	{
		strings::Tick();
		ImGuiMCP::PushItemWidth(200.0F);
		ImGuiMCP::Toggle(strings::TR("NJ_EnableQuestRewards", "Quests pay perk points"), &settings::questrewards::enabled);
		HelpMarker(strings::TR("NJ_HelpQuestRewards", "Each quest below pays its perk points once per character when it completes. Dragon souls pay through Souls to Perks."));
		const auto rows = QuestRewards::Rows();
		if (rows.empty()) { ImGuiMCP::TextWrapped("%s", strings::TR("NJ_NoRows", "No rows: quest-rewards.json is missing or empty.")); }
		for (const auto& r : rows)
		{
			ImGuiMCP::PushID(r.quest.c_str());
			int points = r.points;
			if (!r.inLoadOrder) { ImGuiMCP::BeginDisabled(true); }
			if (NudgeableSliderInt(r.name.c_str(), &points, 0, 20)) { QuestRewards::SetPoints(r.quest, points); }
			if (!r.inLoadOrder) { ImGuiMCP::EndDisabled(); }
			ImGuiMCP::SameLine();
			ImGuiMCP::TextDisabled("%s%s", r.quest.c_str(), r.paid ? strings::TR("NJ_Paid", " - paid") : "");
			ImGuiMCP::PopID();
		}
		ImGuiMCP::PopItemWidth();
		RenderButtons();
	}

	// PLAN.md phase 3 step 4: the Gear tab - set bonuses and unique effects on and off, and their strengths.
	void __stdcall Pages::Gear()
	{
		strings::Tick();
		using namespace settings;
		ImGuiMCP::PushItemWidth(260.0F);
		const auto g = Gear::GetSnapshot();
		if (!g.loaded)
		{
			ImGuiMCP::TextWrapped("%s", strings::TR("NJ_GearNotLoaded", "The gear table could not be read, so set pieces are not counted. See the log."));
			if (!g.error.empty()) { ImGuiMCP::TextWrapped("%s", g.error.c_str()); }
		}
		ImGuiMCP::TextWrapped("%s", strings::TR("NJ_GearIntro", "Unique armour sets grant a bonus at two worn pieces and another at four. Named unique items carry an effect of their own, and no unique item can be enchanted."));

		ImGuiMCP::SeparatorText(strings::TR("NJ_GearSetsTitle", "Set bonuses"));
		if (ImGuiMCP::Toggle(strings::TR("NJ_GearSets", "Set bonuses"), &gear::sets)) { Gear::RequestApply(); }
		HelpMarker(strings::TR("NJ_HelpGearSets", "Off: no set grants anything; the bonuses come back as soon as this is switched on again."));
		if (NudgeableSlider(strings::TR("NJ_GearSetStrength", "Set bonus strength"), &gear::setStrength, 0.25F, 2.0F, "%.2f", 0.01F)) { Gear::RequestApply(); }
		HelpMarker(strings::TR("NJ_HelpGearStrength", "1.00 is as written. 2.00 doubles each bonus (a 20% bonus becomes 40%); 0.50 halves it."));
		for (const auto& st : g.sets)
		{
			ImGuiMCP::Text("%s", st.name.c_str());
			ImGuiMCP::SameLine();
			if (!st.resolved) { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_GearSetMissing", "- not in the plugin")); continue; }
			ImGuiMCP::TextDisabled(strings::TR("NJ_GearSetLine", "- %d of 4 worn - two pieces: %s - four pieces: %s"), st.pieces,
								   st.has2 ? strings::TR("NJ_GearActive", "active") : strings::TR("NJ_GearInactive", "inactive"),
								   st.has4 ? strings::TR("NJ_GearActive", "active") : strings::TR("NJ_GearInactive", "inactive"));
		}

		ImGuiMCP::SeparatorText(strings::TR("NJ_GearUniquesTitle", "Unique effects"));
		if (ImGuiMCP::Toggle(strings::TR("NJ_GearUniques", "Unique effects"), &gear::uniques)) { Gear::RequestApply(); }
		HelpMarker(strings::TR("NJ_HelpGearUniques", "Off: named unique items lose their own effects (their enchantments stay). Uniques still cannot be enchanted."));
		if (NudgeableSlider(strings::TR("NJ_GearUniqueStrength", "Unique effect strength"), &gear::uniqueStrength, 0.25F, 2.0F, "%.2f", 0.01F)) { Gear::RequestApply(); }
		HelpMarker(strings::TR("NJ_HelpGearStrength", "1.00 is as written. 2.00 doubles each bonus (a 20% bonus becomes 40%); 0.50 halves it."));
		int worn = 0;
		for (const auto& u : g.uniques)
		{
			if (!u.worn) { continue; }
			++worn;
			ImGuiMCP::Text(strings::TR("NJ_GearUniqueWorn", "Worn: %s"), u.name.c_str());
		}
		if (worn == 0) { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_GearNoUniqueWorn", "No unique with an effect of its own is worn.")); }
		ImGuiMCP::TextDisabled(strings::TR("NJ_GearNoEnchantLine", "Unique items that cannot be enchanted: %u"), g.uniqueTierItems);
		ImGuiMCP::PopItemWidth();
		RenderButtons();
	}

	// The runesmith's window (D22): his socketing service, drawn with the Soul Stones page; closing it ends the service.
	void __stdcall RunesmithWindow()
	{
		strings::Tick();
		bool open = true;
		if (ImGuiMCP::Begin(strings::TR("NJ_SS_WindowTitle", "The runesmith"), &open, 0))
		{
			g_inRunesmithWindow = true;
			Pages::SoulStones();
			g_inRunesmithWindow = false;
		}
		ImGuiMCP::End();
		if (!open)
		{
			if (g_runesmithWindow) { g_runesmithWindow->IsOpen = false; }
			::SoulStones::SetServiceOpen(false);
		}
	}

	// PLAN.md phase 4 step 4 (Q13): the socketing page - the eligible items, a slot list in word order, the stones you
	// carry with what each does in THIS item, the phrase the slots spell (highlighted when you know it), take out.
	// Every action is a request the main thread runs; the page draws the module's snapshot.
	void __stdcall Pages::SoulStones()
	{
		strings::Tick();
		using namespace settings;
		static std::string selectedKey;
		static int targetSlot = 0;
		static auto lastAsk = std::chrono::steady_clock::now() - std::chrono::seconds(10);
		const auto now = std::chrono::steady_clock::now();
		if (now - lastAsk > std::chrono::milliseconds(750))
		{
			::SoulStones::RequestRefresh();
			lastAsk = now;
		}
		const auto s = ::SoulStones::GetSnapshot();
		auto tierName = [](int a_t) {
			static const char* keys[] = { "NJ_SS_Tier0", "NJ_SS_Tier1", "NJ_SS_Tier2", "NJ_SS_Tier3", "NJ_SS_Tier4" };
			static const char* en[] = { "Petty", "Lesser", "Common", "Greater", "Grand" };
			return (a_t >= 0 && a_t < 5) ? strings::TR(keys[a_t], en[a_t]) : "";
		};
		auto hostName = [](::SoulStones::Host a_h) {
			switch (a_h)
			{
			case ::SoulStones::Host::kMelee: return strings::TR("NJ_SS_HostMelee", "melee weapon");
			case ::SoulStones::Host::kRanged: return strings::TR("NJ_SS_HostRanged", "bow or crossbow");
			case ::SoulStones::Host::kArmour: return strings::TR("NJ_SS_HostArmour", "armour");
			default: return "";
			}
		};
		const ImGuiMCP::ImVec4 known{ 0.45F, 0.85F, 0.45F, 1.0F };
		const ImGuiMCP::ImVec4 unknown{ 0.95F, 0.75F, 0.35F, 1.0F };

		if (!s.loaded)
		{
			ImGuiMCP::TextWrapped("%s", strings::TR("NJ_SS_NotLoaded", "The soul stone table could not be read, so stones do nothing. See the log."));
			if (!s.error.empty()) { ImGuiMCP::TextWrapped("%s", s.error.c_str()); }
			RenderButtons();
			return;
		}
		ImGuiMCP::TextWrapped("%s", strings::TR("NJ_SS_Intro", "A soul stone is a word of the dragon tongue. Magic gear has one slot, epic two, legendary and unique three; staves take none. Each stone acts for the item it sits in, and three stones of one Shout in word order add its phrase once you have unlocked all three words."));
		if (s.serviceOpen) { ImGuiMCP::TextColored(known, "%s", strings::TR("NJ_SS_ServiceOpen", "The runesmith is at work: set and take out stones below.")); }
		else { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_SS_ServiceClosed", "Stones are set and taken out only at the runesmith. Here you can see what your gear carries.")); }
		ImGuiMCP::TextDisabled(strings::TR("NJ_SS_FeeLine", "Fee: one soul gem of the stone's tier for each stone set or taken out. You carry %d / %d / %d / %d / %d (petty to grand)."),
							   s.feeGems[0], s.feeGems[1], s.feeGems[2], s.feeGems[3], s.feeGems[4]);

		ImGuiMCP::SeparatorText(strings::TR("NJ_SS_ItemsTitle", "Your gear with slots"));
		if (s.items.empty())
		{
			ImGuiMCP::TextDisabled("%s", strings::TR("NJ_SS_NoItems", "Nothing in your pack has a stone slot."));
		}
		const ::SoulStones::ItemView* sel = nullptr;
		for (const auto& it : s.items) { if (it.key == selectedKey) { sel = &it; } }
		if (!sel && !s.items.empty()) { sel = &s.items.front(); selectedKey = sel->key; }
		if (ImGuiMCP::BeginChild("njss_items", ImGuiMCP::ImVec2(330.0F, 260.0F), true))
		{
			for (const auto& it : s.items)
			{
				std::string marks;
				for (int k = 0; k < it.slots; ++k) { marks += it.stones[k].word >= 0 ? s.words[it.stones[k].word] : std::string("-"); marks += k + 1 < it.slots ? " " : ""; }
				const std::string label = (it.equipped ? "* " : "  ") + it.name + (it.count > 1 ? " (" + std::to_string(it.count) + ")" : "") + "  [" + marks + "]##" + it.key;
				if (ImGuiMCP::Selectable(label.c_str(), it.key == selectedKey)) { selectedKey = it.key; targetSlot = 0; }
			}
		}
		ImGuiMCP::EndChild();
		ImGuiMCP::SameLine();
		if (ImGuiMCP::BeginChild("njss_detail", ImGuiMCP::ImVec2(0.0F, 260.0F), true) && sel)
		{
			ImGuiMCP::Text("%s", sel->name.c_str());
			ImGuiMCP::TextDisabled(strings::TR("NJ_SS_ItemLine", "A %s with %d slot(s)."), hostName(sel->host), sel->slots);
			if (sel->key.find(":plain") != std::string::npos) { ImGuiMCP::TextWrapped("%s", strings::TR("NJ_SS_PlainHint", "This is a plain stack: wear or wield one of them once, then come back to set a stone.")); }
			if (targetSlot >= sel->slots) { targetSlot = 0; }
			for (int k = 0; k < sel->slots; ++k)
			{
				const auto& st = sel->stones[k];
				std::string label;
				if (st.word >= 0)
				{
					char buf[160];
					std::snprintf(buf, sizeof(buf), strings::TR("NJ_SS_SlotFull", "Slot %d: %s (%s)"), k + 1, s.words[st.word].c_str(), tierName(st.tier));
					label = buf;
				}
				else
				{
					char buf[160];
					std::snprintf(buf, sizeof(buf), strings::TR("NJ_SS_SlotEmpty", "Slot %d: empty"), k + 1);
					label = buf;
				}
				label += "##slot" + std::to_string(k);
				if (ImGuiMCP::Selectable(label.c_str(), targetSlot == k)) { targetSlot = k; }
				if (st.word >= 0)
				{
					ImGuiMCP::TextDisabled("    %s", ::SoulStones::CellText(st.word, sel->host).c_str());
					if (s.serviceOpen)
					{
						ImGuiMCP::SameLine();
						const std::string b = std::string(strings::TR("NJ_SS_TakeOut", "Take out")) + "##out" + std::to_string(k);
						if (ImGuiMCP::SmallButton(b.c_str())) { ::SoulStones::RequestUnsocket(sel->key, k); }
					}
				}
			}
			if (sel->phrase >= 0)
			{
				if (sel->phraseKnown) { ImGuiMCP::TextColored(known, strings::TR("NJ_SS_PhraseActive", "The stones spell %s: the phrase is active."), ::SoulStones::PhraseName(sel->phrase).c_str()); }
				else { ImGuiMCP::TextColored(unknown, "%s", strings::TR("NJ_SS_PhraseUnknown", "The stones spell a phrase you do not yet know.")); }
			}
		}
		ImGuiMCP::EndChild();

		if (sel && s.serviceOpen && sel->stones[targetSlot].word < 0 && sel->key.find(":plain") == std::string::npos)
		{
			char title[160];
			std::snprintf(title, sizeof(title), strings::TR("NJ_SS_StonesTitle", "Your stones - into slot %d"), targetSlot + 1);
			ImGuiMCP::SeparatorText(title);
			if (s.stones.empty()) { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_SS_NoStones", "You carry no soul stones. They are found in boss chests and sold where soul gems are sold.")); }
			for (const auto& v : s.stones)
			{
				// the known-phrase highlight: this stone continues, in this slot, a phrase whose three words you have learned
				const int ph = ::SoulStones::PhraseOfWord(v.word);
				bool continues = false;
				if (ph >= 0 && ph < static_cast<int>(s.phrases.size()) && s.phrases[ph].learned == 3 && s.phrases[ph].words[targetSlot] == v.word)
				{
					continues = true;
					for (int k = 0; k < targetSlot; ++k) { continues &= sel->stones[k].word == s.phrases[ph].words[k]; }
				}
				const std::string b = std::string(strings::TR("NJ_SS_Set", "Set")) + "##set" + std::to_string(v.word) + "_" + std::to_string(v.tier);
				if (ImGuiMCP::SmallButton(b.c_str())) { ::SoulStones::RequestSocket(sel->key, targetSlot, v.word, v.tier); }
				ImGuiMCP::SameLine();
				if (continues) { ImGuiMCP::TextColored(known, "%s (%s) x%d - %s", s.words[v.word].c_str(), tierName(v.tier), v.count, ::SoulStones::CellText(v.word, sel->host).c_str()); }
				else { ImGuiMCP::Text("%s (%s) x%d - %s", s.words[v.word].c_str(), tierName(v.tier), v.count, ::SoulStones::CellText(v.word, sel->host).c_str()); }
			}
		}
		if (!s.lastResult.empty()) { ImGuiMCP::TextWrapped("%s", s.lastResult.c_str()); }

		ImGuiMCP::SeparatorText(strings::TR("NJ_SS_PhrasesTitle", "Phrases you know"));
		int shown = 0;
		for (std::size_t i = 0; i < s.phrases.size(); ++i)
		{
			const auto& p = s.phrases[i];
			if (p.learned < 3) { continue; }   // the blueprint: a phrase is named only once all three words are learned
			++shown;
			const auto name = ::SoulStones::PhraseName(static_cast<int>(i));
			if (p.unlocked) { ImGuiMCP::TextColored(known, strings::TR("NJ_SS_PhraseUnlocked", "%s (%s) - unlocked: works in any item"), name.c_str(), p.shout.c_str()); }
			else { ImGuiMCP::Text(strings::TR("NJ_SS_PhraseLearned", "%s (%s) - learned: unlock all three words with dragon souls to make it work"), name.c_str(), p.shout.c_str()); }
			if (p.granted > 0) { ImGuiMCP::TextDisabled(strings::TR("NJ_SS_Granted", "    your armour's stones let you shout it: %d word(s)"), p.granted); }
		}
		if (shown == 0) { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_SS_NoPhrases", "You have not learned all three words of any Shout yet.")); }

		if (g_inRunesmithWindow) { return; }
		ImGuiMCP::SeparatorText(strings::TR("NJ_SS_SettingsTitle", "Settings"));
		if (ImGuiMCP::Toggle(strings::TR("NJ_SS_Enabled", "Soul stones"), &stones::enabled)) { ::SoulStones::RequestRecompose(); }
		HelpMarker(strings::TR("NJ_SS_HelpEnabled", "Off: socketed stones do nothing and items show only their own enchantment; the stones stay in their slots."));
		if (ImGuiMCP::Toggle(strings::TR("NJ_SS_NameSlots", "Words in item names"), &stones::nameSlots)) { ::SoulStones::RequestRecompose(); }
		HelpMarker(strings::TR("NJ_SS_HelpNameSlots", "A socketed item's name ends with its words, for example Elven Sword [Yol Fo -]."));
		if (ImGuiMCP::Toggle(strings::TR("NJ_SS_NameDragon", "Dragon letters in names"), &stones::nameDragonLetters)) { ::SoulStones::RequestRecompose(); }
		HelpMarker(strings::TR("NJ_SS_HelpNameDragon", "Writes those words in the dragon alphabet. Only for inventories that draw font tags in item names; others show the tags as text."));
		if (ImGuiMCP::Toggle(strings::TR("NJ_SS_Glow", "Weapon glow"), &stones::weaponGlow)) { ::SoulStones::RequestRecompose(); }
		HelpMarker(strings::TR("NJ_SS_HelpGlow", "A drawn weapon glows in the colour of its best stone's tier."));
		RenderButtons();
	}
}
