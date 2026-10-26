#include "PCH.h"

#include "RaceMenuClass.h"

#include "ClassData.h"
#include "Classes.h"
#include "Settings.h"

#include "utils/Logger.h"
#include "utils/Strings.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cctype>
#include <mutex>
#include <thread>

namespace RaceMenuClass
{
	namespace
	{
		constexpr double kClassFlag = 536870912.0;   // 2^29 - the CLASS category's filter flag
		constexpr const char* kPanelPath = "_root.RaceSexMenuBaseInstance.RaceSexPanelsInstance";

		std::atomic<bool> g_open{ false };
		std::atomic<std::uint32_t> g_session{ 0 };   // bumps on every open, so a stale worker stops
		std::atomic<std::uint32_t> g_injections{ 0 };
		std::atomic<bool> g_handlersReplaced{ false };
		std::mutex g_lock;
		std::string g_pending;        // class id picked in the menu, committed on close
		std::string g_lastProblem;    // what was missing, for the status tool
		std::string g_lastCommit;

		void Problem(const std::string& a_what)
		{
			bool fresh = false;
			{
				std::scoped_lock l(g_lock);
				fresh = g_lastProblem != a_what;
				g_lastProblem = a_what;
			}
			if (fresh) { logger::warn("race menu class: {} - the menu is left alone; the class can be chosen on the Classes tab", a_what); }
		}

		bool Member(const RE::GFxValue& a_obj, const char* a_name, RE::GFxValue& a_out)
		{
			return a_obj.IsObject() && a_obj.GetMember(a_name, &a_out) && !a_out.IsUndefined() && !a_out.IsNull();
		}

		std::string Upper(std::string a_s)
		{
			// ASCII upper case only: RaceMenu upper-cases its own category names the same way in ActionScript, and a
			// non-ASCII byte is left as it is rather than broken.
			for (char& c : a_s)
			{
				if (static_cast<unsigned char>(c) < 128) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
			}
			return a_s;
		}

		struct Panel
		{
			RE::GFxValue panel, itemList, items, categoryList, categories, raceDescription;
		};

		bool Resolve(RE::GFxMovieView* a_movie, Panel& a_p)
		{
			if (!a_movie->GetVariable(&a_p.panel, kPanelPath) || !a_p.panel.IsObject()) { Problem(std::string(kPanelPath) + " is not in the movie (not RaceMenu's race menu?)"); return false; }
			if (!Member(a_p.panel, "itemList", a_p.itemList)) { Problem("RaceSexPanelsInstance.itemList is missing"); return false; }
			if (!Member(a_p.itemList, "entryList", a_p.items) || !a_p.items.IsArray()) { Problem("itemList.entryList is missing"); return false; }
			if (!Member(a_p.panel, "categoryList", a_p.categoryList)) { Problem("RaceSexPanelsInstance.categoryList is missing"); return false; }
			if (!Member(a_p.categoryList, "entryList", a_p.categories) || !a_p.categories.IsArray()) { Problem("categoryList.entryList is missing"); return false; }
			Member(a_p.panel, "raceDescription", a_p.raceDescription);   // optional: the description falls back to nothing
			return true;
		}

		bool EntryIsClass(const RE::GFxValue& a_entry, std::string* a_id)
		{
			RE::GFxValue flag, id;
			if (!a_entry.IsObject() || !a_entry.GetMember("isClass", &flag) || !flag.IsBool() || !flag.GetBool()) { return false; }
			if (a_id && a_entry.GetMember("callbackName", &id) && id.IsString()) { *a_id = id.GetString(); }
			return true;
		}

		// The entry the event names: itemList.entryList[event.index].
		bool EventEntry(const RE::GFxFunctionHandler::Params& a_params, RE::GFxValue& a_entry)
		{
			if (a_params.argCount < 1 || !a_params.args || !a_params.thisPtr) { return false; }
			RE::GFxValue index, itemList, items;
			if (!Member(a_params.args[0], "index", index) || !index.IsNumber()) { return false; }
			if (!Member(*a_params.thisPtr, "itemList", itemList) || !Member(itemList, "entryList", items) || !items.IsArray()) { return false; }
			const auto i = static_cast<std::int64_t>(index.GetNumber());
			if (i < 0 || i >= static_cast<std::int64_t>(items.GetArraySize())) { return false; }
			return items.GetElement(static_cast<std::uint32_t>(i), &a_entry) && a_entry.IsObject();
		}

		void ShowDescription(RE::GFxValue& a_panel, const RE::GFxValue& a_entry, bool a_show)
		{
			RE::GFxValue desc, field, text;
			if (!Member(a_panel, "raceDescription", desc)) { return; }
			if (Member(desc, "textField", field))
			{
				if (a_show && a_entry.GetMember("raceDescription", &text) && text.IsString()) { field.Invoke("SetText", std::array<RE::GFxValue, 1>{ text }); }
			}
			RE::GFxValue show(a_show);
			a_panel.Invoke("ShowRaceDescription", std::array<RE::GFxValue, 1>{ show });
		}

