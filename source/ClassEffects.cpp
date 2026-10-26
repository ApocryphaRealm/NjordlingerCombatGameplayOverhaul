#include "PCH.h"

#include "ClassEffects.h"

#include "ClassData.h"
#include "Settings.h"

#include "utils/Logger.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>

namespace ClassEffects
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr std::uint32_t kRecordVersion = 1;

		// Skyrim.esm forms (FormIDs verified in the master, 2026-10-26).
		constexpr RE::FormID kUndead = 0x00013796, kNPC = 0x00013794, kSilver = 0x0010AA1A, kMagicWard = 0x0001EA69;
		constexpr RE::FormID kUnarmed = 0x000001F4, kLockpick = 0x0000000A;
		constexpr RE::FormID kRested = 0x000FB981, kWellRested = 0x000FB984, kWerewolfImmunity = 0x000F5BA0;

		// ---- state: main thread only, except where an atomic or g_lock says otherwise ----------------------------
		std::mutex g_lock;                              // guards g_fired (written on the main thread, read by DevBench)
		std::map<std::string, std::uint32_t> g_fired;   // effect name -> times it fired this session (DevBench)
		// The co-saved values: atomics, because the co-save callbacks and DevBench read them off the main thread.
		std::atomic<float> g_bulwarkCooldown{ 0.0F };   // Crusader B3: seconds left
		std::atomic<float> g_restedUntil{ -1.0F };      // Harbinger: game hours when the Rested bonus we gave ends
		std::atomic<bool> g_finalDoseUsed{ false };     // Agent A3: used in this combat
		std::atomic<std::uint32_t> g_followersBuffedCount{ 0 };
		std::atomic<bool> g_undetected{ false };

		Clock::time_point g_winSpellThenHit{}, g_winHitThenSpell{};
		RE::ActorHandle g_monkTarget;
		int g_monkHits = 0;
		Clock::time_point g_monkLast{};
		float g_healthAtTick = -1.0F;
		bool g_wardBurst = false;                       // Battlemage A2: already burst for this ward
		std::int32_t g_lockpicksAtOpen = -1;
		std::vector<std::pair<RE::FormID, Clock::time_point>> g_pendingPotions;
		std::set<RE::ActiveEffect*> g_lengthened;
		std::vector<RE::TESFaction*> g_crimeFactions;
		std::unordered_map<RE::FormID, float> g_bountySeen;   // faction -> seconds a small bounty has stood out of combat
		float g_sprintVanilla = -1.0F;
		bool g_sprintApplied = false;
		std::set<RE::FormID> g_followersBuffed;
		int g_tickCount = 0;
		std::atomic<bool> g_running{ false };

		std::mt19937& Rng()
		{
			static std::mt19937 rng{ std::random_device{}() };
			return rng;
		}

		void Fired(const char* a_name)
		{
			std::scoped_lock l(g_lock);
			++g_fired[a_name];
		}

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

		bool Owns(RE::BGSPerk* a_perk)
		{
			auto* p = Player();
			return a_perk && p && p->HasPerk(a_perk);
		}

		bool OwnsRank(const char* a_class, int a_step, int a_ab)
		{
			const auto* n = ClassData::Find(a_class);
			return n && Owns(n->ranks[a_step][a_ab].perk);
		}
		bool OwnsAbility(const char* a_class)
		{
			const auto* n = ClassData::Find(a_class);
			return n && Owns(n->ability);
		}
		bool OwnsTitle(const char* a_title)
		{
			const auto* t = ClassData::FindTitle(a_title);
			return t && Owns(t->perk);
		}

		RE::SpellItem* HelperSpell(const char* a_name)
		{
			auto* f = ClassData::Helper(a_name);
			return f ? f->As<RE::SpellItem>() : nullptr;
		}

		void SetGlobal(const char* a_name, bool a_on)
		{
			auto* f = ClassData::Helper(a_name);
			if (auto* g = f ? f->As<RE::TESGlobal>() : nullptr) { g->value = a_on ? 1.0F : 0.0F; }
		}

		// Casts a helper spell from the player at a target (the player for a self effect). a_magnitude 0 = the record's.
		void Cast(const char* a_name, RE::TESObjectREFR* a_target, float a_magnitude = 0.0F)
		{
			auto* p = Player();
			auto* spell = HelperSpell(a_name);
			if (!p || !spell || !a_target)
			{
				static std::set<std::string> warned;
				if (!spell && warned.insert(a_name).second) { logger::warn("class effects: helper spell {} is missing - NjordlingerCombatGameplayOverhaul.esp not loaded?", a_name); }
				return;
			}
			auto* caster = p->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
			if (!caster) { return; }
			caster->CastSpellImmediate(spell, false, a_target, 1.0F, false, a_magnitude, p);
			Fired(a_name);
			logger::debug("class effects: {} cast on {:08X} (magnitude {:.1f})", a_name, a_target->GetFormID(), a_magnitude);
		}

		void Restore(RE::Actor* a_actor, RE::ActorValue a_av, float a_amount)
		{
			auto* avo = a_actor ? a_actor->AsActorValueOwner() : nullptr;
			if (!avo) { return; }
#if RUNTIME_LINE == 17
			if (a_amount >= 0.0F) { avo->RestoreActorValue(a_av, a_amount); } else { avo->DamageActorValue(a_av, -a_amount); }
#else
			avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, a_av, a_amount);
