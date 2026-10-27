#include "PCH.h"

#include "Enchanting.h"

#include "Classes.h"
#include "Settings.h"

#include "utils/Logger.h"

#include <nlohmann/json.hpp>

#include <atomic>

namespace Enchanting
{
	namespace
	{
		constexpr std::uint32_t kRecordVersion = 1;

		std::atomic<bool> g_on{ true };
		std::atomic<bool> g_haveRecord{ false };
		std::atomic<float> g_capAt{ -1.0F };   // the skill's cap while Off, fixed when Off took hold
		RE::TESGlobal* g_global = nullptr;
	}

	RE::TESGlobal* TreeGlobal()
	{
		if (g_global) { return g_global; }
		auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESGlobal>();
		g_global = factory ? factory->Create() : nullptr;
		if (g_global) { g_global->value = 1.0F; logger::info("enchanting: the Enchanting tree's gate global was created at run time"); }
		else { logger::error("enchanting: no global could be created - the Enchanting tree follows the magic group only"); }
		return g_global;
	}

	bool IsOn() { return g_on.load(); }

	float CapWhileOff(float a_current)
	{
		if (g_on.load()) { g_capAt = -1.0F; return -1.0F; }
		float cap = g_capAt.load();
		if (cap < 0.0F)
		{
			cap = a_current;
			g_capAt = cap;
			logger::info("enchanting: Off - the Enchanting skill holds at {:.0f}", cap);
		}
		return cap;
	}

	void RequestSet(bool a_on)
	{
		g_haveRecord = true;
		if (g_on.exchange(a_on) == a_on) { return; }
		if (a_on) { g_capAt = -1.0F; }
		logger::info("enchanting: this save - Enchanting switched {}", a_on ? "On (class-gated)" : "Off (nobody enchants; stones drop)");
		Classes::RequestApply();
	}

	void OnGameLoaded()
	{
		if (!g_haveRecord.load()) { g_on = settings::enchanting::onDefault; }
		logger::info("enchanting: this save - Enchanting {}{}", g_on.load() ? "On" : "Off", g_capAt.load() >= 0.0F ? std::format(" (skill held at {:.0f})", g_capAt.load()) : std::string());
		Classes::RequestApply();
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		const std::string text = nlohmann::json{ { "on", g_on.load() }, { "capAt", g_capAt.load() } }.dump();
		const auto size = static_cast<std::uint32_t>(text.size());
		if (!a_intfc || !a_intfc->OpenRecord(kRecord, kRecordVersion)) { logger::error("enchanting: could not open the co-save record"); return; }
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
		try
		{
			const auto j = nlohmann::json::parse(text);
			g_on = j.value("on", settings::enchanting::onDefault);
			g_capAt = j.value("capAt", -1.0F);
			g_haveRecord = true;
		}
		catch (...) { logger::warn("enchanting: the co-save record is not valid - this save uses the default"); }
	}

	void OnRevert()
	{
		g_haveRecord = false;
		g_on = settings::enchanting::onDefault;
		g_capAt = -1.0F;
	}

	std::string StatusJson()
	{
		return nlohmann::json{ { "on", g_on.load() }, { "capAt", g_capAt.load() }, { "treeGlobal", g_global ? g_global->value : -1.0F } }.dump();
	}
}
