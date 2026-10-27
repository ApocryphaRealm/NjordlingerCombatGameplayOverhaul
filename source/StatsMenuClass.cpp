#include "PCH.h"

#include "StatsMenuClass.h"

#include "ClassData.h"
#include "Classes.h"
#include "Settings.h"

#include "utils/Logger.h"
#include "utils/Strings.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <chrono>
#include <mutex>
#include <thread>

#undef MessageBox

namespace RE
{
	// The game's own message-box opener (title, callback, options) - Souls to Perks' Dragonstone (recorded working,
	// 1.0.7) and Local Map Upgrade call it the same way. Resolved inside the function, never at static init.
	std::uint32_t NJ_UI__OpenMessageBox(const BSString& a_title, const BSTSmartPointer<IMessageBoxCallback>& a_callback,
										std::uint8_t a_arg3, std::uint32_t a_arg4, std::int32_t a_arg5, const BSTArray<BSString>& a_options)
	{
		using func_t = decltype(&NJ_UI__OpenMessageBox);
		static REL::Relocation<func_t> func{ REL::RelocationID(51421, REL::Module::get().version() < SKSE::RUNTIME_SSE_1_6_1130 ? 52270 : 442726) };
		return func(a_title, a_callback, a_arg3, a_arg4, a_arg5, a_options);
	}
}

namespace StatsMenuClass
{
	namespace
	{
		constexpr const char* kFieldName = "njClassLine";
		std::atomic<bool> g_open{ false };
		std::atomic<std::uint32_t> g_session{ 0 };

		// The owner 01:34 (CUSTOM-TREES.md): the Custom Skills Framework ring - SKILLS.json's order, read from
		// skills-ring.json (shipped beside it in the Test-only mod). Each slot: a vanilla skill (our skill index) or a
		// custom tree with the class group that holds it (-1: every class). Empty when the ring is not installed.
		struct RingSlot { int skill = -1; int group = -1; std::string id; };
		std::vector<RingSlot> g_ring;
		RE::TESRace* g_werebearRace = nullptr;
		RE::TESGlobal* g_werebearMenu = nullptr;   // Berserkyr's "show the werebear tree" global

		void LoadRing()
		{
			static const std::pair<const char*, RE::ActorValue> kVanilla[] = {
				{ "OneHanded", RE::ActorValue::kOneHanded }, { "TwoHanded", RE::ActorValue::kTwoHanded }, { "Marksman", RE::ActorValue::kArchery },
				{ "Block", RE::ActorValue::kBlock }, { "Smithing", RE::ActorValue::kSmithing }, { "HeavyArmor", RE::ActorValue::kHeavyArmor },
				{ "LightArmor", RE::ActorValue::kLightArmor }, { "Pickpocket", RE::ActorValue::kPickpocket }, { "Lockpicking", RE::ActorValue::kLockpicking },
				{ "Sneak", RE::ActorValue::kSneak }, { "Alchemy", RE::ActorValue::kAlchemy }, { "Speechcraft", RE::ActorValue::kSpeech },
				{ "Alteration", RE::ActorValue::kAlteration }, { "Conjuration", RE::ActorValue::kConjuration }, { "Destruction", RE::ActorValue::kDestruction },
				{ "Illusion", RE::ActorValue::kIllusion }, { "Restoration", RE::ActorValue::kRestoration }, { "Enchanting", RE::ActorValue::kEnchanting } };
			std::ifstream in(std::filesystem::path("Data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/skills-ring.json"));
			if (!in) { logger::info("stats menu: no skills ring (skills-ring.json not installed) - the 18 vanilla trees"); return; }
			std::map<std::string, int> customGroup;
			try
			{
				std::ifstream cin(std::filesystem::path("Data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/classes.json"));
				const auto cj = cin ? nlohmann::json::parse(cin) : nlohmann::json::object();
				for (const auto& t : cj.value("customTrees", nlohmann::json::array()))
				{
					const auto gid = t.value("group", std::string());
					for (int g = 0; g < ClassData::kGroupCount; ++g) { if (gid == ClassData::kGroupId[g]) { customGroup[t.value("skill", std::string())] = g; } }
				}
				const auto j = nlohmann::json::parse(in);
				for (const auto& e : j.value("ring", nlohmann::json::array()))
				{
					RingSlot slot;
					slot.id = e.get<std::string>();
					for (const auto& [name, av] : kVanilla) { if (slot.id == name) { slot.skill = static_cast<int>(av) - static_cast<int>(RE::ActorValue::kOneHanded); } }
					if (slot.skill < 0) { if (auto it = customGroup.find(slot.id); it != customGroup.end()) { slot.group = it->second; } }
					g_ring.push_back(slot);
				}
				logger::info("stats menu: the skills ring holds {} trees (skills-ring.json)", g_ring.size());
			}
			catch (const std::exception& ex) { logger::warn("stats menu: skills-ring.json not readable ({}) - the 18 vanilla trees", ex.what()); g_ring.clear(); }
		}

