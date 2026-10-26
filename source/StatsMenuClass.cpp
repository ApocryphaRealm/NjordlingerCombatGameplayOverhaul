#include "PCH.h"

#include "StatsMenuClass.h"

#include "Classes.h"
#include "Settings.h"

#include "utils/Logger.h"
#include "utils/Strings.h"

#include <nlohmann/json.hpp>

#include <atomic>
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
		std::atomic<bool> g_boxOpen{ false };
		std::atomic<std::uint32_t> g_lineShown{ 0 };
		std::atomic<std::uint32_t> g_boxesOpened{ 0 };
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
			if (snap.upgradeDue) { text += "  -  " + std::string(strings::TR("NJ_StatsEvolveHint", "your class can evolve")); }

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

		void Worker(std::uint32_t a_session)
		{
			// The menu's movie lays itself out after it opens; poll a few times, then keep the line current while it is open.
			for (int i = 0; i < 4 * 60 * 30 && g_open.load() && g_session.load() == a_session; ++i)
			{
				if (auto* tasks = SKSE::GetTaskInterface())
				{
					tasks->AddUITask([a_session]() { ShowLine(a_session); });
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
		if (auto* ui = RE::UI::GetSingleton()) { ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_sink); }
		logger::info("stats menu class line: menu sink registered");
	}

	std::string StatusJson()
	{
		nlohmann::json j;
		j["menuOpen"] = g_open.load();
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