		class ItemPressHandler : public RE::GFxFunctionHandler
		{
		public:
			void Call(Params& a_params) override
			{
				RE::GFxValue entry;
				std::string id;
				if (EventEntry(a_params, entry) && EntryIsClass(entry, &id))
				{
					{
						std::scoped_lock l(g_lock);
						g_pending = id;
					}
					RE::GFxValue itemList, listState;
					if (Member(*a_params.thisPtr, "itemList", itemList) && Member(itemList, "listState", listState))
					{
						listState.SetMember("activeEntry", entry);
						itemList.Invoke("requestUpdate");
					}
					ShowDescription(*a_params.thisPtr, entry, true);
					logger::info("race menu class: {} picked (committed when the menu closes)", id);
					return;
				}
				if (a_params.thisPtr) { a_params.thisPtr->Invoke("njOrigItemPress", nullptr, a_params.args, a_params.argCount); }
			}
		};

		class SelectionHandler : public RE::GFxFunctionHandler
		{
		public:
			void Call(Params& a_params) override
			{
				RE::GFxValue entry;
				if (EventEntry(a_params, entry) && EntryIsClass(entry, nullptr))
				{
					RE::GFxValue itemList, listState;
					if (Member(*a_params.thisPtr, "itemList", itemList) && Member(itemList, "listState", listState)) { listState.SetMember("selectedEntry", entry); }
					ShowDescription(*a_params.thisPtr, entry, true);
					return;
				}
				if (a_params.thisPtr) { a_params.thisPtr->Invoke("njOrigSelectionChange", nullptr, a_params.args, a_params.argCount); }
			}
		};

		ItemPressHandler* g_press = nullptr;     // owned by Scaleform's reference count once wrapped; never freed by us
		SelectionHandler* g_select = nullptr;

		bool HasMarked(const RE::GFxValue& a_array, const char* a_mark)
		{
			const auto n = a_array.GetArraySize();
			for (std::uint32_t i = 0; i < n; ++i)
			{
				RE::GFxValue e, m;
				if (a_array.GetElement(i, &e) && e.IsObject() && e.GetMember(a_mark, &m) && m.IsBool() && m.GetBool()) { return true; }
			}
			return false;
		}

		// A race entry of the menu's own, to borrow its isColorEnabled / hasColor / hasGlow functions (RaceMenu calls
		// them on whatever entry is selected).
		bool FindRaceEntry(const RE::GFxValue& a_items, RE::GFxValue& a_out)
		{
			const auto n = a_items.GetArraySize();
			for (std::uint32_t i = 0; i < n; ++i)
			{
				RE::GFxValue e, type, isClass;
				if (!a_items.GetElement(i, &e) || !e.IsObject() || !e.GetMember("type", &type) || !type.IsNumber() || type.GetNumber() != 1.0) { continue; }
				if (e.GetMember("isClass", &isClass) && isClass.IsBool() && isClass.GetBool()) { continue; }
				a_out = e;
				return true;
			}
			return false;
		}