		// Werebear (CUSTOM-TREES.md 3, way b): the skills key in werebear form opens Berserkyr's werebear tree (its own
		// global, as its hotkey script does) instead of the werewolf tree.
		void ResolveWerebear()
		{
			// the race from the plugin build (stones.json: resolved from the masters by editor ID)
			try
			{
				std::ifstream in(std::filesystem::path("Data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/stones.json"));
				const auto j = in ? nlohmann::json::parse(in) : nlohmann::json::object();
				const auto ref = j.value("werebearRace", std::string());
				const auto bar = ref.find('|');
				if (bar != std::string::npos)
				{
					if (auto* dh = RE::TESDataHandler::GetSingleton()) { g_werebearRace = dh->LookupForm<RE::TESRace>(static_cast<RE::FormID>(std::stoul(ref.substr(bar + 1), nullptr, 0)), ref.substr(0, bar)); }
				}
			}
			catch (...) {}
			if (auto* dh = RE::TESDataHandler::GetSingleton()) { g_werebearMenu = dh->LookupForm<RE::TESGlobal>(0x814, "Tales of Skyrim - Berserkyr.esp"); }
			logger::info("stats menu: werebear tree swap {} (race {}, Berserkyr menu global {})", g_werebearRace && g_werebearMenu ? "ready" : "off",
				g_werebearRace ? "found" : "not found", g_werebearMenu ? "found" : "not found");
		}
		std::atomic<bool> g_boxOpen{ false };
		std::atomic<std::uint32_t> g_lineShown{ 0 };
		std::atomic<std::uint32_t> g_boxesOpened{ 0 };
		std::atomic<std::uint32_t> g_treesHidden{ 0 };
		std::mutex g_lock;
		std::string g_lastLine;
		std::string g_lastProblem;
		std::vector<std::string> g_boxOptionIds;   // the option ids the open box offers, in button order

		void Problem(const std::string& a_what)
		{
			bool fresh = false;
			{
				std::scoped_lock l(g_lock);
				fresh = g_lastProblem != a_what;
				g_lastProblem = a_what;
			}
			if (fresh) { logger::warn("stats menu class line: {} - the line is not shown; the Classes tab shows it", a_what); }
		}

