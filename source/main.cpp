// Njordlinger Combat Gameplay Overhaul - GPL-3.0-or-later. Phase 2 of the plan (Njordlinger 1.0): the Progression
// module (copied and trimmed from Character Progression Control, ApocryphaRealm, GPL-3.0-or-later - see NOTICE.md),
// the Classes module, the class lock-outs, quest rewards, the race-menu class category and the settings page; phase 3
// (Njordlinger 1.1): the Gear module - set bonuses by piece count, unique effects, the unique tier's no-enchanting;
// phase 4 (Njordlinger 2.0): the Soul Stones module - stones by instance, composed enchantments, phrases, granted Shouts.
#include "PCH.h"

#include "ClassEffects.h"
#include "Classes.h"
#include "Compat.h"
#include "DevBenchTool.h"
#include "Gear.h"
#include "LevelUp.h"
#include "LevelUpGains.h"
#include "Lockouts.h"
#include "Patches.h"
#include "QuestRewards.h"
#include "RaceMenuClass.h"
#include "Settings.h"
#include "SkillExperience.h"
#include "StatsMenuClass.h"
#include "Skills.h"
#include "Shouting.h"
#include "CarryWeight.h"
#include "CardEnchantment.h"
#include "EffectFilter.h"
#include "GuildRoutes.h"
#include "RowTint.h"
#include "CharacterSheetFix.h"
#include "SoulStones.h"
#include "UI.h"

#include "utils/AddressLibraryGuard.h"
#include "utils/Logger.h"
#include "utils/Strings.h"

namespace
{
	void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type)
		{
		case SKSE::MessagingInterface::kPostLoad:
			DevBenchTool::Init(false);
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			strings::Configure("NjordlingerCombatGameplayOverhaul");
			Compat::Detect();
			// Every patch group registers first and installs in one pass; a group that cannot attach is a reported fact,
			// never a crash, and each attaches whatever its setting says (the hooks read their settings live).
			Skills::Register();
			SkillExperience::Register();
			LevelUp::Register();
			LevelUpGains::Register();
			Patches::InstallAll();
			Classes::Install();
			Lockouts::Install();
			ClassEffects::Install();
			Gear::Install();
			SoulStones::Install();
			CardEnchantment::Install();
			EffectFilter::Install();
			GuildRoutes::Install();
			RowTint::Install();
			CharacterSheetFix::Install();
			Shouting::Install();
			CarryWeight::Install();
			QuestRewards::Install();
			RaceMenuClass::Install();
			StatsMenuClass::Install();
			UI::Register();
			DevBenchTool::Init(true);
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			Classes::OnGameLoaded();
			Gear::OnGameLoaded();
			SoulStones::OnGameLoaded();
			Shouting::OnGameLoaded();
			CarryWeight::OnGameLoaded();
			break;
		default:
			break;
		}
	}

	void SaveCallback(SKSE::SerializationInterface* a_intfc)
	{
		Classes::OnSave(a_intfc);
		ClassEffects::OnSave(a_intfc);
		QuestRewards::OnSave(a_intfc);
		SoulStones::OnSave(a_intfc);
		Shouting::OnSave(a_intfc);
		CarryWeight::OnSave(a_intfc);
	}

	void LoadCallback(SKSE::SerializationInterface* a_intfc)
	{
		std::uint32_t type = 0, version = 0, length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length))
		{
			switch (type)
			{
			case Classes::kRecord: Classes::ReadRecord(a_intfc, version, length); break;
			case QuestRewards::kRecord: QuestRewards::ReadRecord(a_intfc, version, length); break;
			case ClassEffects::kRecord: ClassEffects::ReadRecord(a_intfc, version, length); break;
			case SoulStones::kRecord: SoulStones::ReadRecord(a_intfc, version, length); break;
			case Shouting::kRecord: Shouting::ReadRecord(a_intfc, version, length); break;
			case CarryWeight::kRecord: CarryWeight::ReadRecord(a_intfc, version, length); break;
			case 'NJEN': logger::info("enchanting: this save carries the withdrawn D28 Enchanting setting - ignored (DESIGN-RETHINK.md 14)"); break;
			default: logger::warn("co-save: unknown record {:08X} (version {}, {} bytes) skipped", type, version, length); break;
			}
		}
	}

	void RevertCallback(SKSE::SerializationInterface*)
	{
		Classes::OnRevert();
		ClassEffects::OnRevert();
		QuestRewards::OnRevert();
		SoulStones::OnRevert();
		Shouting::OnRevert();
		CarryWeight::OnRevert();
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::log::init("NjordlingerCombatGameplayOverhaul");

	// The Address Library guard runs BEFORE SKSE::Init, which opens the Address Library itself (gate rule
	// address-library-guard-runs-before-skse-init): a missing file gets a message naming it and the plugin loads inert.
	if (!AddressLibraryGuard::Guard("Njordlinger Combat Gameplay Overhaul"))
	{
		return true;
	}
	SKSE::Init(a_skse);

	// Trampoline space for the engine patches installed at kDataLoaded: each far branch or call takes 14 bytes; the
	// skill-improve and attribute-gain entry hooks also park their prologue thunks here.
	SKSE::AllocTrampoline(512);

	settings::Init("NjordlingerCombatGameplayOverhaul.ini");
	settings::ApplyLogLevel();

	logger::info("Njordlinger Combat Gameplay Overhaul {} loading", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."));

	SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);

	if (auto* serialization = SKSE::GetSerializationInterface())
	{
		serialization->SetUniqueID('NJCG');
		serialization->SetSaveCallback(SaveCallback);
		serialization->SetLoadCallback(LoadCallback);
		serialization->SetRevertCallback(RevertCallback);
	}

	return true;
}
