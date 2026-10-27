#include "PCH.h"

#include "CarryWeight.h"

#include "Settings.h"

#include "utils/Logger.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace CarryWeight
{
	namespace
	{
		constexpr std::uint32_t kRecordVersion = 1;

		std::atomic<bool> g_ready{ false };
		std::atomic<bool> g_running{ false };
		std::atomic<bool> g_aaFound{ false };
		std::atomic<bool> g_aaWritesCarry{ false };
		std::atomic<int> g_aaStamina{ 0 };
		std::atomic<float> g_applied{ 0.0F };
		std::atomic<float> g_raceBase{ 300.0F };
		std::atomic<float> g_now{ 0.0F };

		RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

		// The permanent modifier (Papyrus ModActorValue's), the same vfunc on both build lines.
		void ModPermanent(RE::ActorValueOwner* a_avo, float a_delta)
		{
#if RUNTIME_LINE == 17
			a_avo->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kCarryWeight, a_delta);
#else
			a_avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kCarryWeight, a_delta);
#endif
		}

		float RaceBase(RE::PlayerCharacter* a_p)
		{
			auto* base = a_p ? a_p->GetActorBase() : nullptr;
			auto* race = base ? base->race : nullptr;   // the character's own race, not a transformation's
			return race ? race->data.baseCarryWeight : 300.0F;
		}

		// Main thread: move our modifier to what the settings and the Stamina points want.
		void Apply()
		{
			auto* p = Player();
			auto* avo = p ? p->AsActorValueOwner() : nullptr;
			if (!g_ready.load() || !avo) { return; }
			const float race = RaceBase(p);
			g_raceBase = race;
			float want = 0.0F;
			if (settings::carry::enabled)
			{
				want = settings::carry::base - race;
				if (!g_aaWritesCarry.load()) { want += settings::carry::perStamina * static_cast<float>(g_aaStamina.load()); }
			}
			const float delta = want - g_applied.load();
			if (std::fabs(delta) > 0.001F)
			{
				ModPermanent(avo, delta);
				logger::info("carry weight: our modifier {:.1f} -> {:.1f} (start {:.0f} vs race {:.0f}; {} Stamina point(s) x {:.1f}{})", g_applied.load(), want,
					settings::carry::base, race, g_aaStamina.load(), settings::carry::perStamina, g_aaWritesCarry.load() ? " - Adaptive Attributes writes that part itself" : "");
				g_applied = want;
			}
			g_now = avo->GetActorValue(RE::ActorValue::kCarryWeight);
		}

		class BonusCallback : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			void operator()(RE::BSScript::Variable a_result) override
			{
				if (a_result.IsInt())
				{
					const int v = a_result.GetSInt();
					if (!g_aaFound.exchange(true)) { logger::info("carry weight: Adaptive Attributes answers - {} permanent Stamina point(s)", v); }
					g_aaStamina = std::max(0, v);
				}
				if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { Apply(); }); }
			}
			void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
		};

		void AskAdaptiveAttributes()
		{
			auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm) { return; }
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb(new BonusCallback());
			auto* args = RE::MakeFunctionArguments();
			if (!vm->DispatchStaticCall("AdaptiveAttributesSKSE", "GetBonusStamina", args, cb))
			{
				// not installed (no such script): only the starting value applies
				if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { Apply(); }); }
			}
		}

		void Worker()
		{
			while (g_running)
			{
				std::this_thread::sleep_for(std::chrono::seconds(2));
				if (!g_ready.load()) { continue; }
				if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { AskAdaptiveAttributes(); }); }
			}
		}

		bool ReadAaCarryOption()
		{
			std::ifstream in(std::filesystem::path("Data/SKSE/Plugins/AdaptiveAttributesSKSE.ini"));
			std::string line;
			while (in && std::getline(in, line))
			{
				const auto eq = line.find('=');
				if (eq == std::string::npos || line.find(';') < eq) { continue; }
				std::string key = line.substr(0, eq);
				key.erase(0, key.find_first_not_of(" \t"));
				key.erase(key.find_last_not_of(" \t") + 1);
				if (_stricmp(key.c_str(), "bEnableCarryWeightFromStaminaBonus") != 0) { continue; }
				std::string v = line.substr(eq + 1);
				v.erase(0, v.find_first_not_of(" \t"));
				return v.rfind("true", 0) == 0 || v.rfind("1", 0) == 0;
			}
			return false;
		}
	}

	void Install()
	{
		g_aaWritesCarry = ReadAaCarryOption();
		if (!g_running.exchange(true)) { std::thread(Worker).detach(); }
		logger::info("carry weight: one writer - Adaptive Attributes' own carry-weight option is {}", g_aaWritesCarry.load() ? "ON: it keeps the per-stamina part, this module only the starting value" : "off (or not installed): this module writes both parts");
	}

	void OnGameLoaded()
	{
		g_ready = true;
		if (auto* tasks = SKSE::GetTaskInterface()) { tasks->AddTask([]() { AskAdaptiveAttributes(); }); }
		logger::info("carry weight: this save carries our modifier {:.1f}", g_applied.load());
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		const std::string text = nlohmann::json{ { "applied", g_applied.load() } }.dump();
		const auto size = static_cast<std::uint32_t>(text.size());
		if (!a_intfc || !a_intfc->OpenRecord(kRecord, kRecordVersion)) { logger::error("carry weight: could not open the co-save record"); return; }
		a_intfc->WriteRecordData(&size, sizeof(size));
		a_intfc->WriteRecordData(text.data(), size);
	}

	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
	{
		if (!a_intfc || a_version != kRecordVersion || a_length < sizeof(std::uint32_t)) { return; }
		std::uint32_t size = 0;
		if (!a_intfc->ReadRecordData(&size, sizeof(size)) || size != a_length - sizeof(size)) { return; }
		std::string text(size, '\0');
		if (size && !a_intfc->ReadRecordData(text.data(), size)) { return; }
		try { g_applied = nlohmann::json::parse(text).value("applied", 0.0F); }
		catch (...) { logger::warn("carry weight: the co-save record is not valid - treated as nothing applied"); }
	}

	void OnRevert()
	{
		g_ready = false;
		g_applied = 0.0F;   // a save without our record carries no modifier of ours
		g_aaStamina = 0;
	}

	State GetState()
	{
		State s;
		s.ready = g_ready.load();
		s.aaFound = g_aaFound.load();
		s.aaWritesCarry = g_aaWritesCarry.load();
		s.aaStamina = g_aaStamina.load();
		s.raceBase = g_raceBase.load();
		s.applied = g_applied.load();
		s.carryWeight = g_now.load();
		return s;
	}

	std::string StatusJson()
	{
		const auto s = GetState();
		return nlohmann::json{ { "ready", s.ready }, { "aaFound", s.aaFound }, { "aaWritesCarry", s.aaWritesCarry }, { "aaStamina", s.aaStamina }, { "raceBase", s.raceBase },
			{ "applied", s.applied }, { "carryWeight", s.carryWeight }, { "base", settings::carry::base }, { "perStamina", settings::carry::perStamina }, { "enabled", settings::carry::enabled } }.dump();
	}
}