		// UI thread: creates (once per menu) and fills the class-line text field on the Stats Menu's root.
		void ShowLine(std::uint32_t a_session)
		{
			if (!g_open.load() || g_session.load() != a_session) { return; }
			auto* ui = RE::UI::GetSingleton();
			auto menu = ui ? ui->GetMenu(RE::StatsMenu::MENU_NAME) : RE::GPtr<RE::IMenu>{};
			if (!menu || !menu->uiMovie) { return; }
			RE::GFxMovieView* movie = menu->uiMovie.get();
			const auto snap = Classes::GetSnapshot();
			if (!snap.tableLoaded || !snap.enabled) { return; }
			std::string text = snap.line;
			// the line itself says "ready to evolve" once the class can evolve (Classes::LineLocked)

			RE::GFxValue root;
			if (!movie->GetVariable(&root, "_root") || !root.IsObject()) { Problem("the Stats Menu has no _root"); return; }
			RE::GFxValue field;
			if (!root.GetMember(kFieldName, &field) || !field.IsObject())
			{
				const auto rect = movie->GetVisibleFrameRect();
				const double w = static_cast<double>(rect.right - rect.left);
				const double h = static_cast<double>(rect.bottom - rect.top);
				if (w <= 1.0 || h <= 1.0) { return; }   // not laid out yet; the next poll tries again
				RE::GFxValue depth;
				root.Invoke("getNextHighestDepth", &depth);
				std::array<RE::GFxValue, 6> args;
				RE::GFxValue name;
				movie->CreateString(&name, kFieldName);
				args[0] = name;
				args[1] = depth.IsNumber() ? depth : RE::GFxValue(9000.0);
				args[2] = RE::GFxValue(rect.left + w * 0.15);
				args[3] = RE::GFxValue(rect.top + h * 0.06);
				args[4] = RE::GFxValue(w * 0.70);
				args[5] = RE::GFxValue(40.0);
				if (!root.Invoke("createTextField", &field, args) || !field.IsObject())
				{
					// Flash 7 movies return nothing from createTextField: look the field up by name instead
					if (!root.GetMember(kFieldName, &field) || !field.IsObject()) { Problem("createTextField did not create the field"); return; }
				}
				RE::GFxValue fmt;
				movie->CreateObject(&fmt, "TextFormat");
				if (fmt.IsObject())
				{
					RE::GFxValue font, align;
					movie->CreateString(&font, "$EverywhereMediumFont");
					movie->CreateString(&align, "center");
					fmt.SetMember("font", font);
					fmt.SetMember("size", RE::GFxValue(22.0));
					fmt.SetMember("color", RE::GFxValue(static_cast<double>(0xFFFFFF)));
					fmt.SetMember("align", align);
					field.Invoke("setNewTextFormat", std::array<RE::GFxValue, 1>{ fmt });
				}
				field.SetMember("embedFonts", RE::GFxValue(true));
				field.SetMember("selectable", RE::GFxValue(false));
				logger::info("stats menu class line: text field created on the Stats Menu ({}x{} visible)", w, h);
			}
			RE::GFxValue value;
			movie->CreateString(&value, text.c_str());
			field.SetMember("text", value);
			{
				std::scoped_lock l(g_lock);
				if (g_lastLine != text) { logger::debug("stats menu class line: \"{}\"", text); }
				g_lastLine = text;
			}
			++g_lineShown;
		}

		struct EvolveCallback : RE::IMessageBoxCallback
		{
#if RUNTIME_LINE == 17
			void Run(std::uint8_t a_index) override { RequestPick(static_cast<int>(a_index)); }
#else
			void Run(Message a_index) override { RequestPick(static_cast<int>(a_index)); }
#endif
		};

		// Main thread: the evolution box, when an upgrade is due.
		void OpenBox()
		{
			if (!g_open.load() || g_boxOpen.load()) { return; }
			const auto snap = Classes::GetSnapshot();
			if (!snap.upgradeDue || snap.options.empty()) { return; }
			RE::BSTArray<RE::BSString> buttons;
			std::vector<std::string> ids;
			std::string body = strings::TR("NJ_StatsEvolveTitle", "Your class can evolve. Choose one - the other is gone for good.");
			body += "\n\n" + snap.line;
			for (const auto& o : snap.options)
			{
				if (!o.available) { continue; }
				buttons.push_back(RE::BSString(o.label.c_str()));
				ids.push_back(o.id);
				body += "\n\n" + o.label + ": " + o.description;
			}
			if (ids.empty()) { return; }
			buttons.push_back(RE::BSString(strings::TR("NJ_StatsEvolveLater", "Later")));
			{
				std::scoped_lock l(g_lock);
				g_boxOptionIds = ids;
			}
			g_boxOpen = true;
			++g_boxesOpened;
			RE::BSTSmartPointer<RE::IMessageBoxCallback> callback = RE::make_smart<EvolveCallback>();
			RE::NJ_UI__OpenMessageBox(RE::BSString(body.c_str()), callback, 0, 25, 4, buttons);
			logger::info("stats menu: evolution box opened with {} option(s)", ids.size());
		}