#endif
		}

		float HealthFraction(RE::Actor* a_actor)
		{
			auto* avo = a_actor ? a_actor->AsActorValueOwner() : nullptr;
			if (!avo) { return 1.0F; }
			const float max = avo->GetPermanentActorValue(RE::ActorValue::kHealth);
			return max > 0.0F ? avo->GetActorValue(RE::ActorValue::kHealth) / max : 1.0F;
		}

		bool HasKeywordID(RE::Actor* a_actor, RE::FormID a_keyword)
		{
			auto* kw = RE::TESForm::LookupByID<RE::BGSKeyword>(a_keyword);
			return a_actor && kw && a_actor->HasKeyword(kw);
		}

		bool HoldsSpell(RE::Actor* a_actor)
		{
			if (!a_actor) { return false; }
			for (bool left : { true, false })
			{
				auto* f = a_actor->GetEquippedObject(left);
				if (f && f->Is(RE::FormType::Spell)) { return true; }
			}
			return false;
		}

		bool Unarmed(RE::Actor* a_actor)
		{
			auto* r = a_actor ? a_actor->GetEquippedObject(false) : nullptr;
			auto* w = r ? r->As<RE::TESObjectWEAP>() : nullptr;
			return !r || (w && w->IsHandToHandMelee());
		}

		bool Sneaking(RE::Actor* a_actor) { return a_actor && a_actor->IsSneaking(); }

		bool WindowOpen(Clock::time_point a_until) { return Clock::now() < a_until; }

		// The spell in the hand the weapon is not in (the left hand while a weapon is in the right).
		RE::SpellItem* ReadiedSpell(RE::Actor* a_actor)
		{
			auto* l = a_actor ? a_actor->GetEquippedObject(true) : nullptr;
			return l ? l->As<RE::SpellItem>() : nullptr;
		}

		void CastReadied(RE::Actor* a_target, const char* a_what)
		{
			auto* p = Player();
			auto* spell = ReadiedSpell(p);
			if (!p || !spell || !a_target) { return; }
			if (spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf) { return; }
			if (auto* caster = p->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant))
			{
				caster->CastSpellImmediate(spell, false, a_target, 1.0F, false, 0.0F, p);
				Fired(a_what);
				logger::debug("class effects: {} cast the readied {:08X} on {:08X}", a_what, spell->GetFormID(), a_target->GetFormID());
			}
		}

		template <class F>
		void ForEachHighActor(F&& a_fn)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) { return; }
			for (auto& h : lists->highActorHandles)
			{
				auto a = h.get();
				if (a && !a->IsDead()) { a_fn(a.get()); }
			}
		}

		// ---- the hit ----------------------------------------------------------------------------------------------
		struct Hit
		{
			RE::ObjectRefHandle cause, target;
			RE::FormID source = 0, projectile = 0;
			bool power = false, sneak = false, bash = false, blocked = false;
		};

		void HandleHit(const Hit& a_h)
		{
			auto* p = Player();
			if (!p) { return; }
			auto causeRef = a_h.cause.get();
			auto targetRef = a_h.target.get();
			auto* cause = causeRef ? causeRef->As<RE::Actor>() : nullptr;
			auto* target = targetRef ? targetRef->As<RE::Actor>() : nullptr;
			auto* sourceForm = a_h.source ? RE::TESForm::LookupByID(a_h.source) : nullptr;
			auto* weap = sourceForm ? sourceForm->As<RE::TESObjectWEAP>() : nullptr;
			const bool isSpell = sourceForm && (sourceForm->Is(RE::FormType::Spell) || sourceForm->Is(RE::FormType::Scroll));
			const bool unarmed = a_h.source == kUnarmed || (weap && weap->IsHandToHandMelee());

			if (cause == p && target && target != p)
			{
				if (weap || unarmed) { g_winHitThenSpell = Clock::now() + std::chrono::seconds(3); }
				if (weap && !unarmed && OwnsRank("spellsword", 0, 0)) { Restore(p, RE::ActorValue::kMagicka, 5.0F); Fired("spellsword A1 siphon"); }
				if (a_h.bash && HoldsSpell(target) && OwnsRank("crusader", 1, 0)) { Cast("silence", target); }
				if (a_h.power && target->AsActorState()->actorState2.staggered && OwnsRank("rogue", 1, 0)) { Cast("knockdown", target); }
				if (a_h.power && weap && OwnsRank("spellsword", 2, 0)) { CastReadied(target, "spellsword A3 spellstrike"); }
				if (a_h.power && weap && OwnsRank("battlemage", 2, 0)) { CastReadied(target, "battlemage A3 arcane strike"); }
				if (a_h.projectile && weap && weap->IsBow() && OwnsRank("scout", 2, 0))
				{
					// the nearest hostile within 600 units of the target, farther from the player than the target
					RE::Actor* best = nullptr;
					float bestDist = 600.0F;
					const float targetFromPlayer = p->GetPosition().GetDistance(target->GetPosition());
					ForEachHighActor([&](RE::Actor* a) {
						if (a == target || a == p || a->IsPlayerTeammate() || !a->IsHostileToActor(p)) { return; }
						const float d = a->GetPosition().GetDistance(target->GetPosition());
						if (d < bestDist && p->GetPosition().GetDistance(a->GetPosition()) > targetFromPlayer) { best = a; bestDist = d; }
					});
					if (best) { Cast("pierce", best, static_cast<float>(weap->GetAttackDamage())); }
				}
				bool poisoned = false;
				if (weap && !unarmed)
				{
					auto* entry = p->GetEquippedEntryData(false);
					poisoned = entry && entry->IsPoisoned();
				}
				if (weap && HasKeywordID(target, kUndead) && OwnsRank("witchhunter", 1, 0))
				{
					auto* silver = RE::TESForm::LookupByID<RE::BGSKeyword>(kSilver);
					if (poisoned || (silver && weap->HasKeyword(silver))) { Cast("slow", target); }
				}
				if (unarmed && OwnsRank("monk", 1, 0))
				{
					const auto now = Clock::now();
					if (g_monkTarget != target->GetHandle() || now - g_monkLast > std::chrono::seconds(3)) { g_monkTarget = target->GetHandle(); g_monkHits = 0; }
					g_monkLast = now;
					if (++g_monkHits >= 3) { Cast("stun", target); g_monkHits = 0; }
				}
				if (unarmed && HoldsSpell(target) && OwnsRank("monk", 2, 0)) { Cast("manadrain", target); }
				if (a_h.sneak && poisoned && !g_finalDoseUsed && OwnsRank("agent", 2, 0) && HasKeywordID(target, kNPC))
				{
					auto* base = target->GetActorBase();
					if (base && !base->IsUnique() && !target->IsEssential())
					{
						Restore(target, RE::ActorValue::kHealth, -100000.0F);
						g_finalDoseUsed = true;
						Fired("agent A3 final dose");
						logger::info("class effects: Final Dose - poisoned sneak attack killed {:08X}", target->GetFormID());
					}
				}
			}

			if (target == p)
			{
				if (isSpell && a_h.blocked && OwnsRank("crusader", 1, 1)) { Restore(p, RE::ActorValue::kMagicka, 10.0F); Fired("crusader B2 spell catcher"); }
				if (a_h.blocked && cause && Unarmed(p) && OwnsRank("monk", 0, 1)) { Cast("stagger", cause); }
				if (HealthFraction(p) < 0.3F && g_bulwarkCooldown <= 0.0F && OwnsRank("crusader", 2, 1))
				{
					Cast("ward", p);
					g_bulwarkCooldown = 60.0F;
				}
				// Battlemage A2: a ward held and its strength gone -> the burst, once per ward
				bool wardActive = false;
				if (auto* mt = p->AsMagicTarget())
				{
					if (auto* list = mt->GetActiveEffectList())
					{
						for (auto* ae : *list)
						{
							auto* base = ae ? ae->GetBaseObject() : nullptr;
							if (base && base->HasKeywordID(kMagicWard)) { wardActive = true; break; }
						}
					}
				}
				if (!wardActive) { g_wardBurst = false; }
				else if (!g_wardBurst && p->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWardPower) <= 0.0F && OwnsRank("battlemage", 1, 0))
				{
					g_wardBurst = true;
					ForEachHighActor([&](RE::Actor* a) {
						if (a != p && !a->IsPlayerTeammate() && a->IsHostileToActor(p) && a->GetPosition().GetDistance(p->GetPosition()) < 400.0F) { Cast("burst", a); }
					});
				}
				// Rogue B2 (approximation): a hit that did no damage - health no lower than at the last tick, and still not
				// lower one task later - restores 15 stamina
				if (OwnsRank("rogue", 1, 1))
				{
					const float now = p->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
					if (g_healthAtTick >= 0.0F && now >= g_healthAtTick - 0.01F)
					{
						if (auto* tasks = SKSE::GetTaskInterface())
						{
							tasks->AddTask([now]() {
								auto* pl = Player();
								if (pl && pl->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) >= now - 0.01F)
								{
									Restore(pl, RE::ActorValue::kStamina, 15.0F);
									Fired("rogue B2 second wind");
								}
							});
						}
					}
				}
			}
		}

		class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_e, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!a_e || !settings::classes::enabled) { return RE::BSEventNotifyControl::kContinue; }
				auto* p = Player();
				if (!p || (a_e->cause.get() != p && a_e->target.get() != p)) { return RE::BSEventNotifyControl::kContinue; }
				Hit h;
				h.cause = a_e->cause ? a_e->cause->GetHandle() : RE::ObjectRefHandle{};
				h.target = a_e->target ? a_e->target->GetHandle() : RE::ObjectRefHandle{};
				h.source = a_e->source;
				h.projectile = a_e->projectile;
				using F = RE::TESHitEvent::Flag;
				h.power = a_e->flags.any(F::kPowerAttack);
				h.sneak = a_e->flags.any(F::kSneakAttack);
				h.bash = a_e->flags.any(F::kBashAttack);
				h.blocked = a_e->flags.any(F::kHitBlocked);
				if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([h]() { HandleHit(h); }); }
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// ---- death, cast, effect, equip, sleep, menus ------------------------------------------------------------
		class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_e, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				auto* p = Player();
				if (!a_e || !a_e->dead || !p || a_e->actorKiller.get() != p || !settings::classes::enabled) { return RE::BSEventNotifyControl::kContinue; }
				const auto victim = a_e->actorDying ? a_e->actorDying->GetHandle() : RE::ObjectRefHandle{};
				if (auto* tasks = SKSE::GetTaskInterface())
				{
					tasks->AddTask([victim]() {
						auto* pl = Player();
						auto vref = victim.get();
						auto* v = vref ? vref->As<RE::Actor>() : nullptr;
						if (!pl) { return; }
						if (OwnsRank("crusader", 2, 0))
						{
							ForEachHighActor([&](RE::Actor* a) {
								if (a->IsPlayerTeammate() && a->GetPosition().GetDistance(pl->GetPosition()) < 1500.0F)
								{
									Restore(a, RE::ActorValue::kHealth, a->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth) * 0.2F);
									Fired("crusader A3 light of the divines");
								}
							});
						}
						const bool hidden = g_undetected.load();
						if (hidden && OwnsRank("nightblade", 1, 0)) { Cast("invisible", pl); }
						if (Sneaking(pl) && OwnsRank("nightblade", 2, 0)) { Cast("ghost", pl); }
						if (v && HasKeywordID(v, kUndead) && OwnsRank("witchhunter", 2, 0))
						{
							Restore(pl, RE::ActorValue::kHealth, 25.0F);
							Restore(pl, RE::ActorValue::kStamina, 25.0F);
							Fired("witchhunter A3 hunter's reward");
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class CastSink : public RE::BSTEventSink<RE::TESSpellCastEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_e, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (a_e && a_e->object && a_e->object.get() == Player())
				{
					if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { g_winSpellThenHit = Clock::now() + std::chrono::seconds(3); }); }
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class EffectSink : public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* a_e, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override
			{
				auto* p = Player();
				if (!a_e || !p || a_e->caster.get() != p || !a_e->target || a_e->target.get() == p) { return RE::BSEventNotifyControl::kContinue; }
				const auto target = a_e->target->GetHandle();
				const auto effectId = a_e->magicEffect;
				if (auto* tasks = SKSE::GetTaskInterface())
				{
					tasks->AddTask([target, effectId]() {
						auto ref = target.get();
						auto* t = ref ? ref->As<RE::Actor>() : nullptr;
						auto* mgef = RE::TESForm::LookupByID<RE::EffectSetting>(effectId);
						if (!t || !mgef) { return; }
						using A = RE::EffectArchetypes::ArchetypeID;
						const auto arch = mgef->GetArchetype();
						if ((arch == A::kFrenzy || arch == A::kDemoralize) && OwnsRank("nightblade", 1, 1)) { Cast("silence", t); }
						// Agent A2: a poison taking effect = a detrimental contact effect from the player while the weapon is
						// poisoned (the game names no poison in this event)
						if (OwnsRank("agent", 1, 0) && mgef->IsDetrimental() && mgef->data.delivery == RE::MagicSystem::Delivery::kTouch)
						{
							auto* entry = Player()->GetEquippedEntryData(false);
							if (entry && entry->IsPoisoned()) { Cast("calm", t); }
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_e, RE::BSTEventSource<RE::TESEquipEvent>*) override
			{
				if (!a_e || !a_e->equipped || !a_e->actor || a_e->actor.get() != Player()) { return RE::BSEventNotifyControl::kContinue; }
				const auto id = a_e->baseObject;
				if (auto* tasks = SKSE::GetTaskInterface())
				{
					tasks->AddTask([id]() {
						auto* alch = RE::TESForm::LookupByID<RE::AlchemyItem>(id);
						auto* pl = Player();
						if (!alch || !pl || alch->IsPoison() || alch->IsFood()) { return; }
						if (OwnsAbility("witchhunter")) { g_pendingPotions.emplace_back(id, Clock::now()); }
						if (OwnsRank("witchhunter", 1, 1))
						{
							for (auto* sp : pl->GetActorRuntimeData().addedSpells)
							{
								if (sp && sp->GetSpellType() == RE::MagicSystem::SpellType::kDisease)
								{
									pl->RemoveSpell(sp);
									Fired("witchhunter B2 purifying draught");
									logger::info("class effects: the potion cured {:08X}", sp->GetFormID());
									break;
								}
							}
						}
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class SleepSink : public RE::BSTEventSink<RE::TESSleepStopEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStopEvent*, RE::BSTEventSource<RE::TESSleepStopEvent>*) override
			{
				if (auto* tasks = SKSE::GetTaskInterface())
				{
					tasks->AddTask([]() {
						auto* pl = Player();
						auto* cal = RE::Calendar::GetSingleton();
						auto* beast = RE::TESForm::LookupByID<RE::SpellItem>(kWerewolfImmunity);
						auto* rested = RE::TESForm::LookupByID<RE::SpellItem>(kRested);
						auto* well = RE::TESForm::LookupByID<RE::SpellItem>(kWellRested);
						if (!pl || !cal || !beast || !rested || !OwnsTitle("harbinger") || !pl->HasSpell(beast)) { return; }
						if (pl->HasSpell(rested) || (well && pl->HasSpell(well))) { return; }
						pl->AddSpell(rested);
						g_restedUntil = cal->GetHoursPassed() + 8.0F;
						Fired("harbinger rested");
						logger::info("class effects: beast blood slept off - Rested until game hour {:.1f}", g_restedUntil.load());
					});
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		std::int32_t LockpickCount()
		{
			auto* pl = Player();
			auto* pick = RE::TESForm::LookupByID<RE::TESBoundObject>(kLockpick);
			if (!pl || !pick) { return -1; }
			const auto counts = pl->GetInventoryCounts([pick](RE::TESBoundObject& a_obj) { return &a_obj == pick; });
			const auto it = counts.find(pick);
			return it == counts.end() ? 0 : it->second;
		}

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_e || !settings::classes::enabled) { return RE::BSEventNotifyControl::kContinue; }
				const bool opening = a_e->opening;
				if (a_e->menuName == RE::LockpickingMenu::MENU_NAME)
				{
					if (auto* tasks = SKSE::GetTaskInterface())
					{
						tasks->AddTask([opening]() {
							if (!OwnsTitle("nightingale")) { g_lockpicksAtOpen = -1; return; }
							if (opening) { g_lockpicksAtOpen = LockpickCount(); return; }
							const auto now = LockpickCount();
							auto* pick = RE::TESForm::LookupByID<RE::TESBoundObject>(kLockpick);
							if (g_lockpicksAtOpen > 0 && now >= 0 && now < g_lockpicksAtOpen && pick)
							{
								// a lock opened with a key or a spent pick in a door the lock took is not ours to know: give back
								// what broke while the menu was open
								Player()->AddObjectToContainer(pick, nullptr, g_lockpicksAtOpen - now, nullptr);
								Fired("nightingale lockpicks");
								logger::info("class effects: {} broken lockpick(s) given back", g_lockpicksAtOpen - now);
							}
							g_lockpicksAtOpen = -1;
						});
					}
				}
				else if (opening && a_e->menuName == RE::DialogueMenu::MENU_NAME)
				{
					if (auto* tasks = SKSE::GetTaskInterface())
					{
						tasks->AddTask([]() {
							auto* pl = Player();
							if (!pl) { return; }
							const int parts = (OwnsAbility("agent") ? 1 : 0) + (OwnsRank("agent", 0, 1) ? 1 : 0);
							if (parts == 0) { return; }
							const float speech = pl->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeech);
							Cast("speech", pl, std::max(1.0F, std::round(speech * 0.2F * static_cast<float>(parts))));
						});
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		HitSink g_hit;
		DeathSink g_death;
		CastSink g_cast;
		EffectSink g_effect;
		EquipSink g_equip;
		SleepSink g_sleep;
		MenuSink g_menu;

		// ---- the half-second tick (main thread) --------------------------------------------------------------------
		void Tick()
		{
			auto* pl = Player();
			if (!pl || !pl->Is3DLoaded() || !pl->GetParentCell()) { return; }
			++g_tickCount;
			constexpr float kDt = 0.5F;
			g_healthAtTick = pl->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			SetGlobal("NJCGO_Win_SpellThenHit", WindowOpen(g_winSpellThenHit));
			SetGlobal("NJCGO_Win_HitThenSpell", WindowOpen(g_winHitThenSpell));
			if (const float cd = g_bulwarkCooldown.load(); cd > 0.0F) { g_bulwarkCooldown = std::max(0.0F, cd - kDt); }

			// undetected: sneaking, and no living actor near enough to matter detects the player
			bool undetected = false;
			if (Sneaking(pl))
			{
				undetected = true;
				int checked = 0;
				ForEachHighActor([&](RE::Actor* a) {
					if (!undetected || a == pl || a->IsPlayerTeammate() || checked >= 30) { return; }
					if (a->GetPosition().GetDistance(pl->GetPosition()) > 4096.0F) { return; }
					++checked;
					if (a->RequestDetectionLevel(pl) > 0) { undetected = false; }
				});
			}
			SetGlobal("NJCGO_PlayerUndetected", undetected);
			g_undetected = undetected;

			if (!pl->IsInCombat()) { g_finalDoseUsed = false; }

			// detect life: every second while sprinting (Scout B3) or sneaking (Listener)
			if (g_tickCount % 2 == 0)
			{
				const bool sprinting = pl->AsActorState()->IsSprinting();
				if ((sprinting && OwnsRank("scout", 2, 1)) || (Sneaking(pl) && OwnsTitle("listener"))) { Cast("detectlife", pl); }
			}

			// Witchhunter: potions drunk in the last two seconds last 50% longer
			if (!g_pendingPotions.empty())
			{
				const auto now = Clock::now();
				if (auto* list = pl->AsMagicTarget()->GetActiveEffectList())
				{
					for (auto* ae : *list)
					{
						if (!ae || !ae->spell || g_lengthened.contains(ae) || ae->duration <= 0.0F) { continue; }
						for (const auto& [id, when] : g_pendingPotions)
						{
							if (ae->spell->GetFormID() == id)
							{
								ae->duration *= 1.5F;
								g_lengthened.insert(ae);
								Fired("witchhunter potion duration");
							}
						}
					}
				}
				std::erase_if(g_pendingPotions, [now](const auto& a_p) { return now - a_p.second > std::chrono::seconds(2); });
			}
			if (g_tickCount % 1200 == 0) { g_lengthened.clear(); }   // the pointers only guard one lengthening per effect

			// every five seconds: followers, bounties, the sprint setting, the Rested bonus's end
			if (g_tickCount % 10 != 0) { return; }
			auto* follow = HelperSpell("followerdamage");
			const bool harbinger = OwnsTitle("harbinger");
			if (follow)
			{
				std::set<RE::FormID> now;
				ForEachHighActor([&](RE::Actor* a) {
					if (a == pl) { return; }
					const bool want = harbinger && a->IsPlayerTeammate();
					const bool has = a->HasSpell(follow);
					if (want && !has) { a->AddSpell(follow); Fired("harbinger follower ability"); }
					else if (!want && has) { a->RemoveSpell(follow); }
					if (want) { now.insert(a->GetFormID()); }
				});
				g_followersBuffedCount = static_cast<std::uint32_t>(now.size());
				g_followersBuffed = std::move(now);
			}
			if (OwnsRank("agent", 2, 1))
			{
				const bool calm = !pl->IsInCombat();
				for (auto* f : g_crimeFactions)
				{
					const auto gold = f->GetCrimeGold();
					if (gold <= 0 || gold >= 500 || !calm) { g_bountySeen.erase(f->GetFormID()); continue; }
					auto& seen = g_bountySeen[f->GetFormID()];
					seen += 5.0F;
					if (seen >= 30.0F)
					{
						f->SetCrimeGold(0);
						f->SetCrimeGoldViolent(0);
						g_bountySeen.erase(f->GetFormID());
						Fired("agent B3 pardon");
						logger::info("class effects: a bounty of {} in {:08X} was forgiven", gold, f->GetFormID());
					}
				}
			}
			if (auto* gs = RE::GameSettingCollection::GetSingleton())
			{
				if (auto* drain = gs->GetSetting("fSprintStaminaDrainMult"))
				{
					if (g_sprintVanilla < 0.0F) { g_sprintVanilla = drain->data.f; }
					const bool want = OwnsRank("scout", 0, 1);
					if (want != g_sprintApplied)
					{
						drain->data.f = want ? g_sprintVanilla * 0.75F : g_sprintVanilla;
						g_sprintApplied = want;
						logger::info("class effects: fSprintStaminaDrainMult {} ({:.3f})", want ? "lowered for Long Legs" : "restored", drain->data.f);
					}
				}
			}
			if (g_restedUntil.load() >= 0.0F)
			{
				auto* cal = RE::Calendar::GetSingleton();
				auto* rested = RE::TESForm::LookupByID<RE::SpellItem>(kRested);
				if (cal && rested && cal->GetHoursPassed() >= g_restedUntil.load())
				{
					pl->RemoveSpell(rested);
					g_restedUntil = -1.0F;
					logger::info("class effects: the Rested bonus from beast-blood sleep ended");
				}
			}
		}

		void Worker()
		{
			while (g_running.load())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				if (!settings::classes::enabled) { continue; }
				if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) { continue; }
				if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { Tick(); }); }
			}
		}
	}

	void Install()
	{
		auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
		if (!holder) { logger::error("class effects: no event source holder - the event-driven effects are off"); return; }
		holder->AddEventSink<RE::TESHitEvent>(&g_hit);
		holder->AddEventSink<RE::TESDeathEvent>(&g_death);
		holder->AddEventSink<RE::TESSpellCastEvent>(&g_cast);
		holder->AddEventSink<RE::TESMagicEffectApplyEvent>(&g_effect);
		holder->AddEventSink<RE::TESEquipEvent>(&g_equip);
		holder->AddEventSink<RE::TESSleepStopEvent>(&g_sleep);
		if (auto* ui = RE::UI::GetSingleton()) { ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menu); }
		if (auto* handler = RE::TESDataHandler::GetSingleton())
		{
			for (auto* f : handler->GetFormArray<RE::TESFaction>())
			{
				if (f && (static_cast<std::uint32_t>(f->data.flags) & static_cast<std::uint32_t>(RE::FACTION_DATA::Flag::kTrackCrime)) != 0) { g_crimeFactions.push_back(f); }
			}
		}
		g_running = true;
		std::thread(Worker).detach();
		logger::info("class effects: 7 event sinks registered, {} crime factions watched, tick running", g_crimeFactions.size());
	}

	std::string StatusJson()
	{
		nlohmann::json j;
		std::scoped_lock l(g_lock);
		j["fired"] = g_fired;
		j["bulwarkCooldown"] = g_bulwarkCooldown.load();
		j["restedUntil"] = g_restedUntil.load();
		j["finalDoseUsed"] = g_finalDoseUsed.load();
		j["undetected"] = g_undetected.load();
		j["followersBuffed"] = g_followersBuffedCount.load();
		return j.dump();
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		nlohmann::json j;
		{
			std::scoped_lock l(g_lock);
			j["bulwarkCooldown"] = g_bulwarkCooldown.load();
			j["restedUntil"] = g_restedUntil.load();
			j["finalDoseUsed"] = g_finalDoseUsed.load();
		}
		const std::string text = j.dump();
		const auto size = static_cast<std::uint32_t>(text.size());
		if (!a_intfc || !a_intfc->OpenRecord(kRecord, kRecordVersion)) { logger::error("class effects: could not open the co-save record"); return; }
		a_intfc->WriteRecordData(&size, sizeof(size));
		a_intfc->WriteRecordData(text.data(), size);
	}

	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
	{
		OnRevert();
		std::uint32_t size = 0;
		if (!a_intfc || a_version != kRecordVersion || a_length < sizeof(size) || !a_intfc->ReadRecordData(&size, sizeof(size)) || size != a_length - sizeof(size))
		{
			logger::warn("class effects: co-save record not understood; cooldowns start fresh");
			return;
		}
		std::string text(size, '\0');
		if (size && !a_intfc->ReadRecordData(text.data(), size)) { return; }
		try
		{
			const auto j = nlohmann::json::parse(text);
			std::scoped_lock l(g_lock);
			g_bulwarkCooldown = j.value("bulwarkCooldown", 0.0F);
			g_restedUntil = j.value("restedUntil", -1.0F);
			g_finalDoseUsed = j.value("finalDoseUsed", false);
		}
		catch (...) { logger::warn("class effects: co-save record does not parse; cooldowns start fresh"); }
	}

	void OnRevert()
	{
		std::scoped_lock l(g_lock);
		g_bulwarkCooldown = 0.0F;
		g_restedUntil = -1.0F;
		g_finalDoseUsed = false;
	}
}
