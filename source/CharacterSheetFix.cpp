#include "PCH.h"

// Njordlinger Combat Gameplay Overhaul - Character Menu SE's character sheet follows the class (combined run f2d7, D32).
//
// Character Menu SE (interface/charactersheet.swf, class CharacterSheet) fills its SKILLS page through
// CharacterSheet.prototype.SetSkills(skillsData, attributedClass, classSpecialization, classDescription). It listed every
// vanilla skill - naming trees a Warrior does not have (D32) - and showed the engine's class ("Pilgrim"). The method is
// wrapped once per movie: skills the class does not hold are taken out of skillsData (matched by the game's own skill
// names, so renamed skills like Hand to Hand follow), and the class line shows our class and its description. Nothing in
// the SWF is edited; with no class chosen or classes off, the sheet is untouched.

#include "CharacterSheetFix.h"

#include "ClassData.h"
#include "Classes.h"
#include "Settings.h"

#include "utils/Logger.h"

namespace CharacterSheetFix
{
	namespace
	{
		std::string SkillName(int a_skill)
		{
			const auto av = static_cast<RE::ActorValue>(static_cast<int>(RE::ActorValue::kOneHanded) + a_skill);
#if RUNTIME_LINE == 17
			auto* info = RE::ActorValueList::GetActorValueInfo(av);
#else
			auto* list = RE::ActorValueList::GetSingleton();
			auto* info = list ? list->GetActorValue(av) : nullptr;
#endif
			return info && info->GetFullName() ? info->GetFullName() : "";
		}

		class SetSkillsHandler : public RE::GFxFunctionHandler
		{
		public:
			void Call(Params& a_params) override
			{
				if (!a_params.thisPtr) { return; }
				std::vector<RE::GFxValue> args(a_params.args, a_params.args + a_params.argCount);
				const auto snap = Classes::GetSnapshot();
				if (snap.enabled && snap.chosen && !args.empty() && args[0].IsArray())
				{
					std::vector<std::string> hidden;
					for (int i = 0; i < skilllist::kCount; ++i) { if (!Classes::Holds(i)) { hidden.push_back(SkillName(i)); } }
					auto& arr = args[0];
					std::uint32_t removed = 0;
					for (std::uint32_t i = arr.GetArraySize(); i-- > 0;)
					{
						RE::GFxValue entry, name;
						if (!arr.GetElement(i, &entry) || !entry.GetMember("skillName", &name) || !name.IsString()) { continue; }
						const std::string n = name.GetString();
						if (std::find(hidden.begin(), hidden.end(), n) != hidden.end()) { arr.RemoveElement(i); ++removed; }
					}
					if (args.size() >= 2) { args[1] = RE::GFxValue(snap.className.c_str()); }
					if (args.size() >= 4)
					{
						if (const auto* n = ClassData::Find(snap.classId)) { args[3] = RE::GFxValue(n->description.c_str()); }
					}
					static std::atomic<bool> s_told{ false };
					if (!s_told.exchange(true)) { logger::info("character sheet: {} skill(s) the class does not hold left out; class shown as {}", removed, snap.className); }
				}
				RE::GFxValue ret;
				a_params.thisPtr->Invoke("njOrigSetSkills", &ret, args.data(), args.size());
				if (a_params.retVal) { *a_params.retVal = ret; }
			}
		};
		SetSkillsHandler* g_handler = nullptr;