		// PLAN.md phase 5 step 2: the skills menu per class. The ring of skill names is the movie's
		// AnimatingSkillTextInstance with one clip per tree, SkillText0..17, in the order of the menu's own skillTrees
		// array (statsmenu.swf's AnimatedSkillText, read with FFDec from Norden UI - Black and the same in vanilla). A tree
		// the class locks has its name hidden from the ring; its perks already cannot be taken (Lockouts). The
		// constellation itself still turns past - removing it from the rotation is engine work, not done here.
		void HideLockedTrees(std::uint32_t a_session)
		{
			if (!g_open.load() || g_session.load() != a_session) { return; }
			auto* ui = RE::UI::GetSingleton();
			auto menu = ui ? ui->GetMenu<RE::StatsMenu>() : nullptr;
			if (!menu || !menu->uiMovie) { return; }
			RE::GFxMovieView* movie = menu->uiMovie.get();
			const auto snap = Classes::GetSnapshot();
			const bool want = settings::classes::hideLockedTrees && snap.enabled && snap.chosen;
			const auto& trees = menu->GetRuntimeData().skillTrees;
			// efc8: the ring's text clips appear as the movie lays itself out - an early poll found SkillText0 but not
			// SkillText1, warned once for good and never said it worked later. Missing clips are skipped (the next poll
			// tries again), and one line per menu session reports the outcome once the layout has had time (poll 12).
			static std::uint32_t s_reported = 0;
			static int s_polls = 0;
			static std::uint32_t s_pollSession = 0;
			if (s_pollSession != a_session) { s_pollSession = a_session; s_polls = 0; }
			++s_polls;
			RE::GFxValue ring;
			const bool haveRing = movie->GetVariable(&ring, "_root.StatsMenuBaseInstance.AnimatingSkillTextInstance") && ring.IsDisplayObject();
			std::uint32_t hidden = 0, found = 0, missing = 0;
			// the 18-entry limit lifted: with the framework's ring installed every slot is walked, by the ring's own order
			const bool ringOn = !g_ring.empty() && trees.size() == g_ring.size();
			const std::uint32_t count = ringOn ? static_cast<std::uint32_t>(trees.size()) : std::min<std::uint32_t>(static_cast<std::uint32_t>(trees.size()), static_cast<std::uint32_t>(skilllist::kCount));
			for (std::uint32_t i = 0; haveRing && i < count; ++i)
			{
				int skill = static_cast<int>(trees[i]) - static_cast<int>(RE::ActorValue::kOneHanded);
				int group = -1;
				if (ringOn) { skill = g_ring[i].skill; group = g_ring[i].group; }
				bool locked = want && skill >= 0 && skill < skilllist::kCount && !snap.held[static_cast<std::size_t>(skill)];
				if (want && skill < 0 && group >= 0) { locked = !snap.groupsHeld[static_cast<std::size_t>(group)]; }   // a custom tree: its class group
				RE::GFxValue clip;
				const auto name = fmt::format("SkillText{}", i);
				if (!ring.GetMember(name.c_str(), &clip) || !clip.IsDisplayObject()) { ++missing; continue; }
				++found;
				RE::GFxValue::DisplayInfo info;
				clip.GetDisplayInfo(&info);
				if (info.GetVisible() == locked)
				{
					info.SetVisible(!locked);
					clip.SetDisplayInfo(info);
				}
				hidden += locked ? 1 : 0;
			}
			if (s_polls == 12 && s_reported != a_session)
			{
				s_reported = a_session;
				// 02:28 (constellations out of step with the trees in the 20-tree ring): the menu's tree order, slot by slot -
				// with the ring mod unticked this is the game's own order, which the vanilla sky's pictures follow
				std::string order;
				for (std::uint32_t i = 0; i < trees.size(); ++i)
				{
					const int av = static_cast<int>(trees[i]);
					const int sk = av - static_cast<int>(RE::ActorValue::kOneHanded);
					order += fmt::format("{}{}={}", i ? " " : "", i, (sk >= 0 && sk < skilllist::kCount) ? skilllist::kIniName[sk] : fmt::format("av{}", av));
				}
				logger::info("stats menu: tree order {}", order);
				if (!haveRing) { logger::warn("stats menu: this statsmenu.swf has no AnimatingSkillTextInstance - locked trees are not hidden (rule 30)"); }
				else { logger::info("stats menu: the ring holds {} trees; {} text clips found, {} missing; {} hidden by the class", trees.size(), found, missing, hidden); }
			}
			g_treesHidden = hidden;
		}

