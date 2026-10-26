#include "PCH.h"

#include "Gear.h"

#include "Settings.h"

#include "utils/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>

namespace Gear
{
	namespace
	{
		constexpr const char* kPlugin = "NjordlingerCombatGameplayOverhaul.esp";
		constexpr RE::FormID kMagicDisallowEnchanting = 0x000C27BD;   // Skyrim.esm KYWD, verified in the master 2026-10-26

		struct SetDef
		{
			std::string name;
			RE::BGSKeyword* keyword = nullptr;
			RE::SpellItem* ability2 = nullptr;
			RE::SpellItem* ability4 = nullptr;
			RE::BGSPerk* perk = nullptr;
		};

		struct UniqueDef
		{
			std::string id;
			std::string name;
			RE::BGSKeyword* keyword = nullptr;
			RE::BGSPerk* perk = nullptr;
			RE::SpellItem* ability = nullptr;
		};

		// An authored magnitude and where it lives, so a strength is always applied to the plugin's value, never compounded.
		struct ScaledEffect { RE::Effect* effect; float base; };
		struct ScaledEntry { RE::BGSEntryPointFunctionDataOneValue* data; float base; bool multiply; };

		// ---- main-thread state ----
		std::vector<SetDef> g_sets;
		std::vector<UniqueDef> g_uniques;
		RE::TESGlobal* g_setsOn = nullptr;
		RE::TESGlobal* g_uniquesOn = nullptr;
		RE::BGSKeyword* g_tierUnique = nullptr;
		RE::BGSKeyword* g_noEnchant = nullptr;
		std::vector<ScaledEffect> g_setEffects, g_uniqueEffects;
		std::vector<ScaledEntry> g_setEntries, g_uniqueEntries;
		float g_appliedSetStrength = -1.0F;      // -1: never applied (the records hold the authored values)
		float g_appliedUniqueStrength = -1.0F;
		bool g_noEnchantDone = false;

		// ---- shared ----
		std::mutex g_lock;                       // g_snapshot and g_dirty
		Snapshot g_snapshot;
		std::unordered_set<RE::FormID> g_dirty;  // actors whose pieces must be recounted (reference form IDs)
		std::atomic<bool> g_running{ false };

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

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
			if (!a_obj.is_object() || !a_obj.contains(a_key)) { return nullptr; }
			const auto local = ParseLocalId(a_obj[a_key]);
			if (!local) { return nullptr; }
			auto* handler = RE::TESDataHandler::GetSingleton();
			T* form = handler ? handler->LookupForm<T>(local, kPlugin) : nullptr;
			if (!form) { logger::warn("gear: {} {} 0x{:X} not found in {} - that part is skipped", a_owner, a_key, local, kPlugin); }
			else { logger::debug("gear: {} {} -> {:08X}", a_owner, a_key, form->GetFormID()); }
			return form;
		}

		void RecordSpell(RE::SpellItem* a_spell, std::vector<ScaledEffect>& a_out)
		{
			if (!a_spell) { return; }
			for (auto* e : a_spell->effects)
			{
				if (e) { a_out.push_back({ e, e->effectItem.magnitude }); }
			}
		}

		void RecordPerk(RE::BGSPerk* a_perk, std::vector<ScaledEntry>& a_out)
		{
			if (!a_perk) { return; }
			for (auto* entry : a_perk->perkEntries)
			{
				if (!entry || entry->GetType() != RE::PERK_ENTRY_TYPE::kEntryPoint) { continue; }
				auto* ep = static_cast<RE::BGSEntryPointPerkEntry*>(entry);
				auto* data = ep->functionData;
				if (!data || data->GetType() != RE::BGSEntryPointFunctionData::ENTRY_POINT_FUNCTION_DATA::kOneValue) { continue; }
				// the function enum lives in a different type on each build line; its values are the record's (2 add, 3 multiply)
				const auto fn = static_cast<int>(ep->entryData.function.get());
				const bool multiply = fn == 3;
				const bool add = fn == 2;
				if (!multiply && !add) { continue; }
				auto* one = static_cast<RE::BGSEntryPointFunctionDataOneValue*>(data);
				a_out.push_back({ one, one->data, multiply });
			}
		}