		void Tick(std::uint32_t a_session)
		{
			if (!g_open.load() || g_session.load() != a_session) { return; }
			if (!settings::classes::enabled || !settings::classes::raceMenuClass || !ClassData::Get().loaded) { return; }
			if (Classes::GetSnapshot().chosen) { return; }   // the start is chosen once
			auto* ui = RE::UI::GetSingleton();
			auto menu = ui ? ui->GetMenu(RE::RaceSexMenu::MENU_NAME) : RE::GPtr<RE::IMenu>{};
			if (!menu || !menu->uiMovie) { return; }
			RE::GFxMovieView* movie = menu->uiMovie.get();
			Panel p;
			if (!Resolve(movie, p)) { return; }
			RE::GFxValue race;
			if (p.categories.GetArraySize() < 2 || !FindRaceEntry(p.items, race)) { return; }   // the menu has not filled its lists yet

			bool changed = false;
			if (!HasMarked(p.categories, "njClassCat"))
			{
				RE::GFxValue cat;
				movie->CreateObject(&cat);
				RE::GFxValue text;
				movie->CreateString(&text, Upper(strings::TR("NJ_RaceMenuCategory", "Class")).c_str());
				cat.SetMember("type", RE::GFxValue(0.0));
				cat.SetMember("bDontHide", RE::GFxValue(false));
				cat.SetMember("filterFlag", RE::GFxValue(1.0));
				cat.SetMember("text", text);
				cat.SetMember("flag", RE::GFxValue(kClassFlag));
				cat.SetMember("priority", RE::GFxValue(-990.0));   // right after the race category (CATEGORY_PRIORITY_START -1000, step 25)
				cat.SetMember("enabled", RE::GFxValue(true));
				cat.SetMember("njClassCat", RE::GFxValue(true));
				p.categories.PushBack(cat);
				p.categoryList.Invoke("requestInvalidate");
				changed = true;
			}
			if (!HasMarked(p.items, "isClass"))
			{
				RE::GFxValue isColorEnabled, hasColor, hasGlow;
				race.GetMember("isColorEnabled", &isColorEnabled);
				race.GetMember("hasColor", &hasColor);
				race.GetMember("hasGlow", &hasGlow);
				for (const auto& c : Classes::StartClasses())
				{
					RE::GFxValue e, name, desc, id;
					movie->CreateObject(&e);
					movie->CreateString(&name, c.name.c_str());
					movie->CreateString(&desc, c.description.c_str());
					movie->CreateString(&id, c.id.c_str());
					e.SetMember("type", RE::GFxValue(1.0));
					e.SetMember("text", name);
					e.SetMember("filterFlag", RE::GFxValue(kClassFlag));
					e.SetMember("raceDescription", desc);
					e.SetMember("equipState", RE::GFxValue(0.0));
					e.SetMember("raceID", RE::GFxValue(-1.0));
					e.SetMember("enabled", RE::GFxValue(true));
					e.SetMember("isClass", RE::GFxValue(true));
					e.SetMember("callbackName", id);
					if (!isColorEnabled.IsUndefined()) { e.SetMember("isColorEnabled", isColorEnabled); }
					if (!hasColor.IsUndefined()) { e.SetMember("hasColor", hasColor); }
					if (!hasGlow.IsUndefined()) { e.SetMember("hasGlow", hasGlow); }
					p.items.PushBack(e);
				}
				p.itemList.Invoke("requestInvalidate");
				changed = true;
			}
			// The handlers: replaced once per menu (the panel is rebuilt with the menu).
			RE::GFxValue mark;
			if (!p.panel.GetMember("njHandlers", &mark) || !mark.IsBool())
			{
				RE::GFxValue origPress, origSelect;
				if (!Member(p.panel, "onItemPress", origPress) || !Member(p.panel, "onSelectionChange", origSelect))
				{
					Problem("the panel's onItemPress / onSelectionChange are missing");
					return;
				}
				if (!g_press) { g_press = new ItemPressHandler(); }
				if (!g_select) { g_select = new SelectionHandler(); }
				RE::GFxValue press, select;
				movie->CreateFunction(&press, g_press);
				movie->CreateFunction(&select, g_select);
				p.panel.SetMember("njOrigItemPress", origPress);
				p.panel.SetMember("njOrigSelectionChange", origSelect);
				p.panel.SetMember("onItemPress", press);
				p.panel.SetMember("onSelectionChange", select);
				p.panel.SetMember("njHandlers", RE::GFxValue(true));
				g_handlersReplaced = true;
				changed = true;
			}
			if (changed)
			{
				++g_injections;
				logger::info("race menu class: the Class category and {} starting classes are in the race menu (injection {})", Classes::StartClasses().size(), g_injections.load());
			}
		}

		void Worker(std::uint32_t a_session)
		{
			// Polls while the menu is open: it fills its lists after it opens, and refills them (dropping ours) when the
			// race changes. Each poll is a UI task; the worker itself touches nothing of the game.
			for (int i = 0; i < 4 * 60 * 60 && g_open.load() && g_session.load() == a_session; ++i)
			{
				if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddUITask([a_session]() { Tick(a_session); }); }
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
			}
		}

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || a_event->menuName != RE::RaceSexMenu::MENU_NAME) { return RE::BSEventNotifyControl::kContinue; }
				if (a_event->opening)
				{
					g_open = true;
					const auto session = ++g_session;
					g_handlersReplaced = false;
					if (settings::classes::enabled && settings::classes::raceMenuClass)
					{
						std::thread(Worker, session).detach();
						logger::debug("race menu class: race menu opened (session {})", session);
					}
					return RE::BSEventNotifyControl::kContinue;
				}
				g_open = false;
				std::string pick;
				{
					std::scoped_lock l(g_lock);
					pick.swap(g_pending);
				}
				if (!pick.empty())
				{
					std::string why;
					const bool ok = Classes::RequestChooseStart(pick, why);
					{
						std::scoped_lock l(g_lock);
						g_lastCommit = ok ? pick : pick + " refused: " + why;
					}
					logger::info("race menu class: menu closed - {} {}", pick, ok ? "committed as the starting class" : "refused: " + why);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MenuSink g_sink;
	}

	void Install()
	{
		if (auto* ui = RE::UI::GetSingleton()) { ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_sink); }
		logger::info("race menu class: menu sink registered (setting {})", settings::classes::raceMenuClass);
	}

	std::string StatusJson()
	{
		nlohmann::json j;
		j["setting"] = settings::classes::raceMenuClass;
		j["menuOpen"] = g_open.load();
		j["injections"] = g_injections.load();
		j["handlersReplaced"] = g_handlersReplaced.load();
		std::scoped_lock l(g_lock);
		j["pending"] = g_pending;
		j["lastCommit"] = g_lastCommit;
		j["lastProblem"] = g_lastProblem;
		return j.dump();
	}
}