		void Worker(std::uint32_t a_session)
		{
			// The menu's movie lays itself out after it opens; poll a few times, then keep the line current while it is open.
			for (int i = 0; i < 4 * 60 * 30 && g_open.load() && g_session.load() == a_session; ++i)
			{
				if (auto* tasks = SKSE::GetTaskInterface())
				{
					tasks->AddUITask([a_session]() { ShowLine(a_session); HideLockedTrees(a_session); });
					if (i == 4) { tasks->AddTask([]() { OpenBox(); }); }
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(i < 8 ? 125 : 500));
			}
		}

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || a_event->menuName != RE::StatsMenu::MENU_NAME) { return RE::BSEventNotifyControl::kContinue; }
				if (a_event->opening && g_werebearRace && g_werebearMenu)
				{
					// bfbb: the menu the framework opens for the werebear tree is the Stats Menu too - intercepting it again looped
					// (41 times in 70 s); one hand-off, then the next opening within 5 s is left alone
					static std::int64_t s_handedOff = 0;
					const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
					auto* pl = RE::PlayerCharacter::GetSingleton();
					if (pl && pl->GetRace() == g_werebearRace && now - s_handedOff > 5000)
					{
						s_handedOff = now;
						if (auto* q = RE::UIMessageQueue::GetSingleton()) { q->AddMessage(RE::StatsMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr); }
						g_werebearMenu->value = 1.0F;   // the framework opens Berserkyr's werebear tree
						logger::info("stats menu: werebear form - the werebear tree opens in place of the werewolf tree");
						return RE::BSEventNotifyControl::kContinue;
					}
				}
				if (a_event->opening)
				{
					g_open = true;
					const auto session = ++g_session;
					if (settings::classes::enabled) { std::thread(Worker, session).detach(); }
				}
				else
				{
					g_open = false;
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MenuSink g_sink;
	}

	void RequestPick(int a_index)
	{
		if (!g_boxOpen.exchange(false)) { logger::debug("stats menu: evolution choice {} ignored - no box is open", a_index); return; }
		std::vector<std::string> ids;
		{
			std::scoped_lock l(g_lock);
			ids.swap(g_boxOptionIds);
		}
		if (a_index < 0 || a_index >= static_cast<int>(ids.size())) { logger::info("stats menu: evolution put off (Later)"); return; }
		// The box listed only the available options: find the chosen one's index among all options on offer.
		const auto snap = Classes::GetSnapshot();
		for (int k = 0; k < static_cast<int>(snap.options.size()); ++k)
		{
			if (snap.options[k].id == ids[a_index])
			{
				std::string why;
				if (!Classes::RequestChooseUpgrade(k, why)) { logger::warn("stats menu: evolution choice {} refused: {}", ids[a_index], why); }
				if (auto* tasks = SKSE::GetTaskInterface()) { const auto s = g_session.load(); tasks->AddUITask([s]() { ShowLine(s); }); }
				return;
			}
		}
		logger::warn("stats menu: the chosen option {} is no longer on offer", ids[a_index]);
	}

	void Install()
	{
		LoadRing();
		ResolveWerebear();
		if (auto* ui = RE::UI::GetSingleton()) { ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_sink); }
		logger::info("stats menu class line: menu sink registered");
	}

	std::string StatusJson()
	{
		nlohmann::json j;
		j["menuOpen"] = g_open.load();
		j["treesHidden"] = g_treesHidden.load();
		j["boxOpen"] = g_boxOpen.load();
		j["linesShown"] = g_lineShown.load();
		j["boxesOpened"] = g_boxesOpened.load();
		std::scoped_lock l(g_lock);
		j["lastLine"] = g_lastLine;
		j["lastProblem"] = g_lastProblem;
		j["boxOptions"] = g_boxOptionIds;
		return j.dump();
	}
}
