#include "PCH.h"

#include "SoulStones.h"

#include "Settings.h"

#include "utils/Logger.h"
#include "utils/Strings.h"

#include <cstdarg>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace SoulStones
{
	namespace
	{
		constexpr const char* kPlugin = "NjordlingerCombatGameplayOverhaul.esp";
		constexpr std::uint32_t kRecordVersion = 1;
		constexpr int kTiers = 5;
		constexpr std::uint32_t kWordLearned = 1u << 6;    // TESWordOfPower: learned at a wall (RecordFlags::kKnown)
		// TESWordOfPower: unlocked with a dragon soul. NOT bit 0 (CommonLib's generic kUnlocked): combined run dcf7 read 0x9
		// on all three words of a character who knows no Shout. Bit 16 is this build's reading, to be proven with
		// DevBench op=stoneswords before and after the console's unlockword (rule 30).
		constexpr std::uint32_t kWordUnlocked = 1u << 16;
		constexpr std::uint16_t kNoOriginalCharge = 3000;  // a weapon whose only enchantment is its stones: cost 0, this charge

		using Clock = std::chrono::steady_clock;

		// ---- the table read from stones.json (main thread after Install; read-only afterwards) --------------------------
		struct Proc
		{
			enum class On { kHit, kHitBy, kKill, kCooldown } on = On::kHit;
			bool power = false, sneak = false, blocked = false, lowHealth = false, storm = false;
			RE::BGSKeyword* targetKeyword = nullptr;
			int chance = 0;
			int every = 0;
			int stack = 0;               // each qualifying hit within the window adds one stack, up to this many
			float window = 3.0F;
			float damage = 0.0F;
			std::string at = "target";
			float cooldown = 0.0F;
			RE::SpellItem* spell = nullptr;
			std::array<float, kTiers> mag{};
			bool hasMag = false;
			std::array<float, kTiers> mult{};
		};

		struct Cell
		{
			std::string kind;                     // rec, perk, self, proc, grant, stub
			RE::EffectSetting* effect = nullptr;  // the enchantment line (acts for rec, a card line otherwise)
			std::array<float, kTiers> m{}, dur{}, value{};
			RE::BGSPerk* perk = nullptr;
			RE::SpellItem* ability = nullptr;
			bool hasProc = false;
			Proc proc;
			std::string text;
		};

		struct Word
		{
			std::string name;
			int phrase = -1;
			int position = 0;
			std::array<RE::TESBoundObject*, kTiers> stones{};
			std::array<Cell, 3> hosts;
		};

		struct Phrase
		{
			std::string shout;
			std::array<int, 3> words{ -1, -1, -1 };
			std::array<RE::TESWordOfPower*, 3> real{};
			std::array<Cell, 3> hosts;
			RE::TESShout* clone = nullptr;
			std::array<RE::TESWordOfPower*, 3> cloneWords{};
		};

		struct Tier
		{
			std::string name;
			RE::EffectSetting* glow = nullptr;
			std::vector<RE::TESBoundObject*> feeGems;
		};

		struct Table
		{
			bool loaded = false;
			std::string error;
			std::vector<Word> words;
			std::vector<Phrase> phrases;
			std::array<Tier, kTiers> tiers;
			std::vector<std::string> wordNames, tierNames;
			std::vector<std::pair<RE::BGSKeyword*, int>> itemTierSlots;   // item tier keyword -> slots
			RE::EffectSetting* unknownWeapon = nullptr;
			RE::EffectSetting* unknownArmour = nullptr;
			std::unordered_map<RE::FormID, std::pair<int, int>> stoneForm;  // stone MISC -> (word, tier)
			std::unordered_map<RE::EffectSetting*, std::pair<int, int>> effectCell;   // word effect -> (word, host)
			std::unordered_map<RE::EffectSetting*, std::pair<int, int>> phraseCell;   // phrase effect -> (phrase, host)
			std::unordered_map<RE::EffectSetting*, int> glowTier;
			std::unordered_map<std::string, RE::SpellItem*> helpers;
			float lowHealth = 0.30F;
		};
		Table g_t;

		// ---- the per-instance table (main thread; the co-save) ----------------------------------------------------------
		struct SavedEffect
		{
			RE::FormID effect = 0;
			float magnitude = 0.0F;
			std::uint32_t area = 0;
			std::uint32_t duration = 0;
		};

		struct Entry
		{
			RE::FormID base = 0;
			RE::FormID owner = 0;
			std::uint16_t uid = 0;
			std::array<StoneRef, 3> stones{};
			bool hadEnchant = false;              // the instance carried an enchantment of its own before the first stone
			std::vector<SavedEffect> orig;
			std::uint16_t origCharge = 0;
			std::int32_t origCost = -1;           // the original's cost per use (-1: none - our composition costs nothing)
			bool hadName = false;                 // the instance had a custom name before (an enchanter's, say)
			std::string origName;
		};
		std::unordered_map<std::string, Entry> g_entries;
		std::string g_heldRecord;   // the co-save text kept as it was while the table is not loaded (never lose a save's stones)

		struct Equipped
		{
			Host host = Host::kNone;
			RE::FormID base = 0;
			bool left = false;
			std::array<StoneRef, 3> stones{};
			int phrase = -1;                      // a known phrase spelled in slots 1-3, or -1
		};
		std::vector<Equipped> g_equipped;
		std::unordered_map<std::string, Clock::time_point> g_cooldowns;
		std::unordered_map<std::string, std::pair<int, Clock::time_point>> g_streaks;
		std::uint64_t g_knowledgeHash = 0;
		std::uint32_t g_composes = 0;

		// ---- shared ----------------------------------------------------------------------------------------------------
		std::mutex g_lock;
		Snapshot g_snapshot;
		std::atomic<bool> g_service{ false };
		std::atomic<bool> g_dirty{ false };
		std::atomic<bool> g_knowledgeDirty{ false };
		std::atomic<bool> g_running{ false };
		std::atomic<std::uint32_t> g_procsFired{ 0 };

		// ---- the runesmith (runesmith.json; PLAN.md phase 4 step 3, D22) ------------------------------------------------
		struct Runesmith
		{
			bool loaded = false;
			RE::FormID npcRef = 0;
			RE::FormID socketInfo = 0;
			RE::TESGlobal* learned = nullptr;
			float radius = 1500.0F;
			RE::FormID homeCell = 0;
			RE::NiPoint3 homePos{};
			float homeAngleZ = 0.0F;
		};
		Runesmith g_rs;
		std::function<void(bool)> g_windowOpener;   // the settings page's runesmith window (UI registers it)
		std::atomic<bool> g_checkDistance{ false };

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

		void SetResult(std::string a_text)
		{
			logger::info("soul stones: {}", a_text);
			std::scoped_lock l(g_lock);
			g_snapshot.lastResult = std::move(a_text);
		}

		// A player-facing line in the active language (printf-style, as the translation files carry it).
		std::string Say(const char* a_key, const char* a_english, ...)
		{
			const std::string fmtText = strings::TR(a_key, a_english);
			char buf[512];
			va_list args;
			va_start(args, a_english);
			std::vsnprintf(buf, sizeof(buf), fmtText.c_str(), args);
			va_end(args);
			return buf;
		}

		std::string TierName(int a_tier)
		{
			static const char* keys[] = { "NJ_SS_Tier0", "NJ_SS_Tier1", "NJ_SS_Tier2", "NJ_SS_Tier3", "NJ_SS_Tier4" };
			static const char* en[] = { "Petty", "Lesser", "Common", "Greater", "Grand" };
			return (a_tier >= 0 && a_tier < kTiers) ? std::string(strings::TR(keys[a_tier], en[a_tier])) : std::string();
		}

		// "0x812" = our plugin, "Skyrim.esm|0x1234" = another; nullptr (logged) when it does not resolve.
		template <class T>
		T* Form(const nlohmann::json& a_v, const char* a_what)
		{
			if (!a_v.is_string()) { return nullptr; }
			const auto text = a_v.get<std::string>();
			std::string plugin = kPlugin;
			std::string id = text;
			if (const auto bar = text.find('|'); bar != std::string::npos)
			{
				plugin = text.substr(0, bar);
				id = text.substr(bar + 1);
			}
			std::uint32_t local = 0;
			try { local = static_cast<std::uint32_t>(std::stoul(id, nullptr, 0)); } catch (...) { return nullptr; }
			auto* handler = RE::TESDataHandler::GetSingleton();
			auto* any = handler ? handler->LookupForm(local, plugin) : nullptr;
			// combined run dcf7: LookupForm<TESBoundObject> returned null for every stone and soul gem that exists -
			// TESBoundObject is a base class with no form type of its own, so the typed lookup's As<> refuses it
			T* form = nullptr;
			if constexpr (std::is_same_v<T, RE::TESBoundObject>) { form = (any && any->IsBoundObject()) ? static_cast<RE::TESBoundObject*>(any) : nullptr; }
			else if constexpr (std::is_same_v<T, RE::TESForm>) { form = any; }
			else { form = any ? any->As<T>() : nullptr; }
			if (!form) { logger::warn("soul stones: {} {} does not resolve{} - that part is skipped", a_what, text, any ? fmt::format(" as the expected type (it is form type {})", static_cast<int>(any->GetFormType())) : std::string()); }
			return form;
		}

		std::array<float, kTiers> Floats(const nlohmann::json& a_v)
		{
			std::array<float, kTiers> out{};
			if (a_v.is_array())
			{
				for (std::size_t i = 0; i < out.size() && i < a_v.size(); ++i) { out[i] = a_v[i].get<float>(); }
			}
			else if (a_v.is_number()) { out.fill(a_v.get<float>()); }
			return out;
		}

		Cell ReadCell(const nlohmann::json& a_c)
		{
			Cell c;
			c.kind = a_c.value("kind", std::string("stub"));
			c.effect = Form<RE::EffectSetting>(a_c.value("effect", nlohmann::json()), "effect");
			c.text = a_c.value("text", std::string());
			if (a_c.contains("m")) { c.m = Floats(a_c["m"]); }
			if (a_c.contains("dur")) { c.dur = Floats(a_c["dur"]); }
			if (a_c.contains("value")) { c.value = Floats(a_c["value"]); }
			if (a_c.contains("perk")) { c.perk = Form<RE::BGSPerk>(a_c["perk"], "perk"); }
			if (a_c.contains("ability")) { c.ability = Form<RE::SpellItem>(a_c["ability"], "ability"); }
			if (a_c.contains("proc") && a_c["proc"].is_object())
			{
				const auto& p = a_c["proc"];
				c.hasProc = true;
				const auto on = p.value("on", std::string("hit"));
				c.proc.on = on == "hitBy" ? Proc::On::kHitBy : on == "kill" ? Proc::On::kKill : on == "cooldown" ? Proc::On::kCooldown : Proc::On::kHit;
				c.proc.power = p.value("power", false);
				c.proc.sneak = p.value("sneak", false);
				c.proc.blocked = p.value("blocked", false);
				c.proc.lowHealth = p.value("lowHealth", false);
				c.proc.storm = p.value("storm", false);
				c.proc.chance = p.value("chance", 0);
				c.proc.every = p.value("every", 0);
				c.proc.stack = p.value("stack", 0);
				c.proc.window = p.value("window", 3.0F);
				c.proc.damage = p.value("damage", 0.0F);
				c.proc.at = p.value("at", std::string("target"));
				c.proc.cooldown = p.value("cd", 0.0F);
				if (p.contains("targetKeyword")) { c.proc.targetKeyword = Form<RE::BGSKeyword>(p["targetKeyword"], "proc keyword"); }
				if (p.contains("spell")) { c.proc.spell = Form<RE::SpellItem>(p["spell"], "proc spell"); }
				if (p.contains("mag")) { c.proc.mag = Floats(p["mag"]); c.proc.hasMag = true; }
				if (p.contains("mult")) { c.proc.mult = Floats(p["mult"]); }
			}
			return c;
		}

		// ---- item facts --------------------------------------------------------------------------------------------------
		Host HostOf(RE::TESBoundObject* a_obj)
		{
			if (!a_obj) { return Host::kNone; }
			if (auto* w = a_obj->As<RE::TESObjectWEAP>())
			{
				if (w->IsStaff()) { return Host::kNone; }   // D18: staves take no stones
				if (w->IsBow() || w->IsCrossbow()) { return Host::kRanged; }
				return Host::kMelee;
			}
			if (a_obj->Is(RE::FormType::Armor)) { return Host::kArmour; }
			return Host::kNone;
		}

		int SlotsOf(RE::TESBoundObject* a_obj)
		{
			if (HostOf(a_obj) == Host::kNone) { return 0; }
			auto* kf = a_obj->As<RE::BGSKeywordForm>();
			if (!kf) { return 0; }
			int best = 0;
			for (const auto& [kw, slots] : g_t.itemTierSlots)
			{
				if (kw && kf->HasKeyword(kw)) { best = std::max(best, slots); }
			}
			return std::min(best, 3);
		}

		std::string KeyOf(RE::FormID a_base, RE::FormID a_owner, std::uint16_t a_uid)
		{
			return fmt::format("{:08X}:{:08X}:{:04X}", a_base, a_owner, a_uid);
		}

		bool KeyOf(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_x, std::string& a_out)
		{
			auto* u = a_x ? a_x->GetByType<RE::ExtraUniqueID>() : nullptr;
			if (!a_obj || !u) { return false; }
			a_out = KeyOf(a_obj->GetFormID(), u->baseID, u->uniqueID);
			return true;
		}

		bool Worn(RE::ExtraDataList* a_x, bool* a_left = nullptr)
		{
			if (!a_x) { return false; }
			const bool right = a_x->HasType(RE::ExtraDataType::kWorn);
			const bool left = a_x->HasType(RE::ExtraDataType::kWornLeft);
			if (a_left) { *a_left = left && !right; }
			return right || left;
		}

		// Every (entry, extra list) of the player's weapons and armour; a_fn(entry, xList-or-nullptr).
		template <class F>
		void ForEachInstance(F&& a_fn)
		{
			auto* p = Player();
			auto* changes = p ? p->GetInventoryChanges() : nullptr;
			if (!changes || !changes->entryList) { return; }
			for (auto* entry : *changes->entryList)
			{
				if (!entry || !entry->object || HostOf(entry->object) == Host::kNone) { continue; }
				if (entry->extraLists)
				{
					for (auto* x : *entry->extraLists)
					{
						if (x) { a_fn(entry, x); }
					}
				}
			}
		}

		bool FindInstance(const std::string& a_key, RE::InventoryEntryData*& a_entry, RE::ExtraDataList*& a_x)
		{
			a_entry = nullptr;
			a_x = nullptr;
			ForEachInstance([&](RE::InventoryEntryData* e, RE::ExtraDataList* x) {
				std::string k;
				if (!a_x && KeyOf(e->object, x, k) && k == a_key)
				{
					a_entry = e;
					a_x = x;
				}
			});
			return a_x != nullptr;
		}

		// ---- word knowledge (D20 / D21) ------------------------------------------------------------------------------
		bool Learned(RE::TESWordOfPower* a_w) { return a_w && (a_w->formFlags & kWordLearned) != 0; }
		bool Unlocked(RE::TESWordOfPower* a_w) { return a_w && (a_w->formFlags & kWordUnlocked) != 0; }

		bool PhraseUnlocked(int a_phrase)
		{
			if (a_phrase < 0 || a_phrase >= static_cast<int>(g_t.phrases.size())) { return false; }
			for (auto* w : g_t.phrases[a_phrase].real)
			{
				if (!Unlocked(w)) { return false; }
			}
			return true;
		}

		std::uint64_t KnowledgeHash()
		{
			std::uint64_t h = 1469598103934665603ULL;
			for (const auto& ph : g_t.phrases)
			{
				for (auto* w : ph.real)
				{
					const std::uint64_t v = w ? (w->formFlags & (kWordLearned | kWordUnlocked)) : 0;
					h = (h ^ v) * 1099511628211ULL;
				}
			}
			return h;
		}

		// The phrase slots 1-3 spell in word order, or -1.
		int SpelledPhrase(const std::array<StoneRef, 3>& a_s)
		{
			if (a_s[0].word < 0) { return -1; }
			const int p = g_t.words[a_s[0].word].phrase;
			if (p < 0) { return -1; }
			for (int i = 0; i < 3; ++i)
			{
				if (a_s[i].word != g_t.phrases[p].words[i]) { return -1; }
			}
			return p;
		}

		// ---- created enchantments --------------------------------------------------------------------------------------
		// The created-object manager keeps a reference count per created enchantment and frees one nobody holds; an item's
		// ExtraEnchantment holds one (SKSE's WornObject.CreateEnchantment does the same).
		void AdjustRef(RE::EnchantmentItem* a_e, int a_delta)
		{
			auto* m = RE::BGSCreatedObjectManager::GetSingleton();
			if (!m || !a_e) { return; }
			RE::BSSpinLockGuard guard(m->lock);
			for (auto* arr : { &m->weaponEnchantments, &m->armorEnchantments })
			{
				for (auto& d : *arr)
				{
					if (d.magicItem != a_e) { continue; }
					if (a_delta > 0) { d.refCount = d.refCount + 1; }
					else if (d.refCount > 0) { d.refCount = d.refCount - 1; }
					return;
				}
			}
		}

		std::unordered_set<RE::EffectSetting*> g_glowEffects;   // the five tier glow effects (filled at Install)

		struct Want
		{
			RE::EffectSetting* effect = nullptr;
			float magnitude = 0.0F;
			std::uint32_t area = 0;
			std::uint32_t duration = 0;
		};

		std::vector<Want> WantedEffects(RE::TESBoundObject* a_obj, const Entry& a_e, Host a_host)
		{
			std::vector<Want> out;
			const bool weapon = a_host != Host::kArmour;
			// what the item carried of its own: its instance enchantment, else its base form's
			if (a_e.hadEnchant)
			{
				for (const auto& s : a_e.orig)
				{
					auto* mg = RE::TESForm::LookupByID<RE::EffectSetting>(s.effect);
					if (mg) { out.push_back({ mg, s.magnitude, s.area, s.duration }); }
				}
			}
			else if (auto* ef = a_obj->As<RE::TESEnchantableForm>(); ef && ef->formEnchanting)
			{
				for (auto* fx : ef->formEnchanting->effects)
				{
					if (fx && fx->baseEffect) { out.push_back({ fx->baseEffect, fx->effectItem.magnitude, fx->effectItem.area, fx->effectItem.duration }); }
				}
			}
			const int slots = SlotsOf(a_obj);
			int bestTier = -1;
			bool any = false;
			for (int i = 0; i < 3 && i < slots; ++i)
			{
				const auto& s = a_e.stones[i];
				if (s.word < 0 || s.tier < 0) { continue; }
				any = true;
				bestTier = std::max(bestTier, s.tier);
				const auto& cell = g_t.words[s.word].hosts[static_cast<int>(a_host)];
				if (!cell.effect) { continue; }
				if (cell.kind == "rec")
				{
					out.push_back({ cell.effect, cell.m[s.tier], 0, weapon ? static_cast<std::uint32_t>(cell.dur[s.tier]) : 0 });
				}
				else
				{
					// a card line: the magnitude carries the tier (1-5) so a lost table entry can be rebuilt from the item
					out.push_back({ cell.effect, static_cast<float>(s.tier + 1), 0, 0 });
				}
			}
			if (const int p = SpelledPhrase(a_e.stones); p >= 0 && slots >= 3)
			{
				if (PhraseUnlocked(p))
				{
					const auto& cell = g_t.phrases[p].hosts[static_cast<int>(a_host)];
					if (cell.effect)
					{
						const bool rec = cell.kind == "rec";
						out.push_back({ cell.effect, rec ? cell.m[0] : 1.0F, 0, rec && weapon ? static_cast<std::uint32_t>(cell.dur[0]) : 0 });
					}
				}
				else if (auto* u = weapon ? g_t.unknownWeapon : g_t.unknownArmour)
				{
					out.push_back({ u, 1.0F, 0, 0 });
				}
			}
			if (weapon && any && settings::stones::weaponGlow && bestTier >= 0 && g_t.tiers[bestTier].glow)
			{
				out.insert(out.begin(), Want{ g_t.tiers[bestTier].glow, 1.0F, 0, 0 });   // first AND costliest: whichever the game reads
			}
			return out;
		}

		bool SameEffects(RE::EnchantmentItem* a_ench, const std::vector<Want>& a_want)
		{
			if (!a_ench) { return a_want.empty(); }
			if (a_ench->effects.size() != a_want.size()) { return false; }
			for (std::size_t i = 0; i < a_want.size(); ++i)
			{
				auto* fx = a_ench->effects[static_cast<std::uint32_t>(i)];
				if (!fx || fx->baseEffect != a_want[i].effect || std::fabs(fx->effectItem.magnitude - a_want[i].magnitude) > 0.001F ||
					fx->effectItem.duration != a_want[i].duration || fx->effectItem.area != a_want[i].area ||
					(g_glowEffects.contains(fx->baseEffect) && fx->cost < 999.0F))   // a glow made before 86db carries no cost: re-make it
				{
					return false;
				}
			}
			return true;
		}

		RE::EnchantmentItem* Create(const std::vector<Want>& a_want, bool a_weapon)
		{
			auto* m = RE::BGSCreatedObjectManager::GetSingleton();
			if (!m || a_want.empty()) { return nullptr; }
			RE::BSTArray<RE::Effect> arr;
			for (const auto& w : a_want)
			{
				RE::Effect fx;
				fx.baseEffect = w.effect;
				fx.effectItem.magnitude = w.magnitude;
				fx.effectItem.area = w.area;
				fx.effectItem.duration = w.duration;
				fx.cost = (g_glowEffects.contains(w.effect)) ? 1000.0F : 0.0F;
				arr.push_back(fx);
			}
			auto* ench = a_weapon ? m->AddWeaponEnchantment(arr) : m->AddArmorEnchantment(arr);
			// the manager may hand back an existing enchantment made before the glow carried a cost: give it the cost here
			// (only our own glow lines are touched), so the glow is the costliest effect and the comparison settles
			if (ench)
			{
				for (auto* fx : ench->effects)
				{
					if (fx && g_glowEffects.contains(fx->baseEffect)) { fx->cost = 1000.0F; }
				}
			}
			return ench;
		}

		void Reequip(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_x)
		{
			auto* p = Player();
			auto* em = RE::ActorEquipManager::GetSingleton();
			bool left = false;
			if (!p || !em || !a_obj || !a_x || !Worn(a_x, &left)) { return; }
			if (a_obj->Is(RE::FormType::Weapon))
			{
				// the hand slots of Skyrim.esm (EQUP RightHand 013F42, LeftHand 013F43)
				auto* slot = RE::TESForm::LookupByID<RE::BGSEquipSlot>(left ? 0x00013F43 : 0x00013F42);
				em->UnequipObject(p, a_obj, a_x, 1, slot, false, false, false, true);
				em->EquipObject(p, a_obj, a_x, 1, slot, false, false, false, true);
				logger::debug("soul stones: {} put back in the {} hand so its new enchantment shows", a_obj->GetName(), left ? "left" : "right");
				return;
			}
			if (!a_obj->Is(RE::FormType::Armor)) { return; }
			// a worn armour applies its enchantment when it is equipped, so a changed one is put on again (same extra list:
			// nothing is removed from the inventory, so the pointer stays the game's own)
			em->UnequipObject(p, a_obj, a_x, 1, nullptr, false, false, false, true);
			em->EquipObject(p, a_obj, a_x, 1, nullptr, false, false, false, true);
		}

		std::string DragonLetters(const std::string& a_word)
		{
			static const std::pair<const char*, char> digraphs[] = { { "AA", '1' }, { "EI", '2' }, { "II", '3' }, { "AH", '4' }, { "IR", '7' }, { "OO", '8' } };   // as the game's own word records write them ("D4" = Dah, "T8r" = Toor)
			std::string up = a_word;
			std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			std::string out;
			for (std::size_t i = 0; i < up.size();)
			{
				bool hit = false;
				for (const auto& [pair, ch] : digraphs)
				{
					if (up.compare(i, 2, pair) == 0) { out += ch; i += 2; hit = true; break; }
				}
				if (!hit) { out += up[i++]; }
			}
			return out;
		}

		void UpdateName(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_x, const Entry& a_e, bool a_any)
		{
			if (!a_obj || !a_x) { return; }
			const std::string baseName = a_e.hadName ? a_e.origName : std::string(a_obj->GetName());
			auto* t = a_x->GetByType<RE::ExtraTextDisplayData>();
			if (!a_any || !settings::stones::nameSlots)
			{
				if (a_e.hadName) { if (t) { t->SetName(a_e.origName.c_str()); } }
				else if (t) { a_x->RemoveByType(RE::ExtraDataType::kTextDisplayData); }
				return;
			}
			std::string tail;
			const int slots = SlotsOf(a_obj);
			for (int i = 0; i < slots; ++i)
			{
				const auto& s = a_e.stones[i];
				if (!tail.empty()) { tail += ' '; }
				if (s.word < 0) { tail += '-'; continue; }
				const auto& w = g_t.words[s.word].name;
				tail += settings::stones::nameDragonLetters ? "<font face=\"$DragonFont\">" + DragonLetters(w) + "</font>" : w;
			}
			const std::string name = baseName + " [" + tail + "]";
			if (!t) { a_x->Add(new RE::ExtraTextDisplayData(name.c_str())); }
			else { t->SetName(name.c_str()); }
		}

		// Builds and assigns the instance's enchantment and name; false when nothing had to change.
		bool Compose(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_x, Entry& a_e)
		{
			const Host host = HostOf(a_obj);
			if (host == Host::kNone || !a_x) { return false; }
			const bool weapon = host != Host::kArmour;
			bool any = false;
			for (const auto& s : a_e.stones) { any |= s.word >= 0; }
			auto* xe = a_x->GetByType<RE::ExtraEnchantment>();
			bool changed = false;
			if (!any || !settings::stones::enabled)
			{
				// back to what the item was: its own instance enchantment re-made, or none (the base form's shows again)
				if (a_e.hadEnchant)
				{
					std::vector<Want> orig;
					for (const auto& s : a_e.orig)
					{
						if (auto* mg = RE::TESForm::LookupByID<RE::EffectSetting>(s.effect)) { orig.push_back({ mg, s.magnitude, s.area, s.duration }); }
					}
					if (!xe || !SameEffects(xe->enchantment, orig))
					{
						if (auto* ench = Create(orig, weapon))
						{
							if (xe) { AdjustRef(xe->enchantment, -1); xe->enchantment = ench; xe->charge = a_e.origCharge; }
							else { a_x->Add(new RE::ExtraEnchantment(ench, a_e.origCharge, false)); }
							AdjustRef(ench, +1);
							changed = true;
						}
					}
				}
				else if (xe)
				{
					AdjustRef(xe->enchantment, -1);
					a_x->RemoveByType(RE::ExtraDataType::kEnchantment);
					changed = true;
				}
				UpdateName(a_obj, a_x, a_e, false);
				return changed;
			}
			const auto want = WantedEffects(a_obj, a_e, host);
			if (!xe || !SameEffects(xe->enchantment, want))
			{
				auto* ench = Create(want, weapon);
				if (!ench)
				{
					logger::warn("soul stones: the game made no enchantment for {} ({} effects)", a_obj->GetName(), want.size());
					return false;
				}
				if (weapon)
				{
					// the stones cost nothing to use; an item's own enchantment keeps its own cost per hit
					ench->data.flags.set(RE::EnchantmentItem::EnchantmentFlag::kCostOverride);
					if (a_e.origCost >= 0) { ench->data.costOverride = a_e.origCost; }
					else
					{
						auto* ef = a_obj->As<RE::TESEnchantableForm>();
						ench->data.costOverride = (ef && ef->formEnchanting) ? static_cast<std::int32_t>(ef->formEnchanting->CalculateMagickaCost(nullptr)) : 0;
					}
				}
				const std::uint16_t charge = a_e.hadEnchant ? a_e.origCharge : (weapon ? kNoOriginalCharge : 0);
				if (xe)
				{
					if (xe->enchantment != ench)
					{
						AdjustRef(xe->enchantment, -1);
						xe->enchantment = ench;
						AdjustRef(ench, +1);
					}
					if (!a_e.hadEnchant) { xe->charge = charge; }
				}
				else
				{
					a_x->Add(new RE::ExtraEnchantment(ench, charge, false));
					AdjustRef(ench, +1);
				}
				if (weapon && !a_e.hadEnchant)
				{
					auto* xc = a_x->GetByType<RE::ExtraCharge>();
					if (!xc) { xc = new RE::ExtraCharge(); a_x->Add(xc); }
					if (xc) { xc->charge = static_cast<float>(charge); }
				}
				++g_composes;
				changed = true;
				logger::debug("soul stones: composed {} - {} effects", a_obj->GetName(), want.size());
			}
			UpdateName(a_obj, a_x, a_e, true);
			return changed;
		}

		// A new table entry for an instance: what it carried before its first stone.
		Entry NewEntry(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_x)
		{
			Entry e;
			auto* u = a_x->GetByType<RE::ExtraUniqueID>();
			e.base = a_obj->GetFormID();
			e.owner = u ? u->baseID : 0;
			e.uid = u ? u->uniqueID : 0;
			if (auto* xe = a_x->GetByType<RE::ExtraEnchantment>(); xe && xe->enchantment)
			{
				e.hadEnchant = true;
				e.origCharge = xe->charge;
				if (xe->enchantment->data.flags.any(RE::EnchantmentItem::EnchantmentFlag::kCostOverride)) { e.origCost = xe->enchantment->data.costOverride; }
				else { e.origCost = static_cast<std::int32_t>(xe->enchantment->CalculateMagickaCost(nullptr)); }
				for (auto* fx : xe->enchantment->effects)
				{
					if (fx && fx->baseEffect) { e.orig.push_back({ fx->baseEffect->GetFormID(), fx->effectItem.magnitude, fx->effectItem.area, fx->effectItem.duration }); }
				}
			}
			if (auto* t = a_x->GetByType<RE::ExtraTextDisplayData>(); t && t->displayName.size() > 0)
			{
				e.hadName = true;
				e.origName = t->displayName.c_str();
			}
			return e;
		}

		// A lost key (the item moved through a container that renumbered it): rebuild the entry from the item's own
		// composed enchantment - our effects say word, host and tier, in slot order; anything else was the item's own.
		bool Repair(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_x, Entry& a_out)
		{
			auto* xe = a_x->GetByType<RE::ExtraEnchantment>();
			if (!xe || !xe->enchantment) { return false; }
			const Host host = HostOf(a_obj);
			Entry e;
			auto* u = a_x->GetByType<RE::ExtraUniqueID>();
			e.base = a_obj->GetFormID();
			e.owner = u ? u->baseID : 0;
			e.uid = u ? u->uniqueID : 0;
			int slot = 0;
			bool ours = false;
			std::vector<SavedEffect> other;
			for (auto* fx : xe->enchantment->effects)
			{
				if (!fx || !fx->baseEffect) { continue; }
				if (auto it = g_t.effectCell.find(fx->baseEffect); it != g_t.effectCell.end())
				{
					ours = true;
					const auto& cell = g_t.words[it->second.first].hosts[it->second.second];
					int tier = 0;
					if (cell.kind == "rec")
					{
						float best = 1e9F;
						for (int t = 0; t < kTiers; ++t)
						{
							const float d = std::fabs(cell.m[t] - fx->effectItem.magnitude);
							if (d < best) { best = d; tier = t; }
						}
					}
					else { tier = std::clamp(static_cast<int>(std::lround(fx->effectItem.magnitude)) - 1, 0, kTiers - 1); }
					if (slot < 3) { e.stones[slot++] = { it->second.first, tier }; }
					continue;
				}
				if (g_t.phraseCell.contains(fx->baseEffect) || g_t.glowTier.contains(fx->baseEffect) || fx->baseEffect == g_t.unknownWeapon || fx->baseEffect == g_t.unknownArmour)
				{
					ours = true;
					continue;
				}
				other.push_back({ fx->baseEffect->GetFormID(), fx->effectItem.magnitude, fx->effectItem.area, fx->effectItem.duration });
			}
			if (!ours || host == Host::kNone) { return false; }
			// the base form's own enchantment is not an "original" of the instance
			bool same = false;
			if (auto* ef = a_obj->As<RE::TESEnchantableForm>(); ef && ef->formEnchanting && ef->formEnchanting->effects.size() == other.size())
			{
				same = true;
				for (std::size_t i = 0; i < other.size(); ++i)
				{
					auto* fx = ef->formEnchanting->effects[static_cast<std::uint32_t>(i)];
					if (!fx || !fx->baseEffect || fx->baseEffect->GetFormID() != other[i].effect) { same = false; break; }
				}
			}
			if (!other.empty() && !same)
			{
				e.hadEnchant = true;
				e.orig = std::move(other);
				e.origCharge = xe->charge;
			}
			a_out = std::move(e);
			return true;
		}

		// ---- perks, abilities, granted Shouts (from what is equipped) -----------------------------------------------------
		void SetPerkValue(RE::BGSPerk* a_perk, float a_v)
		{
			if (!a_perk) { return; }
			for (auto* entry : a_perk->perkEntries)
			{
				if (!entry || entry->GetType() != RE::PERK_ENTRY_TYPE::kEntryPoint) { continue; }
				auto* ep = static_cast<RE::BGSEntryPointPerkEntry*>(entry);
				if (ep->functionData && ep->functionData->GetType() == RE::BGSEntryPointFunctionData::ENTRY_POINT_FUNCTION_DATA::kOneValue)
				{
					static_cast<RE::BGSEntryPointFunctionDataOneValue*>(ep->functionData)->data = a_v;
				}
			}
		}

		void ApplyEquipped(std::vector<std::string>& a_active)
		{
			auto* p = Player();
			if (!p) { return; }
			std::unordered_map<RE::BGSPerk*, int> perks;
			std::unordered_map<RE::SpellItem*, int> abilities;
			for (const auto& w : g_t.words)
			{
				for (const auto& c : w.hosts)
				{
					if (c.perk) { perks.emplace(c.perk, -1); }
					if (c.ability) { abilities.emplace(c.ability, -1); }
				}
			}
			std::vector<std::array<bool, 3>> worn(g_t.phrases.size(), std::array<bool, 3>{});
			if (settings::stones::enabled)
			{
				for (const auto& eq : g_equipped)
				{
					for (const auto& s : eq.stones)
					{
						if (s.word < 0) { continue; }
						const auto& c = g_t.words[s.word].hosts[static_cast<int>(eq.host)];
						if (c.perk) { perks[c.perk] = std::max(perks[c.perk], s.tier); }
						if (c.ability) { abilities[c.ability] = std::max(abilities[c.ability], s.tier); }
						if (eq.host == Host::kArmour)
						{
							const auto& w = g_t.words[s.word];
							if (w.phrase >= 0) { worn[w.phrase][w.position] = true; }
						}
					}
				}
			}
			for (const auto& [perk, tier] : perks)
			{
				const bool has = p->HasPerk(perk);
				if (tier >= 0)
				{
					for (const auto& w : g_t.words)
					{
						for (const auto& c : w.hosts) { if (c.perk == perk) { SetPerkValue(perk, c.value[tier]); } }
					}
					if (!has) { p->AddPerk(perk); }
					a_active.push_back(fmt::format("perk {} ({})", perk->GetName(), g_t.tierNames[tier]));
				}
				else if (has) { p->RemovePerk(perk); }
			}
			for (const auto& [spell, tier] : abilities)
			{
				const bool has = p->HasSpell(spell);
				if (tier >= 0)
				{
					float v = 0.0F;
					for (const auto& w : g_t.words)
					{
						for (const auto& c : w.hosts) { if (c.ability == spell) { v = c.value[tier]; } }
					}
					auto* fx = spell->effects.empty() ? nullptr : spell->effects[0];
					const bool differs = fx && std::fabs(fx->effectItem.magnitude - v) > 0.0001F;
					if (fx) { fx->effectItem.magnitude = v; }
					if (has && differs) { p->RemoveSpell(spell); p->AddSpell(spell); }
					else if (!has) { p->AddSpell(spell); }
					a_active.push_back(fmt::format("ability {} ({})", spell->GetName(), g_t.tierNames[tier]));
				}
				else if (has) { p->RemoveSpell(spell); }
			}
			// D21: the clone Shout at the level of the run of words, from the first, socketed in worn armour AND learned at
			// a wall; its words are marked known and unlocked up to that level and cleared above it
			for (std::size_t i = 0; i < g_t.phrases.size(); ++i)
			{
				auto& ph = g_t.phrases[i];
				if (!ph.clone) { continue; }
				int level = 0;
				while (settings::stones::enabled && level < 3 && worn[i][level] && Learned(ph.real[level])) { ++level; }
				for (int k = 0; k < 3; ++k)
				{
					if (auto* w = ph.cloneWords[k])
					{
						if (k < level) { w->formFlags |= (kWordLearned | kWordUnlocked); }
						else { w->formFlags &= ~(kWordLearned | kWordUnlocked); }
					}
				}
				static std::array<int, 32> s_levels{};
				if (i < s_levels.size() && s_levels[i] != level)
				{
					logger::info("soul stones: Shout {} level {} -> {}", ph.shout, s_levels[i], level);
					s_levels[i] = level;
				}
				const bool has = p->HasShout(ph.clone);
				if (level > 0 && !has) { p->AddShout(ph.clone); logger::info("soul stones: Shout {} granted at level {}", ph.clone->GetName(), level); }
				else if (level == 0 && has)
				{
					if (auto* npc = p->GetActorBase(); npc && npc->GetSpellList()) { npc->GetSpellList()->RemoveShout(ph.clone); }
					logger::info("soul stones: Shout {} taken back", ph.clone->GetName());
				}
				if (level > 0) { a_active.push_back(fmt::format("Shout {} level {}", ph.shout, level)); }
			}
		}

		// The runesmith's quest asks for a word of power learned at a wall: any word of the game's, ours excluded.
		void UpdateLearnedGlobal()
		{
			if (!g_rs.learned) { return; }
			auto* handler = RE::TESDataHandler::GetSingleton();
			if (!handler || g_rs.learned->value >= 1.0F) { return; }
			for (auto* w : handler->GetFormArray<RE::TESWordOfPower>())
			{
				if (!w || (w->formFlags & kWordLearned) == 0) { continue; }
				bool clone = false;
				for (const auto& ph : g_t.phrases) { for (auto* c : ph.cloneWords) { clone |= c == w; } }
				if (clone) { continue; }
				g_rs.learned->value = 1.0F;
				logger::info("soul stones: a word of power is learned ({}) - the runesmith's quest can go on", w->GetName());
				return;
			}
		}

		void CloseService(const char* a_why)
		{
			if (!g_service.exchange(false)) { return; }
			logger::info("soul stones: the runesmith's service closes ({})", a_why);
			if (g_windowOpener) { g_windowOpener(false); }
		}

		// ---- the main-thread pass ----------------------------------------------------------------------------------------
		void BuildSnapshot();

		// Re-composes every socketed instance the player carries (idempotent), rebuilds the equipped list and applies the
		// perks, abilities and granted Shouts. a_repair also rebuilds lost table entries from the items' enchantments.
		void Recompute(bool a_repair)
		{
			if (!g_t.loaded) { return; }
			g_knowledgeHash = KnowledgeHash();
			UpdateLearnedGlobal();
			g_equipped.clear();
			std::vector<std::pair<RE::TESBoundObject*, RE::ExtraDataList*>> reequip;
			ForEachInstance([&](RE::InventoryEntryData* e, RE::ExtraDataList* x) {
				std::string key;
				if (!KeyOf(e->object, x, key)) { return; }
				auto it = g_entries.find(key);
				if (it == g_entries.end())
				{
					Entry rebuilt;
					if (!a_repair || !Repair(e->object, x, rebuilt)) { return; }
					logger::info("soul stones: rebuilt the table entry of {} from its enchantment (its key had changed)", e->object->GetName());
					it = g_entries.emplace(key, std::move(rebuilt)).first;
				}
				const bool changed = Compose(e->object, x, it->second);
				bool left = false;
				if (Worn(x, &left))
				{
					if (changed) { reequip.emplace_back(e->object, x); }
					Equipped eq;
					eq.host = HostOf(e->object);
					eq.base = e->object->GetFormID();
					eq.left = left;
					eq.stones = it->second.stones;
					const int p = SpelledPhrase(eq.stones);
					eq.phrase = (p >= 0 && SlotsOf(e->object) >= 3 && PhraseUnlocked(p)) ? p : -1;
					g_equipped.push_back(eq);
				}
			});
			for (auto& [obj, x] : reequip) { Reequip(obj, x); }
			std::vector<std::string> active;
			ApplyEquipped(active);
			{
				std::scoped_lock l(g_lock);
				g_snapshot.activePerks = std::move(active);
			}
			BuildSnapshot();
		}

		int CountOf(RE::TESBoundObject* a_obj)
		{
			auto* p = Player();
			if (!p || !a_obj) { return 0; }
			const auto counts = p->GetInventoryCounts([a_obj](RE::TESBoundObject& o) { return &o == a_obj; });
			const auto it = counts.find(a_obj);
			return it == counts.end() ? 0 : it->second;
		}

		void BuildSnapshot()
		{
			Snapshot s;
			s.loaded = g_t.loaded;
			s.error = g_t.error;
			s.words = g_t.wordNames;
			s.tiers = g_t.tierNames;
			s.serviceOpen = g_service.load();
			s.tableEntries = static_cast<std::uint32_t>(g_entries.size());
			s.composes = g_composes;
			s.procsFired = g_procsFired.load();
			auto* p = Player();
			if (g_t.loaded && p)
			{
				// stones and fee gems
				const auto counts = p->GetInventoryCounts([](RE::TESBoundObject& o) { return o.Is(RE::FormType::Misc) || o.Is(RE::FormType::SoulGem) || o.Is(RE::FormType::Weapon) || o.Is(RE::FormType::Armor); });
				for (const auto& [obj, n] : counts)
				{
					if (n <= 0 || !obj) { continue; }
					if (auto it = g_t.stoneForm.find(obj->GetFormID()); it != g_t.stoneForm.end()) { s.stones.push_back({ it->second.first, it->second.second, n }); }
					for (int t = 0; t < kTiers; ++t)
					{
						for (auto* g : g_t.tiers[t].feeGems) { if (g == obj) { s.feeGems[t] += n; } }
					}
				}
				std::sort(s.stones.begin(), s.stones.end(), [](const StoneView& a, const StoneView& b) { return a.word != b.word ? a.word < b.word : a.tier < b.tier; });
				// eligible items: every instance with an extra list, plus one row per plain stack
				std::unordered_map<RE::TESBoundObject*, int> listed;
				ForEachInstance([&](RE::InventoryEntryData* e, RE::ExtraDataList* x) {
					const int slots = SlotsOf(e->object);
					listed[e->object] += std::max(1, static_cast<int>(x->GetCount()));
					if (slots <= 0) { return; }
					ItemView v;
					v.host = HostOf(e->object);
					v.slots = slots;
					v.equipped = Worn(x);
					v.count = std::max(1, static_cast<int>(x->GetCount()));
					const char* dn = x->GetDisplayName(e->object);
					v.name = dn ? dn : e->object->GetName();
					std::string key;
					if (KeyOf(e->object, x, key))
					{
						v.key = key;
						if (auto it = g_entries.find(key); it != g_entries.end()) { v.stones = it->second.stones; }
					}
					else { v.key = fmt::format("{:08X}:list:{:X}", e->object->GetFormID(), reinterpret_cast<std::uintptr_t>(x)); }
					v.phrase = SpelledPhrase(v.stones);
					v.phraseKnown = v.phrase >= 0 && PhraseUnlocked(v.phrase);
					s.items.push_back(std::move(v));
				});
				for (const auto& [obj, n] : counts)
				{
					if (!obj || n <= 0 || SlotsOf(obj) <= 0) { continue; }
					const int plain = n - listed[obj];
					if (plain <= 0) { continue; }
					ItemView v;
					v.key = fmt::format("{:08X}:plain", obj->GetFormID());
					v.name = obj->GetName();
					v.host = HostOf(obj);
					v.slots = SlotsOf(obj);
					v.count = plain;
					s.items.push_back(std::move(v));
				}
				std::sort(s.items.begin(), s.items.end(), [](const ItemView& a, const ItemView& b) { return a.equipped != b.equipped ? a.equipped : a.name < b.name; });
				// phrases: the blueprint (learned at a wall) and the working state (unlocked)
				for (std::size_t i = 0; i < g_t.phrases.size(); ++i)
				{
					const auto& ph = g_t.phrases[i];
					PhraseView pv;
					pv.shout = ph.shout;
					pv.words = ph.words;
					pv.unlocked = PhraseUnlocked(static_cast<int>(i));
					for (auto* w : ph.real) { pv.learned += Learned(w) ? 1 : 0; }
					if (ph.clone && p->HasShout(ph.clone))
					{
						for (auto* w : ph.cloneWords) { pv.granted += (w && (w->formFlags & kWordUnlocked)) ? 1 : 0; }
					}
					s.phrases.push_back(pv);
				}
			}
			std::scoped_lock l(g_lock);
			s.lastResult = g_snapshot.lastResult;
			s.activePerks = g_snapshot.activePerks;
			s.revision = g_snapshot.revision + 1;
			g_snapshot = std::move(s);
		}

		// ---- socketing (D22) -------------------------------------------------------------------------------------------
		bool PayFee(int a_tier, std::string& a_why)
		{
			auto* p = Player();
			for (auto* g : g_t.tiers[a_tier].feeGems)
			{
				if (g && CountOf(g) > 0)
				{
					p->RemoveItem(g, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
					return true;
				}
			}
			a_why = Say("NJ_SS_RFee", "The fee is one %s soul gem, and you carry none.", TierName(a_tier).c_str());
			return false;
		}

		bool HasFee(int a_tier)
		{
			for (auto* g : g_t.tiers[a_tier].feeGems) { if (g && CountOf(g) > 0) { return true; } }
			return false;
		}

		// A plain stack has no extra list of its own, and this CommonLib cannot construct one (no ExtraDataList constructor
		// in either build line's library). Equipping an item gives it one (the game adds ExtraWorn to a list of its own),
		// so the page asks for the item to be worn once; after its first stone the list keeps its ID for good.
		RE::ExtraDataList* Individualise(RE::TESBoundObject* a_obj, RE::InventoryEntryData*& a_entry)
		{
			a_entry = nullptr;
			SetResult(Say("NJ_SS_RPlain", "%s is still part of a plain stack - wear or wield it once, then set the stone.", a_obj ? a_obj->GetName() : ""));
			return nullptr;
		}

		RE::TESBoundObject* BaseOfKey(const std::string& a_key)
		{
			try { return RE::TESForm::LookupByID<RE::TESBoundObject>(static_cast<RE::FormID>(std::stoul(a_key.substr(0, 8), nullptr, 16))); }
			catch (...) { return nullptr; }
		}

		// Finds (or makes) the instance a key names: an instance key, a listed instance without an ID yet, or a plain stack.
		RE::ExtraDataList* Resolve(const std::string& a_key, RE::TESBoundObject*& a_obj, std::string& a_finalKey)
		{
			a_obj = BaseOfKey(a_key);
			if (!a_obj) { return nullptr; }
			RE::InventoryEntryData* e = nullptr;
			RE::ExtraDataList* x = nullptr;
			if (a_key.find(":plain") != std::string::npos) { x = Individualise(a_obj, e); }
			else if (a_key.find(":list:") != std::string::npos)
			{
				const auto ptr = static_cast<std::uintptr_t>(std::stoull(a_key.substr(a_key.rfind(':') + 1), nullptr, 16));
				auto* changes = Player()->GetInventoryChanges();
				ForEachInstance([&](RE::InventoryEntryData* en, RE::ExtraDataList* xl) {
					if (en->object == a_obj && reinterpret_cast<std::uintptr_t>(xl) == ptr) { x = xl; }
				});
				if (x && changes && !x->HasType(RE::ExtraDataType::kUniqueID)) { changes->SetUniqueID(x, nullptr, a_obj); }
			}
			else { FindInstance(a_key, e, x); }
			if (x && !KeyOf(a_obj, x, a_finalKey)) { return nullptr; }
			return x;
		}

		void DoSocket(std::string a_key, int a_slot, int a_word, int a_tier, bool a_unsocket)
		{
			auto* p = Player();
			if (!g_t.loaded || !p) { SetResult(Say("NJ_SS_RNotLoaded", "The soul stone table is not loaded - see the log.")); return; }
			if (!g_service.load()) { SetResult(Say("NJ_SS_RService", "Stones are set and taken out only at the runesmith.")); BuildSnapshot(); return; }
			RE::TESBoundObject* obj = nullptr;
			if (!a_unsocket)
			{
				if (a_word < 0 || a_word >= static_cast<int>(g_t.words.size()) || a_tier < 0 || a_tier >= kTiers) { SetResult(Say("NJ_SS_RNoSuchStone", "There is no such stone.")); return; }
				auto* stone = g_t.words[a_word].stones[a_tier];
				if (!stone || CountOf(stone) <= 0) { SetResult(Say("NJ_SS_RNoStone", "You carry no such stone.")); BuildSnapshot(); return; }
				if (!HasFee(a_tier)) { SetResult(Say("NJ_SS_RFee", "The fee is one %s soul gem, and you carry none.", TierName(a_tier).c_str())); BuildSnapshot(); return; }
				obj = BaseOfKey(a_key);
				if (!obj || a_slot < 0 || a_slot >= SlotsOf(obj)) { SetResult(Say("NJ_SS_RNoSlot", "That item has no such slot.")); BuildSnapshot(); return; }
			}
			std::string key;
			auto* x = Resolve(a_key, obj, key);
			if (!x || !obj)
			{
				if (a_key.find(":plain") == std::string::npos) { SetResult(Say("NJ_SS_RNoItem", "The item was not found in your pack.")); }
				BuildSnapshot();
				return;
			}
			const int slots = SlotsOf(obj);
			if (a_slot < 0 || a_slot >= slots) { SetResult(Say("NJ_SS_RNoSlot", "That item has no such slot.")); BuildSnapshot(); return; }
			auto it = g_entries.find(key);
			std::string why;
			if (a_unsocket)
			{
				if (it == g_entries.end() || it->second.stones[a_slot].word < 0) { SetResult(Say("NJ_SS_REmpty", "That slot is empty.")); BuildSnapshot(); return; }
				const auto s = it->second.stones[a_slot];
				if (!PayFee(s.tier, why)) { SetResult(why); BuildSnapshot(); return; }
				if (auto* stone = g_t.words[s.word].stones[s.tier]) { p->AddObjectToContainer(stone, nullptr, 1, nullptr); }
				it->second.stones[a_slot] = {};
				SetResult(Say("NJ_SS_RTaken", "%s (%s) taken out of %s - the stone is back in your pack.", g_t.words[s.word].name.c_str(), TierName(s.tier).c_str(), obj->GetName()));
			}
			else
			{
				if (it == g_entries.end()) { it = g_entries.emplace(key, NewEntry(obj, x)).first; }
				if (it->second.stones[a_slot].word >= 0) { SetResult(Say("NJ_SS_RFull", "That slot holds a stone already - take it out first.")); BuildSnapshot(); return; }
				if (!PayFee(a_tier, why)) { SetResult(why); BuildSnapshot(); return; }
				p->RemoveItem(g_t.words[a_word].stones[a_tier], 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
				it->second.stones[a_slot] = { a_word, a_tier };
				SetResult(Say("NJ_SS_RSet", "%s (%s) set into slot %d of %s.", g_t.words[a_word].name.c_str(), TierName(a_tier).c_str(), a_slot + 1, obj->GetName()));
			}
			const bool changed = Compose(obj, x, it->second);
			bool any = false;
			for (const auto& s : it->second.stones) { any |= s.word >= 0; }
			if (changed) { Reequip(obj, x); }
			if (!any) { g_entries.erase(it); }
			Recompute(false);
		}

		// ---- procs --------------------------------------------------------------------------------------------------
		float HealthFraction(RE::Actor* a_actor)
		{
			auto* avo = a_actor ? a_actor->AsActorValueOwner() : nullptr;
			if (!avo) { return 1.0F; }
			const float max = avo->GetPermanentActorValue(RE::ActorValue::kHealth);
			return max > 0.0F ? avo->GetActorValue(RE::ActorValue::kHealth) / max : 1.0F;
		}

		void Damage(RE::Actor* a_actor, float a_amount)
		{
			auto* avo = a_actor ? a_actor->AsActorValueOwner() : nullptr;
			if (!avo || a_amount <= 0.0F) { return; }
#if RUNTIME_LINE == 17
			avo->DamageActorValue(RE::ActorValue::kHealth, a_amount);
#else
			avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -a_amount);
#endif
		}

		bool Stormy()
		{
			auto* sky = RE::Sky::GetSingleton();
			auto* w = sky ? sky->currentWeather : nullptr;
			if (!w) { return false; }
			return w->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy) || w->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow);
		}

		std::vector<RE::Actor*> Hostiles(RE::Actor* a_around, float a_radius, RE::Actor* a_except)
		{
			std::vector<RE::Actor*> out;
			auto* p = Player();
			auto* pl = RE::ProcessLists::GetSingleton();
			if (!p || !pl || !a_around) { return out; }
			const auto centre = a_around->GetPosition();
			for (auto& h : pl->highActorHandles)
			{
				auto ptr = h.get();
				auto* a = ptr.get();
				if (!a || a == p || a == a_except || a->IsDead() || !a->IsHostileToActor(p)) { continue; }
				if (a->GetPosition().GetDistance(centre) <= a_radius) { out.push_back(a); }
			}
			return out;
		}

		float CooldownMult()
		{
			float m = 1.0F;
			for (const auto& eq : g_equipped)
			{
				if (eq.host != Host::kArmour) { continue; }
				for (const auto& s : eq.stones)
				{
					if (s.word < 0) { continue; }
					const auto& c = g_t.words[s.word].hosts[2];
					if (c.hasProc && c.proc.on == Proc::On::kCooldown) { m = std::min(m, c.proc.mult[s.tier]); }
				}
			}
			return m;
		}

		struct Event
		{
			Proc::On on = Proc::On::kHit;
			RE::FormID source = 0;
			RE::ObjectRefHandle other;   // the target (hit, kill) or the attacker (hit-by)
			bool power = false, sneak = false, blocked = false;
		};

		void Fire(const std::string& a_key, const Cell& a_cell, int a_tier, const Event& a_ev, RE::Actor* a_other)
		{
			const auto& pr = a_cell.proc;
			auto* p = Player();
			if (!p || pr.on != a_ev.on) { return; }
			if (pr.power && !a_ev.power) { return; }
			if (pr.sneak && !a_ev.sneak) { return; }
			if (pr.blocked && !a_ev.blocked) { return; }
			if (pr.lowHealth && HealthFraction(p) >= g_t.lowHealth) { return; }
			if (pr.storm && !Stormy()) { return; }
			if (pr.targetKeyword && (!a_other || !a_other->HasKeyword(pr.targetKeyword))) { return; }
			const auto now = Clock::now();
			if (pr.every > 0)
			{
				auto& [n, last] = g_streaks[a_key];
				n = (now - last) <= std::chrono::duration<float>(pr.window) ? n + 1 : 1;
				last = now;
				if (n < pr.every) { return; }
				n = 0;
			}
			static thread_local std::mt19937 rng{ std::random_device{}() };
			if (pr.chance > 0 && static_cast<int>(rng() % 100) >= pr.chance) { return; }
			if (pr.cooldown > 0.0F)
			{
				const auto it = g_cooldowns.find(a_key);
				const auto cd = std::chrono::duration<float>(pr.cooldown * CooldownMult());
				if (it != g_cooldowns.end() && now - it->second < cd) { return; }
				g_cooldowns[a_key] = now;
			}
			float mag = pr.hasMag ? pr.mag[std::clamp(a_tier, 0, kTiers - 1)] : 0.0F;
			if (pr.stack > 0)
			{
				auto& [n, last] = g_streaks[a_key];
				n = (now - last) <= std::chrono::duration<float>(pr.window) ? std::min(n + 1, pr.stack) : 1;
				last = now;
				mag *= static_cast<float>(n);
			}
			std::vector<RE::Actor*> targets;
			if (pr.at == "self") { targets.push_back(p); }
			else if (pr.at == "nearby") { targets = Hostiles(p, 1024.0F, nullptr); }
			else if (pr.at == "second")
			{
				auto close = Hostiles(a_other ? a_other : p, 512.0F, a_other);
				if (!close.empty()) { targets.push_back(close.front()); }
			}
			else if (a_other && !a_other->IsDead()) { targets.push_back(a_other); }
			else if (a_other && pr.on == Proc::On::kKill) { targets.push_back(a_other); }   // raise the dead
			if (targets.empty()) { return; }
			auto* caster = p->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
			for (auto* t : targets)
			{
				if (pr.damage > 0.0F) { Damage(t, pr.damage); }
				if (pr.spell && caster)
				{
					caster->CastSpellImmediate(pr.spell, false, t, 1.0F, false, mag, p);
				}
			}
			g_procsFired.fetch_add(1);
			logger::debug("soul stones: proc {} fired on {} target(s)", a_key, targets.size());
		}

		void HandleEvent(const Event& a_ev)
		{
			if (!g_t.loaded || !settings::stones::enabled) { return; }
			auto ref = a_ev.other.get();
			auto* other = ref ? ref->As<RE::Actor>() : nullptr;
			for (const auto& eq : g_equipped)
			{
				const bool weapon = eq.host != Host::kArmour;
				if (a_ev.on == Proc::On::kHit && (!weapon || eq.base != a_ev.source)) { continue; }
				if (a_ev.on == Proc::On::kHitBy && weapon) { continue; }
				for (int i = 0; i < 3; ++i)
				{
					const auto& s = eq.stones[i];
					if (s.word < 0) { continue; }
					const auto& c = g_t.words[s.word].hosts[static_cast<int>(eq.host)];
					if (c.hasProc) { Fire(fmt::format("{}/{}/{}", g_t.words[s.word].name, static_cast<int>(eq.host), eq.left), c, s.tier, a_ev, other); }
				}
				if (eq.phrase >= 0)
				{
					const auto& c = g_t.phrases[eq.phrase].hosts[static_cast<int>(eq.host)];
					if (c.hasProc) { Fire(fmt::format("{}/phrase/{}/{}", g_t.phrases[eq.phrase].shout, static_cast<int>(eq.host), eq.left), c, 0, a_ev, other); }
				}
			}
		}

		// ---- event sinks (copy, then queue) ----------------------------------------------------------------------------
		void Queue(std::function<void()> a_fn)
		{
			if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask(std::move(a_fn)); }
		}

		class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_e, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				auto* p = Player();
				if (!a_e || !p || g_equipped.empty()) { return RE::BSEventNotifyControl::kContinue; }
				const bool byPlayer = a_e->cause.get() == p;
				const bool onPlayer = a_e->target.get() == p;
				if (!byPlayer && !onPlayer) { return RE::BSEventNotifyControl::kContinue; }
				Event ev;
				using F = RE::TESHitEvent::Flag;
				ev.on = byPlayer ? Proc::On::kHit : Proc::On::kHitBy;
				ev.source = a_e->source;
				ev.other = byPlayer ? (a_e->target ? a_e->target->GetHandle() : RE::ObjectRefHandle{}) : (a_e->cause ? a_e->cause->GetHandle() : RE::ObjectRefHandle{});
				ev.power = a_e->flags.any(F::kPowerAttack);
				ev.sneak = a_e->flags.any(F::kSneakAttack);
				ev.blocked = a_e->flags.any(F::kHitBlocked);
				Queue([ev]() { HandleEvent(ev); });
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_e, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				auto* p = Player();
				if (!a_e || !a_e->dead || !p || a_e->actorKiller.get() != p) { return RE::BSEventNotifyControl::kContinue; }
				Event ev;
				ev.on = Proc::On::kKill;
				ev.other = a_e->actorDying ? a_e->actorDying->GetHandle() : RE::ObjectRefHandle{};
				Queue([ev]() { HandleEvent(ev); });
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_e, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (a_e && a_e->actor && a_e->actor.get() == Player()) { g_dirty = true; }
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				// a word learned at a wall or unlocked in the magic menu: the next pass sees the knowledge change
				if (a_e && !a_e->opening) { g_knowledgeDirty = true; }
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class TopicSink : public RE::BSTEventSink<RE::TESTopicInfoEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESTopicInfoEvent* a_e, RE::BSTEventSource<RE::TESTopicInfoEvent>*) override
			{
				if (!a_e || !g_rs.loaded || a_e->topicInfoFormID != g_rs.socketInfo || a_e->type.get() != RE::TESTopicInfoEvent::TopicInfoEventType::kTopicEnd)
				{
					return RE::BSEventNotifyControl::kContinue;
				}
				Queue([]() {
					g_service = true;
					logger::info("soul stones: the runesmith opens his service");
					BuildSnapshot();
					if (g_windowOpener) { g_windowOpener(true); }
				});
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		HitSink g_hit;
		TopicSink g_topic;
		DeathSink g_death;
		EquipSink g_equip;
		MenuSink g_menu;

		// The pacer: a quarter-second after an equip or a menu closing, one main-thread pass (several events arrive for
		// one change). It only queues; nothing here touches the game.
		void Worker()
		{
			while (g_running)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				static int ticks = 0;
				++ticks;
				if (g_rs.loaded && g_rs.homeCell && ticks % 4 == 0)
				{
					Queue([]() {
						// Q67 (Fable 14:20): no override of the inn's CELL record - he is brought in the first time the player
						// is there, and stays (a persistent reference: the save keeps where he is)
						auto* p = Player();
						auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_rs.npcRef);
						auto* pc = p ? p->GetParentCell() : nullptr;
						if (!p || !ref || !pc || pc->GetFormID() != g_rs.homeCell) { return; }
						auto* rc = ref->GetParentCell();
						if (rc && rc->GetFormID() == g_rs.homeCell) { return; }
						ref->MoveTo(p);
						ref->SetPosition(g_rs.homePos);
						ref->data.angle.z = g_rs.homeAngleZ;
						logger::info("soul stones: the runesmith is brought to his place in the inn ({:.0f}, {:.0f}, {:.0f})", g_rs.homePos.x, g_rs.homePos.y, g_rs.homePos.z);
					});
				}
				if (g_service && g_rs.loaded && ticks % 4 == 0)
				{
					Queue([]() {
						auto* p = Player();
						auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_rs.npcRef);
						if (p && ref && (ref->GetParentCell() != p->GetParentCell() || ref->GetPosition().GetDistance(p->GetPosition()) > g_rs.radius)) { CloseService("you walked away"); }
					});
				}
				if (ticks % 4 == 0 && g_t.loaded) { g_knowledgeDirty = true; }
				const bool dirty = g_dirty.exchange(false);
				const bool knowledge = g_knowledgeDirty.exchange(false);
				if (!dirty && !knowledge) { continue; }
				Queue([dirty]() {
					if (dirty || KnowledgeHash() != g_knowledgeHash) { Recompute(false); }
				});
			}
		}
	}

	// ---- public ------------------------------------------------------------------------------------------------------
	void Install()
	{
		const auto path = std::filesystem::path("Data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/stones.json");
		auto fail = [](std::string a_why) {
			logger::error("soul stones: {} - the module is off", a_why);
			g_t.error = std::move(a_why);
			std::scoped_lock l(g_lock);
			g_snapshot.error = g_t.error;
		};
		auto* handler = RE::TESDataHandler::GetSingleton();
		if (!handler || !handler->LookupModByName(kPlugin)) { fail(std::string(kPlugin) + " is not loaded"); return; }
		nlohmann::json j;
		try
		{
			std::ifstream in(path);
			if (!in) { fail("stones.json is missing"); return; }
			j = nlohmann::json::parse(in);
		}
		catch (const std::exception& e) { fail(std::string("stones.json is not valid: ") + e.what()); return; }
		try
		{
			g_t.lowHealth = j.value("lowHealth", 0.30F);
			for (auto& [name, slots] : j["slotsByItemTier"].items())
			{
				if (auto* kw = Form<RE::BGSKeyword>(j["itemTierKeywords"].value(name, nlohmann::json()), "item tier keyword")) { g_t.itemTierSlots.emplace_back(kw, slots.get<int>()); }
			}
			const auto& tiers = j["tiers"];
			for (int t = 0; t < kTiers && t < static_cast<int>(tiers.size()); ++t)
			{
				g_t.tiers[t].name = tiers[t].value("name", std::string());
				g_t.tierNames.push_back(g_t.tiers[t].name);
				g_t.tiers[t].glow = Form<RE::EffectSetting>(tiers[t].value("glow", nlohmann::json()), "glow");
				if (g_t.tiers[t].glow) { g_t.glowTier[g_t.tiers[t].glow] = t; g_glowEffects.insert(g_t.tiers[t].glow); }
				for (const auto& g : tiers[t]["feeGems"])
				{
					if (auto* gem = Form<RE::TESBoundObject>(g, "fee gem")) { g_t.tiers[t].feeGems.push_back(gem); }
				}
			}
			g_t.unknownWeapon = Form<RE::EffectSetting>(j["unknownPhrase"].value("weapon", nlohmann::json()), "unknown phrase");
			g_t.unknownArmour = Form<RE::EffectSetting>(j["unknownPhrase"].value("armour", nlohmann::json()), "unknown phrase");
			for (auto& [name, v] : j["helpers"].items()) { g_t.helpers[name] = Form<RE::SpellItem>(v, "helper"); }
			for (const auto& w : j["words"])
			{
				Word word;
				word.name = w.value("word", std::string());
				word.phrase = w.value("phrase", -1);
				word.position = w.value("position", 0);
				const int wi = static_cast<int>(g_t.words.size());
				for (int t = 0; t < kTiers && t < static_cast<int>(w["stones"].size()); ++t)
				{
					word.stones[t] = Form<RE::TESBoundObject>(w["stones"][t], "stone");
					if (word.stones[t]) { g_t.stoneForm[word.stones[t]->GetFormID()] = { wi, t }; }
				}
				const char* hosts[] = { "melee", "ranged", "armour" };
				for (int h = 0; h < 3; ++h)
				{
					word.hosts[h] = ReadCell(w["hosts"][hosts[h]]);
					if (word.hosts[h].effect) { g_t.effectCell[word.hosts[h].effect] = { wi, h }; }
				}
				g_t.wordNames.push_back(word.name);
				g_t.words.push_back(std::move(word));
			}
			for (const auto& p : j["phrases"])
			{
				Phrase ph;
				ph.shout = p.value("shout", std::string());
				const int pi = static_cast<int>(g_t.phrases.size());
				for (int i = 0; i < 3; ++i)
				{
					const auto name = p["words"][i].get<std::string>();
					for (int w = 0; w < static_cast<int>(g_t.words.size()); ++w)
					{
						if (g_t.words[w].name == name) { ph.words[i] = w; }
					}
					ph.real[i] = Form<RE::TESWordOfPower>(p["wordForms"][i], "word of power");
				}
				const char* hosts[] = { "melee", "ranged", "armour" };
				for (int h = 0; h < 3; ++h)
				{
					ph.hosts[h] = ReadCell(p["hosts"][hosts[h]]);
					if (ph.hosts[h].effect) { g_t.phraseCell[ph.hosts[h].effect] = { pi, h }; }
				}
				if (p.contains("grant"))
				{
					ph.clone = Form<RE::TESShout>(p["grant"].value("shout", nlohmann::json()), "granted Shout");
					for (int i = 0; i < 3; ++i) { ph.cloneWords[i] = Form<RE::TESWordOfPower>(p["grant"]["words"][i], "granted word"); }
				}
				g_t.phrases.push_back(std::move(ph));
			}
		}
		catch (const std::exception& e) { fail(std::string("stones.json is not in the expected shape: ") + e.what()); return; }
		if (g_t.words.size() != 60 || g_t.phrases.size() != 20) { fail(fmt::format("stones.json holds {} words and {} phrases, not 60 and 20", g_t.words.size(), g_t.phrases.size())); return; }
		g_t.loaded = true;
		logger::info("soul stones: {} words x {} tiers, {} phrases, {} item tiers with slots; {} stone forms", g_t.words.size(), kTiers, g_t.phrases.size(), g_t.itemTierSlots.size(), g_t.stoneForm.size());
		// rule 30: ask the object - the knowledge flags of the first phrase, once
		for (auto* w : g_t.phrases[0].real)
		{
			if (w) { logger::info("soul stones: word {:08X} flags {:08X} (learned bit {}, unlocked bit {})", w->GetFormID(), w->formFlags, (w->formFlags & kWordLearned) != 0, (w->formFlags & kWordUnlocked) != 0); }
		}
		try
		{
			std::ifstream rin(std::filesystem::path("Data/SKSE/Plugins/NjordlingerCombatGameplayOverhaul/runesmith.json"));
			if (rin)
			{
				const auto r = nlohmann::json::parse(rin);
				auto* ref = Form<RE::TESObjectREFR>(r.value("npcRef", nlohmann::json()), "runesmith reference");
				RE::FormID infoId = 0;
				if (auto* file = handler->LookupModByName(kPlugin); file && r.contains("socketInfo"))
				{
					const auto local = static_cast<std::uint32_t>(std::stoul(r["socketInfo"].get<std::string>(), nullptr, 0));
					infoId = file->IsLight() ? (0xFE000000u | (static_cast<std::uint32_t>(file->GetSmallFileCompileIndex()) << 12) | (local & 0xFFFu))
					                         : ((static_cast<std::uint32_t>(file->GetCompileIndex()) << 24) | (local & 0xFFFFFFu));
				}
				g_rs.learned = Form<RE::TESGlobal>(r.value("learnedGlobal", nlohmann::json()), "runesmith global");
				g_rs.radius = r.value("serviceRadius", 1500.0F);
				if (auto* cell = Form<RE::TESForm>(r.value("homeCell", nlohmann::json()), "runesmith's inn")) { g_rs.homeCell = cell->GetFormID(); }
				if (r.contains("homePos") && r["homePos"].is_array() && r["homePos"].size() == 3)
				{
					g_rs.homePos = { r["homePos"][0].get<float>(), r["homePos"][1].get<float>(), r["homePos"][2].get<float>() };
				}
				g_rs.homeAngleZ = r.value("homeAngleZ", 0.0F);
				g_rs.npcRef = ref ? ref->GetFormID() : 0;
				g_rs.socketInfo = infoId;
				// one missing part never disables the rest: the quest's global, the move into the inn and the line each
				// work on their own (logged above when a form does not resolve)
				g_rs.loaded = ref != nullptr;
				if (!infoId) { logger::warn("soul stones: the runesmith's socketing line has no form ID - his window will not open from dialogue"); }
			}
		}
		catch (const std::exception& e) { logger::warn("soul stones: runesmith.json is not valid ({}) - no runesmith", e.what()); }
		logger::info("soul stones: runesmith {}", g_rs.loaded ? fmt::format("ref {:08X}, socketing line {:08X}", g_rs.npcRef, g_rs.socketInfo) : std::string("not loaded"));
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton())
		{
			holder->AddEventSink<RE::TESTopicInfoEvent>(&g_topic);
			holder->AddEventSink<RE::TESHitEvent>(&g_hit);
			holder->AddEventSink<RE::TESDeathEvent>(&g_death);
			holder->AddEventSink<RE::TESEquipEvent>(&g_equip);
		}
		if (auto* ui = RE::UI::GetSingleton()) { ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menu); }
		if (!g_running.exchange(true)) { std::thread(Worker).detach(); }
		std::scoped_lock l(g_lock);
		g_snapshot.loaded = true;
		g_snapshot.words = g_t.wordNames;
		g_snapshot.tiers = g_t.tierNames;
	}

	void OnGameLoaded()
	{
		if (!g_t.loaded) { return; }
		g_service = false;
		g_cooldowns.clear();
		g_streaks.clear();
		for (auto* w : g_t.phrases[0].real)
		{
			if (w) { logger::info("soul stones: at load, word {:08X} flags {:08X} (learned {}, unlocked {})", w->GetFormID(), w->formFlags, Learned(w), Unlocked(w)); }
		}
		Recompute(true);
		logger::info("soul stones: game loaded - {} socketed instance(s) in the table, {} equipped with stones", g_entries.size(), g_equipped.size());
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		if (!g_t.loaded && !g_heldRecord.empty())
		{
			const auto held = static_cast<std::uint32_t>(g_heldRecord.size());
			if (a_intfc && a_intfc->OpenRecord(kRecord, kRecordVersion)) { a_intfc->WriteRecordData(&held, sizeof(held)); a_intfc->WriteRecordData(g_heldRecord.data(), held); }
			logger::warn("soul stones: the table is not loaded - the co-save record was written back unchanged");
			return;
		}
		nlohmann::json arr = nlohmann::json::array();
		for (const auto& [key, e] : g_entries)
		{
			nlohmann::json st = nlohmann::json::array();
			for (const auto& s : e.stones)
			{
				if (s.word < 0) { st.push_back(nullptr); }
				else { st.push_back({ g_t.words[s.word].name, s.tier }); }
			}
			nlohmann::json orig = nlohmann::json::array();
			for (const auto& o : e.orig) { orig.push_back({ o.effect, o.magnitude, o.area, o.duration }); }
			arr.push_back({ { "base", e.base }, { "owner", e.owner }, { "uid", e.uid }, { "stones", st }, { "hadEnchant", e.hadEnchant }, { "orig", orig },
				{ "origCharge", e.origCharge }, { "origCost", e.origCost }, { "hadName", e.hadName }, { "name", e.origName } });
		}
		const std::string text = nlohmann::json{ { "entries", arr } }.dump();
		const auto size = static_cast<std::uint32_t>(text.size());
		if (!a_intfc || !a_intfc->OpenRecord(kRecord, kRecordVersion)) { logger::error("soul stones: could not open the co-save record; the socketed stones were not saved"); return; }
		if (!a_intfc->WriteRecordData(&size, sizeof(size)) || !a_intfc->WriteRecordData(text.data(), size)) { logger::error("soul stones: writing the co-save record failed"); return; }
		logger::info("soul stones: saved {} socketed instance(s)", g_entries.size());
	}

	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
	{
		g_entries.clear();
		if (!a_intfc || a_version != kRecordVersion || a_length < sizeof(std::uint32_t)) { logger::warn("soul stones: co-save record version {} not read", a_version); return; }
		std::uint32_t size = 0;
		if (!a_intfc->ReadRecordData(&size, sizeof(size)) || size != a_length - sizeof(size)) { logger::warn("soul stones: co-save record is short; no stones loaded"); return; }
		std::string text(size, '\0');
		if (size && !a_intfc->ReadRecordData(text.data(), size)) { logger::warn("soul stones: co-save record could not be read"); return; }
		g_heldRecord.clear();
		if (!g_t.loaded) { g_heldRecord = std::move(text); logger::warn("soul stones: the table is not loaded - the co-save record is held unchanged"); return; }
		try
		{
			const auto j = nlohmann::json::parse(text);
			for (const auto& v : j["entries"])
			{
				Entry e;
				RE::FormID base = v.value("base", 0u), owner = v.value("owner", 0u);
				if (!a_intfc->ResolveFormID(base, e.base)) { continue; }   // the item's plugin is gone
				if (owner && !a_intfc->ResolveFormID(owner, e.owner)) { e.owner = owner; }
				e.uid = v.value("uid", static_cast<std::uint16_t>(0));
				for (int i = 0; i < 3 && i < static_cast<int>(v["stones"].size()); ++i)
				{
					const auto& s = v["stones"][i];
					if (!s.is_array() || s.size() < 2) { continue; }
					const auto name = s[0].get<std::string>();
					for (int w = 0; w < static_cast<int>(g_t.words.size()); ++w)
					{
						if (g_t.words[w].name == name) { e.stones[i] = { w, std::clamp(s[1].get<int>(), 0, kTiers - 1) }; }
					}
				}
				e.hadEnchant = v.value("hadEnchant", false);
				for (const auto& o : v["orig"])
				{
					SavedEffect s;
					RE::FormID id = o[0].get<RE::FormID>();
					if (!a_intfc->ResolveFormID(id, s.effect)) { continue; }
					s.magnitude = o[1].get<float>();
					s.area = o[2].get<std::uint32_t>();
					s.duration = o[3].get<std::uint32_t>();
					e.orig.push_back(s);
				}
				e.origCharge = v.value("origCharge", static_cast<std::uint16_t>(0));
				e.origCost = v.value("origCost", -1);
				e.hadName = v.value("hadName", false);
				e.origName = v.value("name", std::string());
				g_entries[KeyOf(e.base, e.owner, e.uid)] = std::move(e);
			}
		}
		catch (const std::exception& ex) { logger::warn("soul stones: co-save record is not valid JSON ({}); no stones loaded", ex.what()); g_entries.clear(); return; }
		logger::info("soul stones: loaded {} socketed instance(s) from the co-save", g_entries.size());
	}

	void OnRevert()
	{
		g_entries.clear();
		g_heldRecord.clear();
		g_equipped.clear();
		g_service = false;
	}

	Snapshot GetSnapshot()
	{
		std::scoped_lock l(g_lock);
		return g_snapshot;
	}

	void RequestRefresh() { Queue([]() { BuildSnapshot(); }); }

	void RequestRecompose() { Queue([]() { Recompute(false); }); }

	void RequestSocket(const std::string& a_itemKey, int a_slot, int a_word, int a_tier)
	{
		Queue([key = a_itemKey, a_slot, a_word, a_tier]() { DoSocket(key, a_slot, a_word, a_tier, false); });
	}

	void RequestUnsocket(const std::string& a_itemKey, int a_slot)
	{
		Queue([key = a_itemKey, a_slot]() { DoSocket(key, a_slot, -1, -1, true); });
	}

	void SetServiceOpen(bool a_open)
	{
		if (!a_open) { Queue([]() { CloseService("closed"); BuildSnapshot(); }); return; }
		g_service = true;
		RequestRefresh();
	}

	void SetWindowOpener(std::function<void(bool)> a_fn) { g_windowOpener = std::move(a_fn); }

	bool IsServiceOpen() { return g_service.load(); }

	void RequestGiveForTest(int a_word, int a_tier, int a_count)
	{
		Queue([a_word, a_tier, a_count]() {
			auto* p = Player();
			if (!p || !g_t.loaded || a_word < 0 || a_word >= static_cast<int>(g_t.words.size()) || a_tier < 0 || a_tier >= kTiers) { SetResult("give: no such stone"); return; }
			auto* stone = g_t.words[a_word].stones[a_tier];
			auto* gem = g_t.tiers[a_tier].feeGems.empty() ? nullptr : g_t.tiers[a_tier].feeGems[0];
			if (!stone || !gem) { SetResult(fmt::format("give: FAILED - the {} form did not resolve at load (see the log)", !stone ? "stone" : "fee gem")); BuildSnapshot(); return; }
			p->AddObjectToContainer(stone, nullptr, a_count, nullptr);
			p->AddObjectToContainer(gem, nullptr, a_count, nullptr);
			SetResult(fmt::format("gave {} x {} ({}) and {} fee gem(s)", a_count, g_t.words[a_word].name, g_t.tierNames[a_tier], a_count));
			BuildSnapshot();
		});
	}

	const std::vector<std::string>& WordNames() { return g_t.wordNames; }
	const std::vector<std::string>& TierNames() { return g_t.tierNames; }
	int PhraseOfWord(int a_word) { return (a_word >= 0 && a_word < static_cast<int>(g_t.words.size())) ? g_t.words[a_word].phrase : -1; }
	std::string PhraseName(int a_phrase)
	{
		if (a_phrase < 0 || a_phrase >= static_cast<int>(g_t.phrases.size())) { return {}; }
		const auto& ph = g_t.phrases[a_phrase];
		return fmt::format("{} {} {}", g_t.words[ph.words[0]].name, g_t.words[ph.words[1]].name, g_t.words[ph.words[2]].name);
	}
	std::string CellText(int a_word, Host a_host, int a_tier)
	{
		if (a_word < 0 || a_word >= static_cast<int>(g_t.words.size()) || a_host == Host::kNone) { return {}; }
		const auto& c = g_t.words[a_word].hosts[static_cast<int>(a_host)];
		std::string text = c.text;
		const int t = std::clamp(a_tier, 0, kTiers - 1);
		auto fill = [&text](const char* a_tag, float a_v) {
			for (std::size_t at = text.find(a_tag); at != std::string::npos; at = text.find(a_tag))
			{
				text.replace(at, std::strlen(a_tag), fmt::format("{:g}", std::round(a_v * 100.0F) / 100.0F));
			}
		};
		fill("<mag>", c.kind == "self" ? c.value[t] : c.m[t]);
		fill("<dur>", c.dur[t]);
		return text;
	}

	std::string WordsJson()
	{
		nlohmann::json arr = nlohmann::json::array();
		for (const auto& ph : g_t.phrases)
		{
			for (int i = 0; i < 3; ++i)
			{
				auto* w = ph.real[i];
				arr.push_back({ { "word", ph.words[i] >= 0 ? g_t.words[ph.words[i]].name : std::string() }, { "shout", ph.shout },
					{ "form", w ? fmt::format("{:08X}", w->GetFormID()) : std::string() }, { "flags", w ? fmt::format("{:08X}", w->formFlags) : std::string() },
					{ "learned", Learned(w) }, { "unlocked", Unlocked(w) } });
			}
		}
		return arr.dump();
	}

	std::string StatusJson()
	{
		const auto s = GetSnapshot();
		nlohmann::json items = nlohmann::json::array();
		for (const auto& i : s.items)
		{
			nlohmann::json st = nlohmann::json::array();
			for (int k = 0; k < i.slots; ++k)
			{
				const auto& r = i.stones[k];
				st.push_back(r.word < 0 ? nlohmann::json(nullptr) : nlohmann::json{ { "word", s.words[r.word] }, { "tier", s.tiers[r.tier] } });
			}
			items.push_back({ { "key", i.key }, { "name", i.name }, { "host", static_cast<int>(i.host) }, { "slots", i.slots }, { "count", i.count },
				{ "equipped", i.equipped }, { "stones", st }, { "phrase", i.phrase >= 0 ? PhraseName(i.phrase) : "" }, { "phraseKnown", i.phraseKnown } });
		}
		nlohmann::json stones = nlohmann::json::array();
		for (const auto& v : s.stones) { stones.push_back({ { "word", s.words[v.word] }, { "wordIndex", v.word }, { "tier", s.tiers[v.tier] }, { "tierIndex", v.tier }, { "count", v.count } }); }
		nlohmann::json phrases = nlohmann::json::array();
		for (const auto& p : s.phrases) { phrases.push_back({ { "shout", p.shout }, { "learned", p.learned }, { "unlocked", p.unlocked }, { "granted", p.granted } }); }
		return nlohmann::json{ { "loaded", s.loaded }, { "error", s.error }, { "serviceOpen", s.serviceOpen }, { "tableEntries", s.tableEntries },
			{ "composes", s.composes }, { "procsFired", s.procsFired }, { "feeGems", s.feeGems }, { "active", s.activePerks }, { "lastResult", s.lastResult },
			{ "items", items }, { "stones", stones }, { "phrases", phrases }, { "revision", s.revision } }.dump();
	}
}