		void Scale(std::vector<ScaledEffect>& a_effects, std::vector<ScaledEntry>& a_entries, float a_strength)
		{
			for (auto& s : a_effects) { s.effect->effectItem.magnitude = s.base * a_strength; }
			for (auto& s : a_entries)
			{
				// a multiplier scales its distance from 1 (x1.25 at strength 2 -> x1.5; x0.8 -> x0.6), never below 0
				s.data->data = s.multiply ? std::max(0.0F, 1.0F + (s.base - 1.0F) * a_strength) : s.base * a_strength;
			}
		}

		// The armour an actor wears, each piece once (a cuirass covers several slots).
		std::vector<RE::TESObjectARMO*> WornArmor(RE::Actor* a_actor)
		{
			std::vector<RE::TESObjectARMO*> out;
			if (!a_actor) { return out; }
			for (std::uint32_t bit = 0; bit < 32; ++bit)
			{
				auto* a = a_actor->GetWornArmor(static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << bit), true);
				if (a && std::find(out.begin(), out.end(), a) == out.end()) { out.push_back(a); }
			}
			return out;
		}

		void SetSpell(RE::Actor* a_actor, RE::SpellItem* a_spell, bool a_want, Snapshot& a_counts)
		{
			if (!a_actor || !a_spell) { return; }
			const bool has = a_actor->HasSpell(a_spell);
			if (a_want && !has)
			{
				a_actor->AddSpell(a_spell);
				++a_counts.abilitiesAdded;
				logger::debug("gear: {:08X} gains {:08X} ({})", a_actor->GetFormID(), a_spell->GetFormID(), a_spell->GetName());
			}
			else if (!a_want && has)
			{
				a_actor->RemoveSpell(a_spell);
				++a_counts.abilitiesRemoved;
				logger::debug("gear: {:08X} loses {:08X} ({})", a_actor->GetFormID(), a_spell->GetFormID(), a_spell->GetName());
			}
		}

		// Counts the actor's worn pieces of every set and keeps the 2- and 4-piece abilities in step (main thread).
		void Recount(RE::Actor* a_actor)
		{
			if (!a_actor || g_sets.empty()) { return; }
			const auto worn = WornArmor(a_actor);
			const bool on = settings::gear::sets;
			const bool isPlayer = a_actor == Player();
			Snapshot counts;
			std::vector<SetState> states;
			for (const auto& s : g_sets)
			{
				int pieces = 0;
				if (s.keyword)
				{
					for (auto* a : worn)
					{
						if (a->HasKeyword(s.keyword)) { ++pieces; }
					}
				}
				SetSpell(a_actor, s.ability2, on && pieces >= 2, counts);
				SetSpell(a_actor, s.ability4, on && pieces >= 4, counts);
				if (isPlayer)
				{
					SetState st;
					st.name = s.name;
					st.pieces = pieces;
					st.has2 = s.ability2 && a_actor->HasSpell(s.ability2);
					st.has4 = s.ability4 && a_actor->HasSpell(s.ability4);
					st.resolved = s.keyword && s.ability2 && s.ability4;
					states.push_back(std::move(st));
				}
			}
			std::vector<UniqueState> uniques;
			if (isPlayer)
			{
				std::vector<RE::TESForm*> equipped(worn.begin(), worn.end());
				for (bool left : { false, true })
				{
					if (auto* f = a_actor->GetEquippedObject(left)) { equipped.push_back(f); }
				}
				for (const auto& u : g_uniques)
				{
					UniqueState us;
					us.id = u.id;
					us.name = u.name;
					us.resolved = u.keyword && u.perk && u.ability;
					for (auto* f : equipped)
					{
						auto* kf = f ? f->As<RE::BGSKeywordForm>() : nullptr;
						if (kf && u.keyword && kf->HasKeyword(u.keyword)) { us.worn = true; break; }
					}
					uniques.push_back(std::move(us));
				}
			}
			std::scoped_lock l(g_lock);
			++g_snapshot.recounts;
			g_snapshot.abilitiesAdded += counts.abilitiesAdded;
			g_snapshot.abilitiesRemoved += counts.abilitiesRemoved;
			if (isPlayer)
			{
				// log the player's set picture when it changes (an equip can fire several events for one change)
				std::string picture;
				for (const auto& st : states) { picture += st.name + "=" + std::to_string(st.pieces) + (st.has2 ? "+2" : "") + (st.has4 ? "+4" : "") + " "; }
				static std::string lastPicture;
				if (picture != lastPicture)
				{
					logger::info("gear: player sets {}", picture);
					lastPicture = picture;
				}
				g_snapshot.sets = std::move(states);
				g_snapshot.uniques = std::move(uniques);
			}
		}

		void RecountDirty()
		{
			std::vector<RE::FormID> ids;
			{
				std::scoped_lock l(g_lock);
				ids.assign(g_dirty.begin(), g_dirty.end());
				g_dirty.clear();
			}
			for (const auto id : ids)
			{
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!actor) { continue; }   // unloaded or gone since the event: its next load recounts it
				Recount(actor);
			}
		}

		void MarkDirty(RE::FormID a_ref)
		{
			if (!a_ref) { return; }
			std::scoped_lock l(g_lock);
			g_dirty.insert(a_ref);
		}

		class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_e, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (a_e && a_e->actor) { MarkDirty(a_e->actor->GetFormID()); }
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		EquipSink g_equip;

		class LoadedSink : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_e, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				if (a_e && a_e->loaded) { MarkDirty(a_e->formID); }   // the recount ignores anything that is not an actor
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		LoadedSink g_loaded;

		// The pacer: a quarter-second after an equip (several events arrive for one change), one main-thread task.
		void Worker()
		{
			while (g_running.load())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				bool any = false;
				{
					std::scoped_lock l(g_lock);
					any = !g_dirty.empty();
				}
				if (!any) { continue; }
				if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { RecountDirty(); }); }
			}
		}

		// D15: the unique tier cannot be enchanted. Adds NJCGO_NoEnchant and MagicDisallowEnchanting wherever KID left one
		// out (main thread, once per session - the forms are the same for every save).
		template <class T>
		void NoEnchantPass(RE::BGSKeyword* a_disallow, std::uint32_t& a_items, std::uint32_t& a_noEnchant, std::uint32_t& a_disallowAdded)
		{
			auto* handler = RE::TESDataHandler::GetSingleton();
			if (!handler) { return; }
			for (auto* f : handler->GetFormArray<T>())
			{
				if (!f) { continue; }
				const bool unique = (g_tierUnique && f->HasKeyword(g_tierUnique)) || (g_noEnchant && f->HasKeyword(g_noEnchant));
				if (!unique) { continue; }
				++a_items;
				if (g_noEnchant && !f->HasKeyword(g_noEnchant) && f->AddKeyword(g_noEnchant)) { ++a_noEnchant; }
				if (a_disallow && !f->HasKeyword(a_disallow) && f->AddKeyword(a_disallow)) { ++a_disallowAdded; }
			}
		}

		void RunNoEnchantPass()
		{
			if (g_noEnchantDone) { return; }
			g_noEnchantDone = true;
			auto* disallow = RE::TESForm::LookupByID<RE::BGSKeyword>(kMagicDisallowEnchanting);
			if (!disallow) { logger::warn("gear: MagicDisallowEnchanting (Skyrim.esm 0C27BD) not found - uniques keep only NJCGO_NoEnchant"); }
			if (!g_tierUnique) { logger::warn("gear: NJCGO_Tier_Unique is not in the plugin - the no-enchant pass relies on NJCGO_NoEnchant alone"); }
			std::uint32_t items = 0, noEnchant = 0, disallowAdded = 0;
			NoEnchantPass<RE::TESObjectWEAP>(disallow, items, noEnchant, disallowAdded);
			NoEnchantPass<RE::TESObjectARMO>(disallow, items, noEnchant, disallowAdded);
			logger::info("gear: no-enchant pass - {} unique-tier weapons and armour; added NJCGO_NoEnchant to {}, MagicDisallowEnchanting to {} (KID had placed the rest)", items, noEnchant, disallowAdded);
			if (items == 0) { logger::warn("gear: no item carries NJCGO_Tier_Unique - is NjordlingerCGO_Tiers_KID.ini installed and Keyword Item Distributor loaded?"); }
			std::scoped_lock l(g_lock);
			g_snapshot.uniqueTierItems = items;
			g_snapshot.noEnchantAdded = noEnchant;
			g_snapshot.disallowAdded = disallowAdded;
			g_snapshot.noEnchantPassDone = true;
		}

		// Re-applies a spell the player holds, so its effects take the magnitudes now in the record.
		void Reapply(RE::PlayerCharacter* a_player, RE::SpellItem* a_spell)
		{
			if (!a_player || !a_spell || !a_player->HasSpell(a_spell)) { return; }
			a_player->RemoveSpell(a_spell);
			a_player->AddSpell(a_spell);
			logger::debug("gear: re-applied {:08X} ({}) for the new strength", a_spell->GetFormID(), a_spell->GetName());
		}

		void Apply()
		{
			if (g_setsOn) { g_setsOn->value = settings::gear::sets ? 1.0F : 0.0F; }
			if (g_uniquesOn) { g_uniquesOn->value = settings::gear::uniques ? 1.0F : 0.0F; }
			auto* player = Player();
			const float ss = settings::gear::setStrength;
			const float us = settings::gear::uniqueStrength;
			if (ss != g_appliedSetStrength)
			{
				Scale(g_setEffects, g_setEntries, ss);
				const bool first = g_appliedSetStrength < 0.0F;
				g_appliedSetStrength = ss;
				if (!first)
				{
					for (const auto& s : g_sets) { Reapply(player, s.ability2); Reapply(player, s.ability4); }
				}
			}
			if (us != g_appliedUniqueStrength)
			{
				Scale(g_uniqueEffects, g_uniqueEntries, us);
				const bool first = g_appliedUniqueStrength < 0.0F;
				g_appliedUniqueStrength = us;
				if (!first)
				{
					for (const auto& u : g_uniques) { Reapply(player, u.ability); }
				}
			}
			logger::info("gear: sets {} (strength {:.2f}), uniques {} (strength {:.2f})", settings::gear::sets, ss, settings::gear::uniques, us);
			{
				std::scoped_lock l(g_lock);
				g_snapshot.appliedSetStrength = ss;
				g_snapshot.appliedUniqueStrength = us;
			}
			if (player) { Recount(player); }   // sets switched off -> the abilities come off; on -> back
		}
	}

	void Install()
	{
		const auto path = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "NjordlingerCombatGameplayOverhaul" / "gear.json";
		auto fail = [](std::string a_why) {
			logger::error("gear: {} - the Gear module is off (the set and unique records still apply through SPID, unscaled, without piece counting)", a_why);
			std::scoped_lock l(g_lock);
			g_snapshot.error = std::move(a_why);
		};
		std::ifstream in(path);
		if (!in) { fail("gear.json was not found at " + path.string()); return; }
		nlohmann::json j;
		try { j = nlohmann::json::parse(in, nullptr, true, true); }
		catch (const std::exception& e) { fail(std::string("gear.json does not parse: ") + e.what()); return; }

		try
		{
			g_setsOn = Resolve<RE::TESGlobal>(j.value("globals", nlohmann::json::object()), "setsOn", "globals");
			g_uniquesOn = Resolve<RE::TESGlobal>(j.value("globals", nlohmann::json::object()), "uniquesOn", "globals");
			g_tierUnique = Resolve<RE::BGSKeyword>(j.value("keywords", nlohmann::json::object()), "tierUnique", "keywords");
			g_noEnchant = Resolve<RE::BGSKeyword>(j.value("keywords", nlohmann::json::object()), "noEnchant", "keywords");
			for (const auto& s : j.value("sets", nlohmann::json::array()))
			{
				SetDef d;
				d.name = s.value("name", std::string("?"));
				d.keyword = Resolve<RE::BGSKeyword>(s, "keyword", d.name);
				d.ability2 = Resolve<RE::SpellItem>(s, "ability2", d.name);
				d.ability4 = Resolve<RE::SpellItem>(s, "ability4", d.name);
				d.perk = Resolve<RE::BGSPerk>(s, "perk", d.name);
				RecordSpell(d.ability2, g_setEffects);
				RecordSpell(d.ability4, g_setEffects);
				RecordPerk(d.perk, g_setEntries);
				g_sets.push_back(std::move(d));
			}
			for (const auto& u : j.value("uniques", nlohmann::json::array()))
			{
				UniqueDef d;
				d.id = u.value("id", std::string("?"));
				d.name = u.value("name", d.id);
				d.keyword = Resolve<RE::BGSKeyword>(u, "keyword", d.id);
				d.perk = Resolve<RE::BGSPerk>(u, "perk", d.id);
				d.ability = Resolve<RE::SpellItem>(u, "ability", d.id);
				RecordSpell(d.ability, g_uniqueEffects);
				RecordPerk(d.perk, g_uniqueEntries);
				g_uniques.push_back(std::move(d));
			}
		}
		catch (const std::exception& e) { fail(std::string("gear.json is not in the expected shape: ") + e.what()); return; }

		auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
		if (!holder) { fail("no event source holder - piece counting cannot run"); return; }
		holder->AddEventSink<RE::TESEquipEvent>(&g_equip);
		holder->AddEventSink<RE::TESObjectLoadedEvent>(&g_loaded);
		g_running = true;
		std::thread(Worker).detach();
		{
			std::scoped_lock l(g_lock);
			g_snapshot.loaded = true;
			for (const auto& s : g_sets) { g_snapshot.sets.push_back({ s.name, 0, false, false, s.keyword && s.ability2 && s.ability4 }); }
			for (const auto& u : g_uniques) { g_snapshot.uniques.push_back({ u.id, u.name, false, u.keyword && u.perk && u.ability }); }
		}
		logger::info("gear: {} sets, {} uniques; {} set and {} unique effects, {} set and {} unique entry points can be scaled; equip and load sinks registered",
					 g_sets.size(), g_uniques.size(), g_setEffects.size(), g_uniqueEffects.size(), g_setEntries.size(), g_uniqueEntries.size());
	}

	void OnGameLoaded()
	{
		RunNoEnchantPass();
		Apply();
	}

	void RequestApply()
	{
		if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { Apply(); }); }
	}

	void RequestRecountPlayer()
	{
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([]() { if (auto* p = Player()) { Recount(p); } });
		}
	}

	Snapshot GetSnapshot()
	{
		std::scoped_lock l(g_lock);
		return g_snapshot;
	}

	std::string StatusJson()
	{
		const auto s = GetSnapshot();
		nlohmann::json j;
		j["loaded"] = s.loaded;
		j["error"] = s.error;
		j["settings"] = { { "sets", settings::gear::sets }, { "uniques", settings::gear::uniques }, { "setStrength", settings::gear::setStrength }, { "uniqueStrength", settings::gear::uniqueStrength } };
		j["applied"] = { { "setStrength", s.appliedSetStrength }, { "uniqueStrength", s.appliedUniqueStrength } };
		auto& sets = j["sets"] = nlohmann::json::array();
		for (const auto& st : s.sets) { sets.push_back({ { "name", st.name }, { "pieces", st.pieces }, { "has2", st.has2 }, { "has4", st.has4 }, { "resolved", st.resolved } }); }
		auto& uniques = j["uniquesWorn"] = nlohmann::json::array();
		std::uint32_t resolved = 0;
		for (const auto& u : s.uniques)
		{
			if (u.resolved) { ++resolved; }
			if (u.worn) { uniques.push_back(u.id); }
		}
		j["uniquesResolved"] = resolved;
		j["uniquesDefined"] = s.uniques.size();
		j["recounts"] = s.recounts;
		j["abilitiesAdded"] = s.abilitiesAdded;
		j["abilitiesRemoved"] = s.abilitiesRemoved;
		j["noEnchant"] = { { "done", s.noEnchantPassDone }, { "uniqueTierItems", s.uniqueTierItems }, { "noEnchantAdded", s.noEnchantAdded }, { "disallowAdded", s.disallowAdded } };
		return j.dump();
	}
}
