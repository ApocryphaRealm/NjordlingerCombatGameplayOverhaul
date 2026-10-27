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
#include "Shouting.h"
#include "CarryWeight.h"
#include "Skills.h"
#include "SoulStones.h"

#include "utils/Logger.h"
#include "utils/Strings.h"
#include "utils/Toggle.h"

#include <algorithm>
#include <atomic>
#include <thread>
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
		int g_runesmithTab = 0;                          // D22 revised: 0 Socket, 1 Grind, 2 Craft
		std::atomic<std::uint32_t> g_windowAsk{ 0 };      // each open request
		std::atomic<std::uint32_t> g_windowDrawn{ 0 };    // the request the window last drew for
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

		// A skill's name as the running game calls it (its actor value's own name - a mod that repurposes a skill, such as
		// Hand to Hand for Lockpicking, renames it there), else our translated label.
		const char* SkillName(int a_i)
		{
			static std::array<std::string, skilllist::kCount> s_names;
			static bool s_read = false;
			if (!s_read)
			{
				s_read = true;
				if (auto* list = RE::ActorValueList::GetSingleton())
				{
					for (int i = 0; i < skilllist::kCount; ++i)
					{
						auto* info = list->actorValues[static_cast<int>(RE::ActorValue::kOneHanded) + i];
						const char* n = info ? info->GetFullName() : nullptr;
						if (n && *n) { s_names[static_cast<std::size_t>(i)] = n; }
					}
				}
			}
			return s_names[static_cast<std::size_t>(a_i)].empty() ? strings::TR(kSkillKeys[a_i], kSkillLabels[a_i]) : s_names[static_cast<std::size_t>(a_i)].c_str();
		}

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
				"igRadioButton_IntPtr",  // the runesmith's Craft tab
				"igGetIO",               // the runesmith window fits the screen
				"igSetNextWindowPos",
				"igSetNextWindowSize",
				"igNewLine",
				"igIndent",
				"igUnindent",
				"igSeparator",
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

	void HoldVanityCamera(bool a_hold);

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
		// 6616: on the first line after the window was closed with its X, IsOpen = true did not show it (the second line did).
		// The opener now checks a moment later that the window really drew, and if not sets it closed and open again.
		::SoulStones::SetWindowOpener([](bool a_open) {
			if (!g_runesmithWindow) { return; }
			HoldVanityCamera(a_open);
			g_runesmithWindow->IsOpen = a_open;
			if (!a_open) { return; }
			const auto asked = ++g_windowAsk;
			std::thread([asked]() {
				for (int attempt = 0; attempt < 3; ++attempt)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(400));
					if (g_windowAsk.load() != asked || g_windowDrawn.load() >= asked) { return; }
					if (auto* tasks = SKSE::GetTaskInterface())
					{
						tasks->AddTask([asked]() {
							if (!g_runesmithWindow || g_windowAsk.load() != asked || g_windowDrawn.load() >= asked) { return; }
							g_runesmithWindow->IsOpen = false;
							g_runesmithWindow->IsOpen = true;
							logger::info("runesmith window: it did not draw after the open - opened again");
						});
					}
				}
			}).detach();
		});
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

		// D26 (DESIGN-RETHINK.md 14): Shouting On / Off, Off by default; auto-convert dragon souls, off by default - per save
		{
			ImGuiMCP::SeparatorText(strings::TR("NJ_ShoutingTitle", "The Voice"));
			const auto sh = Shouting::GetState();
			bool on = sh.enabled;
			if (ImGuiMCP::Toggle(strings::TR("NJ_Shouting", "Shouting"), &on)) { Shouting::RequestSetEnabled(on); }
			HelpMarker(strings::TR("NJ_HelpShoutingDefaultOn", "On (the default): you shout as in the game. Off: your shout key does nothing - the Shouts your soul stones grant and the ones a quest needs still work - and Stormcrown's Dragonborn tree and meditations stay out of reach. This choice is kept per save."));
			bool conv = sh.autoConvert;
			if (ImGuiMCP::Toggle(strings::TR("NJ_AutoConvertSouls", "Auto-convert dragon souls"), &conv)) { Shouting::RequestSetAutoConvert(conv); }
			HelpMarker(strings::TR("NJ_HelpAutoConvertSouls", "On: every dragon soul you absorb becomes perk points at once, at Souls to Perks' rate (the soul of Dragon Rising still unlocks your first word). Off (the default): souls stay souls - spend them with Souls to Perks. Kept per save."));
			if (sh.stpConverts) { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_StpConverts", "Souls to Perks converts souls by itself (bAutoConvert in its INI), so this switch waits.")); }
			ImGuiMCP::TextDisabled(strings::TR("NJ_ShoutingLine", "Dragon souls turned into perk points: %u (%d soul(s) per point)%s"), sh.soulsConverted, sh.soulsPerPoint,
				sh.reserved ? strings::TR("NJ_ShoutingReserved", " - waiting until The Way of the Voice is done") : "");
		}
		// The owner 19:53/19:54: carry weight - one owned modifier (CarryWeight.h)
		{
			ImGuiMCP::SeparatorText(strings::TR("NJ_CarryTitle", "Carry weight"));
			ImGuiMCP::Toggle(strings::TR("NJ_CarryControl", "Control carry weight"), &carry::enabled);
			HelpMarker(strings::TR("NJ_HelpCarryControl", "On: your carry weight starts at the value below, and every permanent Stamina point Adaptive Attributes grants adds the amount below. Potions, the Steed Stone and other effects still add on top. Off: the game's own values."));
			NudgeableSlider(strings::TR("NJ_CarryBase", "Starting carry weight"), &carry::base, 50.0F, 1000.0F, "%.0f", 10.0F);
			NudgeableSlider(strings::TR("NJ_CarryPerStamina", "Carry weight per stamina point"), &carry::perStamina, 0.0F, 10.0F, "%.1f", 0.5F);
			const auto cw = CarryWeight::GetState();
			if (cw.aaWritesCarry) { ImGuiMCP::TextWrapped("%s", strings::TR("NJ_CarryAaWrites", "Adaptive Attributes adds the per-stamina carry weight itself (bEnableCarryWeightFromStaminaBonus in its INI), so this slider waits: set that option to false to let this page control it.")); }
			else if (!cw.aaFound) { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_CarryNoAa", "Adaptive Attributes did not answer: only the starting value applies.")); }
			ImGuiMCP::TextDisabled(strings::TR("NJ_CarryLine", "Stamina points from Adaptive Attributes: %d - this page's change: %+.1f - carry weight now: %.0f"), cw.aaStamina, cw.applied, cw.carryWeight);
		}
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
			const char* name = SkillName(i);
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
				ImGuiMCP::TextDisabled(strings::TR("NJ_LockedSkill", "%s - locked at %.0f"), SkillName(i), s.lockedCap[i]);
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

	// The owner 23:18: no idle vanity camera while the runesmith window is up. The game's own INI value
	// fAutoVanityModeDelay:Camera is held at a very large delay and given back exactly when the window closes (the player's
	// value is never written to any file).
	// 11b9: moving the cursor over the window also turned the camera, so Thorvar left the frame - the game's own "looking"
	// control is switched off while the window is up and back on when it closes (only if it was on before).
	void HoldLooking(bool a_hold)
	{
		static bool s_heldLook = false;
		auto* cm = RE::ControlMap::GetSingleton();
		if (!cm) { return; }
		if (a_hold && !s_heldLook && cm->IsLookingControlsEnabled())
		{
#if RUNTIME_LINE == 17
			cm->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kLooking, false, false);
#else
			cm->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kLooking, false);
#endif
			s_heldLook = true;
		}
		else if (!a_hold && s_heldLook)
		{
#if RUNTIME_LINE == 17
			cm->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kLooking, true, false);
#else
			cm->ToggleControls(RE::UserEvents::USER_EVENT_FLAG::kLooking, true);
#endif
			s_heldLook = false;
		}
	}

	void HoldVanityCamera(bool a_hold)
	{
		HoldLooking(a_hold);
		static bool s_held = false;
		static bool s_allowWas = true;
		static float s_saved = 0.0F;
		auto* ini = RE::INISettingCollection::GetSingleton();
		auto* setting = ini ? ini->GetSetting("fAutoVanityModeDelay:Camera") : nullptr;
		if (!setting) { return; }
		if (a_hold && !s_held)
		{
			s_saved = setting->GetFloat();
			setting->data.f = 1.0e9F;
			if (auto* cam = RE::PlayerCamera::GetSingleton())
			{
				s_allowWas = cam->GetRuntimeData2().allowAutoVanityMode;
				cam->GetRuntimeData2().allowAutoVanityMode = false;
			}
			s_held = true;
			logger::info("runesmith window: the idle vanity camera is held off (was {:.0f} s)", s_saved);
		}
		else if (!a_hold && s_held)
		{
			setting->data.f = s_saved;
			// 1c64: the orbit started at once after the window closed - the idle countdown is wound back to the full delay
			if (auto* cam = RE::PlayerCamera::GetSingleton())
			{
				cam->GetRuntimeData2().idleTimer = 0.0F;   // 74d2: it counts UP - the orbit began at once with s_saved
				cam->GetRuntimeData2().allowAutoVanityMode = s_allowWas;
			}
			s_held = false;
			logger::info("runesmith window: the idle vanity camera delay is back to {:.0f} s", s_saved);
		}
	}

	// The owner 22:36: Font Awesome icons through AMF's own fonts (icons/fa-solid-900.ttf ships with AMF). An icon is drawn
	// in the icon font, the label beside it in the page's font, so every language keeps its text.
	namespace fa
	{
		inline constexpr unsigned kGem = 0xf3a5, kGrind = 0xf5a7, kHammer = 0xf6e3, kAdd = 0xf067, kSet = 0xf055, kTakeOut = 0xf056, kEmpty = 0xf111, kScroll = 0xf70e, kBook = 0xf02d;
	}

	void Icon(unsigned a_cp, const ImGuiMCP::ImVec4* a_colour = nullptr)
	{
		const std::string glyph = FontAwesome::UnicodeToUtf8(a_cp);
		FontAwesome::PushSolid();
		if (a_colour) { ImGuiMCP::TextColored(*a_colour, "%s", glyph.c_str()); }
		else { ImGuiMCP::Text("%s", glyph.c_str()); }
		FontAwesome::Pop();
		ImGuiMCP::SameLine();
	}

	// An icon button and its label button; either click acts.
	bool IconButton(unsigned a_cp, const char* a_label, const std::string& a_id, bool a_small = true)
	{
		const std::string glyph = FontAwesome::UnicodeToUtf8(a_cp) + "##i" + a_id;
		FontAwesome::PushSolid();
		bool hit = a_small ? ImGuiMCP::SmallButton(glyph.c_str()) : ImGuiMCP::Button(glyph.c_str());
		FontAwesome::Pop();
		ImGuiMCP::SameLine();
		const std::string label = std::string(a_label) + "##l" + a_id;
		hit |= a_small ? ImGuiMCP::SmallButton(label.c_str()) : ImGuiMCP::Button(label.c_str());
		return hit;
	}

	ImGuiMCP::ImVec4 TierColour(int a_tier)
	{
		static const ImGuiMCP::ImVec4 c[5] = { { 0.81F, 0.89F, 0.96F, 1.0F }, { 0.31F, 0.55F, 1.0F, 1.0F }, { 0.42F, 0.36F, 1.0F, 1.0F }, { 0.61F, 0.30F, 1.0F, 1.0F }, { 0.42F, 0.12F, 0.72F, 1.0F } };
		return c[std::clamp(a_tier, 0, 4)];
	}

	// The owner 22:55: the words you know - every Shout whose three words are learned at walls, its words in slot order,
	// the phrase line per host and each word's own line from petty to grand; partly learned Shouts greyed with "?" for the
	// words still missing. The same data as the phrase check (Stormcrown's soft-linked phrases when its plugin is loaded).
	void KnownWords(const ::SoulStones::Snapshot& a_s)
	{
		const ImGuiMCP::ImVec4 known{ 0.45F, 0.85F, 0.45F, 1.0F };
		ImGuiMCP::TextDisabled(strings::TR("NJ_SS_BlanksLine", "Rune Blanks: %d - orichalcum ingots: %d"), a_s.blanks, a_s.ingots);
		ImGuiMCP::TextWrapped("%s", strings::TR("NJ_SS_WordsIntro", "The Shouts whose words you have learned at word walls. Set their three stones in slot order on one item to add the phrase."));
		const ::SoulStones::Host hosts[3] = { ::SoulStones::Host::kMelee, ::SoulStones::Host::kRanged, ::SoulStones::Host::kArmour };
		const char* hostKeys[3] = { "NJ_SS_HostMelee", "NJ_SS_HostRanged", "NJ_SS_HostArmour" };
		const char* hostEn[3] = { "melee weapon", "bow or crossbow", "armour" };
		int shown = 0;
		for (int pass = 0; pass < 2; ++pass)   // complete Shouts first, then the partly learned ones
		{
			for (std::size_t i = 0; i < a_s.phrases.size(); ++i)
			{
				const auto& p = a_s.phrases[i];
				if (!p.available || p.learned == 0 || (pass == 0) != (p.learned == 3)) { continue; }
				++shown;
				std::string order;
				for (int k = 0; k < 3; ++k)
				{
					if (k) { order += " \xC2\xB7 "; }
					order += p.wordLearned[k] ? a_s.words[p.words[k]] : std::string("?");
				}
				const std::string head = p.shout + ": " + order + "##kw" + std::to_string(i);
				Icon(p.learned == 3 ? fa::kScroll : fa::kBook, p.learned == 3 ? &known : nullptr);
				if (p.learned == 3) { ImGuiMCP::TextColored(known, "%s", (p.shout + ": " + order).c_str()); }
				else { ImGuiMCP::TextDisabled("%s", (p.shout + ": " + order).c_str()); }
				ImGuiMCP::Indent();
				if (p.learned == 3)
				{
					for (int h = 0; h < 3; ++h)
					{
						ImGuiMCP::TextWrapped("%s: %s", strings::TR(hostKeys[h], hostEn[h]), ::SoulStones::PhraseText(static_cast<int>(i), hosts[h]).c_str());
					}
				}
				for (int k = 0; k < 3; ++k)
				{
					if (!p.wordLearned[k]) { continue; }
					const int w = p.words[k];
					ImGuiMCP::TextDisabled("%s", a_s.words[w].c_str());
					ImGuiMCP::Indent();
					for (int h = 0; h < 3; ++h)
					{
						ImGuiMCP::TextWrapped("%s: %s  ->  %s", strings::TR(hostKeys[h], hostEn[h]), ::SoulStones::CellText(w, hosts[h], 0).c_str(), ::SoulStones::CellText(w, hosts[h], 4).c_str());
					}
					ImGuiMCP::Unindent();
				}
				ImGuiMCP::Unindent();
				(void)head;
			}
		}
		if (shown == 0) { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_SS_NoWords", "You have learned no word of power yet.")); }
	}

	// D22 revised (STONE-CRAFTING.md): the Grind tab - soul gems in, dust out (a filled gem gives two, black two grand).
	void GrindTab(const ::SoulStones::Snapshot& a_s, const char* (*a_tier)(int))
	{
		ImGuiMCP::TextWrapped("%s", strings::TR("NJ_SS_GrindIntro", "Thorvar grinds soul gems into soul gem dust of their size, free of charge. A filled gem gives two dust, an empty one one; a black soul gem gives two grand dust (four when filled)."));
		if (a_s.gems.empty()) { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_SS_NoGems", "You carry no soul gems he will grind.")); }
		for (const auto& g : a_s.gems)
		{
			ImGuiMCP::Text(strings::TR("NJ_SS_GemLine", "%s - %d filled, %d empty: %d %s dust"), g.name.c_str(), g.filled, g.empty, g.dust, a_tier(g.tier));
			ImGuiMCP::SameLine();
			if (IconButton(fa::kGrind, strings::TR("NJ_SS_GrindOne", "Grind one"), "g1_" + std::to_string(g.form))) { ::SoulStones::RequestGrind(g.form, false); }
			ImGuiMCP::SameLine();
			if (IconButton(fa::kGrind, strings::TR("NJ_SS_GrindAll", "Grind all"), "ga_" + std::to_string(g.form))) { ::SoulStones::RequestGrind(g.form, true); }
		}
	}

	// The Craft tab - a learned word, a tier, the recipe line, Make.
	void CraftTab(const ::SoulStones::Snapshot& a_s, const char* (*a_tier)(int))
	{
		static int tier = 0;
		ImGuiMCP::TextWrapped(strings::TR("NJ_SS_CraftIntroBlank", "Thorvar cuts an orichalcum ingot into %d Rune Blanks, then carves a blank and engraves it with soul gem dust of the stone's size - a stone of any word you have learned at a word wall. With no blank in your pack he cuts an ingot first."), a_s.blanksPerIngot);
		for (int t = 0; t < 5; ++t)
		{
			if (t > 0) { ImGuiMCP::SameLine(); }
			ImGuiMCP::RadioButton(a_tier(t), &tier, t);
		}
		ImGuiMCP::TextDisabled(strings::TR("NJ_SS_RecipeLineBlank", "Recipe: one Rune Blank and %d %s dust. You carry %d blank(s), %d ingot(s) and %d %s dust."),
							   a_s.craftDust[tier], a_tier(tier), a_s.blanks, a_s.ingots, a_s.feeGems[tier], a_tier(tier));
		if (a_s.learnedWords.empty()) { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_SS_NoLearned", "You have learned no word of power yet: learn one at a word wall.")); }
		const bool can = (a_s.blanks >= 1 || a_s.ingots >= 1) && a_s.feeGems[tier] >= a_s.craftDust[tier];
		for (const int w : a_s.learnedWords)
		{
			if (IconButton(fa::kHammer, strings::TR("NJ_SS_Make", "Make"), "mk" + std::to_string(w))) { ::SoulStones::RequestCraft(w, tier); }
			ImGuiMCP::SameLine();
			{ const auto col = TierColour(tier); Icon(fa::kGem, &col); }
			if (can) { ImGuiMCP::Text("%s (%s)", a_s.words[w].c_str(), a_tier(tier)); }
			else { ImGuiMCP::TextDisabled("%s (%s)", a_s.words[w].c_str(), a_tier(tier)); }
		}
		ImGuiMCP::TextDisabled(strings::TR("NJ_SS_Tallies", "Dust ground: %u - stones made: %u"), a_s.dustMade, a_s.stonesMade);
	}

	// ENCHANTING-OFF.md 7: the Add slot tab - items below three slots, the cost of the next slot, Add.
	void AddSlotTab(const ::SoulStones::Snapshot& a_s)
	{
		static const char* gemKeys[] = { "NJ_SS_GemLesser", "NJ_SS_GemGreater", "NJ_SS_GemGrand" };
		static const char* gemEn[] = { "lesser", "greater", "grand" };
		ImGuiMCP::TextWrapped("%s", strings::TR("NJ_SS_SlotIntro", "Thorvar cuts new stone slots into gear, up to three on any piece. Each new slot costs one orichalcum ingot and one soul gem, filled or empty: lesser for a piece's first added slot, greater for the second, grand for the third. The piece keeps its tier and colour."));
		ImGuiMCP::TextDisabled(strings::TR("NJ_SS_SlotCarry", "You carry %d orichalcum ingot(s) and %d / %d / %d lesser / greater / grand soul gems."), a_s.addIngots, a_s.addGems[0], a_s.addGems[1], a_s.addGems[2]);
		if (a_s.slotOffers.empty()) { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_SS_NoSlotOffers", "Nothing in your pack can take another slot.")); }
		for (const auto& o : a_s.slotOffers)
		{
			if (IconButton(fa::kAdd, strings::TR("NJ_SS_AddSlot", "Add"), "as_" + o.key)) { ::SoulStones::RequestAddSlot(o.key); }
			ImGuiMCP::SameLine();
			const bool can = a_s.addIngots >= 1 && a_s.addGems[o.gemTier] >= 1;
			char line[256];
			std::snprintf(line, sizeof(line), strings::TR("NJ_SS_SlotLine", "%s - %d of 3 slots; next: one ingot and one %s soul gem"), o.name.c_str(), o.slots, strings::TR(gemKeys[o.gemTier], gemEn[o.gemTier]));
			if (can) { ImGuiMCP::Text("%s", line); }
			else { ImGuiMCP::TextDisabled("%s", line); }
		}
	}

	// The runesmith's window (D22): his services - Socket, Grind, Craft, Add slot - drawn with the Soul Stones page;
	// closing it ends the service. His line picks the tab it opens on.
	void __stdcall RunesmithWindow()
	{
		strings::Tick();
		bool open = true;
		g_windowDrawn = g_windowAsk.load();
		if (const int asked = ::SoulStones::TakeRequestedTab(); asked >= 0) { g_runesmithTab = asked; }
		// The owner 22:36: the whole frame fits on any screen and any AMF text size - a fixed share of the screen on the LEFT
		// (Thorvar stands in the right third, SoulStones turns the player for that), the contents scroll inside it
		if (auto* io = ImGuiMCP::GetIO(); io && io->DisplaySize.x > 0.0F && io->DisplaySize.y > 0.0F)
		{
			const float w = io->DisplaySize.x, h = io->DisplaySize.y;
			ImGuiMCP::SetNextWindowPos(ImGuiMCP::ImVec2(w * 0.03F, h * 0.06F), ImGuiMCP::ImGuiCond_Always);
			// 74d2: in third person the player stands left of centre behind the window - the window is narrower there
			auto* cam = RE::PlayerCamera::GetSingleton();
			const bool third = cam && cam->currentState && cam->currentState->id == RE::CameraState::kThirdPerson;
			ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2(std::clamp(w * (third ? 0.36F : 0.46F), 420.0F, w * 0.6F), h * 0.86F), ImGuiMCP::ImGuiCond_Always);
		}
		if (ImGuiMCP::Begin(strings::TR("NJ_SS_WindowTitle", "The runesmith"), &open, ImGuiMCP::ImGuiWindowFlags_NoCollapse))
		{
			const char* tabs[] = { strings::TR("NJ_SS_TabSocket", "Socket"), strings::TR("NJ_SS_TabGrind", "Grind"), strings::TR("NJ_SS_TabCraft", "Craft"), strings::TR("NJ_SS_TabAddSlot", "Add slot"), strings::TR("NJ_SS_TabWords", "Words") };
			const unsigned tabIcons[] = { fa::kGem, fa::kGrind, fa::kHammer, fa::kAdd, fa::kBook };
			for (int t = 0; t < 5; ++t)
			{
				if (t > 0) { ImGuiMCP::SameLine(); }
				const std::string label = g_runesmithTab == t ? std::string("[ ") + tabs[t] + " ]" : std::string(tabs[t]);
				if (IconButton(tabIcons[t], label.c_str(), "tab" + std::to_string(t), false)) { g_runesmithTab = t; }
			}
			ImGuiMCP::Separator();
			auto tierName = [](int a_t) -> const char* {
				static const char* keys[] = { "NJ_SS_Tier0", "NJ_SS_Tier1", "NJ_SS_Tier2", "NJ_SS_Tier3", "NJ_SS_Tier4" };
				static const char* en[] = { "Petty", "Lesser", "Common", "Greater", "Grand" };
				return (a_t >= 0 && a_t < 5) ? strings::TR(keys[a_t], en[a_t]) : "";
			};
			if (g_runesmithTab == 0)
			{
				g_inRunesmithWindow = true;
				Pages::SoulStones();
				g_inRunesmithWindow = false;
			}
			else
			{
				static auto lastAsk = std::chrono::steady_clock::now() - std::chrono::seconds(10);
				const auto now = std::chrono::steady_clock::now();
				if (now - lastAsk > std::chrono::milliseconds(750)) { ::SoulStones::RequestRefresh(); lastAsk = now; }
				const auto snap = ::SoulStones::GetSnapshot();
				if (g_runesmithTab == 1) { GrindTab(snap, tierName); }
				else if (g_runesmithTab == 2) { CraftTab(snap, tierName); }
				else if (g_runesmithTab == 3) { AddSlotTab(snap); }
				else { KnownWords(snap); }
				if (!snap.lastResult.empty()) { ImGuiMCP::TextWrapped("%s", snap.lastResult.c_str()); }
			}
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
		ImGuiMCP::TextWrapped("%s", strings::TR("NJ_SS_IntroTier", "A soul stone is a word of the dragon tongue. An item's slots come from its base item's tier - common none, magic one, epic two, legendary and unique three; an enchantment does not change it, and staves take none. The runesmith can add slots up to three. Each stone acts for the item it sits in, and three stones of one Shout in word order add its phrase once you have learned all three words."));
		if (s.serviceOpen) { ImGuiMCP::TextColored(known, "%s", strings::TR("NJ_SS_ServiceOpen", "The runesmith is at work: set and take out stones below.")); }
		else { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_SS_ServiceClosedCraft", "Stones are made, set and taken out only by the runesmith. Here you can see what your gear carries.")); }
		{
			// 11b9: the line ran past the window's right edge - wrapped
			char fee[512];
			std::snprintf(fee, sizeof(fee), strings::TR("NJ_SS_FeeLineDust", "Fee: one soul gem dust of the stone's tier for each stone set; taking one out is free. You carry %d / %d / %d / %d / %d dust (petty to grand)."),
						  s.feeGems[0], s.feeGems[1], s.feeGems[2], s.feeGems[3], s.feeGems[4]);
			ImGuiMCP::TextWrapped("%s", fee);
		}

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
			ImGuiMCP::TextDisabled(strings::TR("NJ_SS_ItemLineKind", "Kind: %s - slots: %d"), hostName(sel->host), sel->slots);
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
				if (st.word >= 0) { const auto col = TierColour(st.tier); Icon(fa::kGem, &col); }
				else { Icon(fa::kEmpty); }
				if (ImGuiMCP::Selectable(label.c_str(), targetSlot == k)) { targetSlot = k; }
				if (st.word >= 0)
				{
					ImGuiMCP::TextDisabled("    %s", ::SoulStones::CellText(st.word, sel->host, st.tier).c_str());
					if (s.canSocket)
					{
						ImGuiMCP::Text("    ");   // 1c64: on its own line - beside the slot text it was cut off at the pane edge
						ImGuiMCP::SameLine();
						if (IconButton(fa::kTakeOut, strings::TR("NJ_SS_TakeOut", "Take out"), "out" + std::to_string(k))) { ::SoulStones::RequestUnsocket(sel->key, k); }
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

		// The owner 22:39: an item card in a fixed place - tier colour, stat, its own enchantment, the stone lines, the phrase,
		// and the slots as icons (filled in the stone's tier colour, empty, added ones marked +)
		if (sel)
		{
			static const ImGuiMCP::ImVec4 tierCol[5] = { { 0.85F, 0.85F, 0.85F, 1.0F }, { 0.35F, 0.6F, 1.0F, 1.0F }, { 0.7F, 0.4F, 1.0F, 1.0F }, { 1.0F, 0.65F, 0.2F, 1.0F }, { 0.95F, 0.3F, 0.3F, 1.0F } };
			const char* tierNames[5] = { "Common", "Magic", "Epic", "Legendary", "Unique" };
			int ti = 0;
			for (int t = 0; t < 5; ++t) { if (sel->itemTier == tierNames[t]) { ti = t; } }
			ImGuiMCP::SeparatorText(strings::TR("NJ_SS_CardTitle", "Item card"));
			if (ImGuiMCP::BeginChild("njss_card", ImGuiMCP::ImVec2(0.0F, 170.0F), true))
			{
				ImGuiMCP::TextColored(tierCol[ti], "%s", sel->name.c_str());
				if (sel->stat > 0.0F)
				{
					ImGuiMCP::TextDisabled(sel->host == ::SoulStones::Host::kArmour ? strings::TR("NJ_SS_CardArmour", "Armour %.0f") : strings::TR("NJ_SS_CardDamage", "Damage %.0f"), sel->stat);
				}
				for (int k = 0; k < sel->slots; ++k)
				{
					const auto& st = sel->stones[k];
					if (st.word >= 0) { const auto col = TierColour(st.tier); Icon(fa::kGem, &col); }
					else { Icon(fa::kEmpty); }
					if (k >= sel->naturalSlots) { ImGuiMCP::TextDisabled("+"); ImGuiMCP::SameLine(); }
				}
				if (sel->slots == 0) { ImGuiMCP::TextDisabled("%s", strings::TR("NJ_SS_CardNoSlots", "no stone slots")); }
				else { ImGuiMCP::NewLine(); }
				for (const auto& l : sel->enchantLines) { ImGuiMCP::Text("%s", l.c_str()); }
				for (int k = 0; k < sel->slots; ++k)
				{
					const auto& st = sel->stones[k];
					if (st.word < 0) { continue; }
					const auto col = TierColour(st.tier);
					Icon(fa::kGem, &col);
					ImGuiMCP::TextWrapped("%s", ::SoulStones::CellText(st.word, sel->host, st.tier).c_str());
				}
				if (sel->phrase >= 0)
				{
					Icon(fa::kScroll, sel->phraseKnown ? &known : &unknown);
					ImGuiMCP::TextColored(sel->phraseKnown ? known : unknown, "%s", sel->phraseKnown ? ::SoulStones::PhraseName(sel->phrase).c_str() : strings::TR("NJ_SS_PhraseUnknown", "The stones spell a phrase you do not yet know."));
				}
			}
			ImGuiMCP::EndChild();
		}

		if (sel && s.canSocket && sel->stones[targetSlot].word < 0 && sel->key.find(":plain") == std::string::npos)
		{
			char title[160];
			std::snprintf(title, sizeof(title), strings::TR("NJ_SS_StonesTitle", "Your stones - into slot %d"), targetSlot + 1);
			ImGuiMCP::SeparatorText(title);
			if (s.stones.empty())
			{
				ImGuiMCP::TextDisabled("%s", strings::TR("NJ_SS_NoStonesCraft", "You carry no soul stones. The runesmith cuts them from orichalcum and soul gem dust, for words you have learned."));
			}
			for (const auto& v : s.stones)
			{
				// the known-phrase highlight: this stone continues, in this slot, a phrase whose three words you have learned
				bool continues = false;
				for (const auto& pv : s.phrases)
				{
					if (pv.learned < 3 || pv.words[targetSlot] != v.word) { continue; }
					bool fits = true;
					for (int k = 0; k < targetSlot; ++k) { fits &= sel->stones[k].word == pv.words[k]; }
					continues |= fits;
				}
				if (IconButton(fa::kSet, strings::TR("NJ_SS_Set", "Set"), "set" + std::to_string(v.word) + "_" + std::to_string(v.tier))) { ::SoulStones::RequestSocket(sel->key, targetSlot, v.word, v.tier); }
				ImGuiMCP::SameLine();
				{ const auto col = TierColour(v.tier); Icon(fa::kGem, &col); }
				if (continues) { ImGuiMCP::TextColored(known, "%s (%s) x%d - %s", s.words[v.word].c_str(), tierName(v.tier), v.count, ::SoulStones::CellText(v.word, sel->host, v.tier).c_str()); }
				else { ImGuiMCP::Text("%s (%s) x%d - %s", s.words[v.word].c_str(), tierName(v.tier), v.count, ::SoulStones::CellText(v.word, sel->host, v.tier).c_str()); }
			}
		}
		if (!s.lastResult.empty()) { ImGuiMCP::TextWrapped("%s", s.lastResult.c_str()); }

		ImGuiMCP::SeparatorText(strings::TR("NJ_SS_WordsTitle", "Words you know"));
		KnownWords(s);
		for (const auto& p : s.phrases) { if (p.granted > 0) { ImGuiMCP::TextDisabled(strings::TR("NJ_SS_Granted", "    your armour's stones let you shout it: %d word(s)"), p.granted); } }

		if (g_inRunesmithWindow) { return; }
		ImGuiMCP::SeparatorText(strings::TR("NJ_SS_SettingsTitle", "Settings"));
		if (ImGuiMCP::Toggle(strings::TR("NJ_SS_Enabled", "Soul stones"), &stones::enabled)) { ::SoulStones::RequestRecompose(); }
		HelpMarker(strings::TR("NJ_SS_HelpEnabled", "Off: socketed stones do nothing and items show only their own enchantment; the stones stay in their slots."));
		if (ImGuiMCP::Toggle(strings::TR("NJ_SS_NameSlots", "Words in item names"), &stones::nameSlots)) { ::SoulStones::RequestRecompose(); }
		HelpMarker(strings::TR("NJ_SS_HelpNameSlots", "A socketed item's name ends with its words, for example Elven Sword [Yol Fo -]."));
		if (ImGuiMCP::Toggle(strings::TR("NJ_SS_Glow", "Weapon glow"), &stones::weaponGlow)) { ::SoulStones::RequestRecompose(); }
		HelpMarker(strings::TR("NJ_SS_HelpGlow", "A drawn weapon glows in the colour of its best stone's tier."));
		RenderButtons();
	}
}