		// Combined run 5c7f (2026-10-27): the wrap alone never ran. Character Menu SE's DLL fills the sheet from C++
		// (_root.CharacterSheet_mc.SetSkills, called by its CharacterSheet::SetSkills) while the menu is being shown -
		// before the MenuOpenCloseEvent this file wraps on. So the sheet is also corrected after the fact: the skill rows
		// the class does not hold are removed and the rest closed up (45 px apart, as SetSkills lays them out), and the
		// class line on the CHARACTER page is rewritten. The wrap stays for any later SetSkills call.
		void Repair(RE::GFxMovieView* a_movie)
		{
			const auto snap = Classes::GetSnapshot();
			if (!a_movie || !snap.enabled || !snap.chosen) { return; }
			RE::GFxValue sheet;
			if (!a_movie->GetVariable(&sheet, "_root.CharacterSheet_mc") || !sheet.IsObject()) { return; }
			std::vector<std::string> hidden;
			for (int i = 0; i < skilllist::kCount; ++i) { if (!Classes::Holds(i)) { hidden.push_back(SkillName(i)); } }
			std::uint32_t removed = 0, kept = 0;
			RE::GFxValue clips;
			if (sheet.GetMember("aSkillClipsContainer", &clips) && clips.IsArray() && clips.GetArraySize() > 0)
			{
				std::vector<RE::GFxValue> keep;
				for (std::uint32_t i = 0; i < clips.GetArraySize(); ++i)
				{
					RE::GFxValue clip, name;
					if (!clips.GetElement(i, &clip) || !clip.IsDisplayObject()) { continue; }
					const bool hide = clip.GetMember("name", &name) && name.IsString() &&
					                  std::find(hidden.begin(), hidden.end(), std::string(name.GetString())) != hidden.end();
					if (hide) { clip.Invoke("removeMovieClip"); ++removed; } else { keep.push_back(clip); }
				}
				if (removed > 0)
				{
					clips.ClearElements();
					for (std::size_t j = 0; j < keep.size(); ++j)
					{
						RE::GFxValue::DisplayInfo info;
						keep[j].GetDisplayInfo(&info);
						info.SetY(45.0 * static_cast<double>(j));
						keep[j].SetDisplayInfo(info);
						keep[j].SetMember("index", RE::GFxValue(static_cast<double>(j)));
						clips.PushBack(keep[j]);
					}
					sheet.SetMember("skillsListHeight", RE::GFxValue(static_cast<double>((keep.size() + 1) * 45 - 5)));
				}
				kept = static_cast<std::uint32_t>(keep.size());
			}
			RE::GFxValue attr;
			bool classSet = false;
			if (a_movie->GetVariable(&attr, "_root.CharacterSheet_mc.InfoHolder.attrClass") && attr.IsDisplayObject())
			{
				RE::GFxValue field;
				if (attr.GetMember("info", &field) && field.IsDisplayObject()) { classSet = field.SetMember("text", RE::GFxValue(snap.className.c_str())); }
				attr.SetMember("descTitle", RE::GFxValue(snap.className.c_str()));
				if (const auto* n = ClassData::Find(snap.classId)) { attr.SetMember("description", RE::GFxValue(n->description.c_str())); }
			}
			logger::info("character sheet: after the sheet filled itself - {} skill row(s) the class does not hold removed, {} kept; class line {} {}",
				removed, kept, classSet ? "set to" : "NOT found for", snap.className);
		}

		void Wrap(RE::GFxMovieView* a_movie)
		{
			RE::GFxValue proto;
			if (!a_movie || !a_movie->GetVariable(&proto, "_global.CharacterSheet.prototype") || !proto.IsObject()) { return; }
			RE::GFxValue mark;
			if (proto.GetMember("njSheetWrapped", &mark) && mark.IsBool()) { return; }
			RE::GFxValue orig;
			if (!proto.GetMember("SetSkills", &orig) || orig.IsUndefined()) { return; }
			if (!g_handler) { g_handler = new SetSkillsHandler(); }
			RE::GFxValue fn;
			a_movie->CreateFunction(&fn, g_handler);
			proto.SetMember("njOrigSetSkills", orig);
			proto.SetMember("SetSkills", fn);
			proto.SetMember("njSheetWrapped", RE::GFxValue(true));
			logger::info("character sheet: Character Menu SE's SetSkills wrapped (skills and class follow the class)");
		}

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_e || !a_e->opening) { return RE::BSEventNotifyControl::kContinue; }
				const std::string menu = a_e->menuName.c_str();
				if (auto* tasks = SKSE::GetTaskInterface())
				{
					tasks->AddUITask([menu]() {
						auto* ui = RE::UI::GetSingleton();
						auto m = ui ? ui->GetMenu(menu) : nullptr;
						if (m && m->uiMovie) { Wrap(m->uiMovie.get()); Repair(m->uiMovie.get()); }
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
	}
}
