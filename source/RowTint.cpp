#include "PCH.h"

// Njordlinger Combat Gameplay Overhaul - inventory rows tinted by item tier (the owner 02:09: "colour the bar that the
// icon and text sit within"; research in the session's rowtint notes).
//
// When the inventory, a container or the barter menu opens, InventoryListEntry.prototype.setEntry (the item list's row
// class - SkyUI's, and Norden UI Black's which keeps it) is wrapped once per movie with a native function: the original
// runs first (I4 colours the icon inside it), then a child clip "njTierBar" under the row's own clips is filled with the
// item's tier colour - the same colours as I4's icons - at a low alpha. Common items get no bar. Nothing in any SWF is
// edited; the wrap is marked on the prototype so it is never applied twice.

#include "RowTint.h"

#include "Settings.h"

#include "utils/Logger.h"

#include <atomic>

namespace RowTint
{
	namespace
	{
		std::atomic<std::uint32_t> g_wraps{ 0 };
		std::atomic<std::uint32_t> g_bars{ 0 };
		std::atomic<bool> g_layerWarned{ false };

		std::uint32_t TierColour(RE::TESForm* a_form)
		{
			auto* kf = a_form ? a_form->As<RE::BGSKeywordForm>() : nullptr;
			if (!kf) { return 0; }
			if (kf->HasKeywordString("NJCGO_Tier_Unique")) { return 0xE6312B; }
			if (kf->HasKeywordString("NJCGO_Tier_Legendary")) { return 0xFF8000; }
			if (kf->HasKeywordString("NJCGO_Tier_Epic")) { return 0xA335EE; }
			if (kf->HasKeywordString("NJCGO_Tier_Magic")) { return 0x3A8EF0; }
			return 0;
		}

		double Num(const RE::GFxValue& a_obj, const char* a_name, double a_default)
		{
			RE::GFxValue v;
			return (a_obj.GetMember(a_name, &v) && v.IsNumber()) ? v.GetNumber() : a_default;
		}

		void DrawBar(RE::GFxValue& a_row, RE::GFxValue& a_entry)
		{
			// eacb: a child clip below depth -16384 could not be made ("rows stay untinted"). The row clip's own drawing-API
			// content is rendered beneath all of its children, so the bar is drawn on the row itself (its timeline shapes and
			// clips are children; clear() only removes what the drawing API drew).
			RE::GFxValue& bar = a_row;
			static std::atomic<bool> s_told{ false };
			if (!s_told.exchange(true)) { logger::info("row tint: bars are drawn on the row clips themselves (beneath their children)"); }
			bar.Invoke("clear");
			RE::GFxValue formId;
			const std::uint32_t colour = settings::gear::tierRows && a_entry.GetMember("formId", &formId) && formId.IsNumber()
			                                 ? TierColour(RE::TESForm::LookupByID(static_cast<RE::FormID>(formId.GetNumber())))
			                                 : 0;
			if (!colour) { return; }
			// the row's size: its hit area (background, sized by the list layout), else the row clip itself
			RE::GFxValue bg;
			double x = 0.0, y = 0.0, w = Num(a_row, "_width", 0.0), h = Num(a_row, "_height", 0.0);
			if (a_row.GetMember("background", &bg) && bg.IsDisplayObject())
			{
				x = Num(bg, "_x", 0.0);
				y = Num(bg, "_y", 0.0);
				w = Num(bg, "_width", w);
				h = Num(bg, "_height", h);
			}
			if (w <= 1.0 || h <= 1.0) { return; }
			std::array<RE::GFxValue, 2> fill{ RE::GFxValue(static_cast<double>(colour)), RE::GFxValue(28.0) };
			bar.Invoke("beginFill", nullptr, fill.data(), fill.size());
			auto point = [&](const char* a_fn, double a_x, double a_y) {
				std::array<RE::GFxValue, 2> p{ RE::GFxValue(a_x), RE::GFxValue(a_y) };
				bar.Invoke(a_fn, nullptr, p.data(), p.size());
			};
			point("moveTo", x, y);
			point("lineTo", x + w, y);
			point("lineTo", x + w, y + h);
			point("lineTo", x, y + h);
			point("lineTo", x, y);
			bar.Invoke("endFill");
			++g_bars;
		}

		class SetEntryHandler : public RE::GFxFunctionHandler
		{
		public:
			void Call(Params& a_params) override
			{
				if (!a_params.thisPtr) { return; }
				RE::GFxValue ret;
				a_params.thisPtr->Invoke("njOrigSetEntry", &ret, a_params.args, a_params.argCount);
				if (a_params.argCount >= 1 && a_params.args[0].IsObject()) { DrawBar(*a_params.thisPtr, a_params.args[0]); }
				if (a_params.retVal) { *a_params.retVal = ret; }
			}
		};
		SetEntryHandler* g_handler = nullptr;   // owned by Scaleform's reference count once wrapped

		void Wrap(RE::GFxMovieView* a_movie)
		{
			RE::GFxValue proto;
			if (!a_movie || !a_movie->GetVariable(&proto, "_global.InventoryListEntry.prototype") || !proto.IsObject()) { return; }
			RE::GFxValue mark;
			if (proto.GetMember("njTintWrapped", &mark) && mark.IsBool()) { return; }
			RE::GFxValue orig;
			if (!proto.GetMember("setEntry", &orig) || orig.IsUndefined()) { logger::warn("row tint: this list has no InventoryListEntry.setEntry - rows stay untinted"); return; }
			if (!g_handler) { g_handler = new SetEntryHandler(); }
			RE::GFxValue fn;
			a_movie->CreateFunction(&fn, g_handler);
			proto.SetMember("njOrigSetEntry", orig);
			proto.SetMember("setEntry", fn);
			proto.SetMember("njTintWrapped", RE::GFxValue(true));
			++g_wraps;
			logger::info("row tint: the item list's rows are tinted by tier (setEntry wrapped)");
		}

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_e || !a_e->opening) { return RE::BSEventNotifyControl::kContinue; }
				const auto& name = a_e->menuName;
				if (name != RE::InventoryMenu::MENU_NAME && name != RE::ContainerMenu::MENU_NAME && name != RE::BarterMenu::MENU_NAME && name != RE::GiftMenu::MENU_NAME)
				{
					return RE::BSEventNotifyControl::kContinue;
				}
				const std::string menu = name.c_str();
				if (auto* tasks = SKSE::GetTaskInterface())
				{
					tasks->AddUITask([menu]() {
						auto* ui = RE::UI::GetSingleton();
						auto m = ui ? ui->GetMenu(menu) : nullptr;
						if (m && m->uiMovie) { Wrap(m->uiMovie.get()); }
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MenuSink g_sink;
	}

	void Install()
	{
		if (auto* ui = RE::UI::GetSingleton()) { ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_sink); }
		logger::info("row tint: menu sink registered");
	}

	std::uint32_t Bars() { return g_bars.load(); }
}
