#include "PCH.h"

#include "Compat.h"

#include "utils/Logger.h"

#include <Windows.h>

namespace Compat
{
	namespace
	{
		std::vector<Detection> detections;
		bool cpc = false;
		bool plugin = false;

		bool DllLoaded(const char* a_dll) { return GetModuleHandleA(a_dll) != nullptr; }

		bool EspLoaded(std::string_view a_file)
		{
			auto* handler = RE::TESDataHandler::GetSingleton();
			return handler && handler->LookupModByName(a_file) != nullptr;
		}
	}

	void Detect()
	{
		detections.clear();
		cpc = DllLoaded("CharacterProgressionControl.dll");
		detections.push_back({ "Character Progression Control", cpc,
							   cpc ? "Loaded beside this mod. Both hook the same game code: whichever installs second refuses its patches. Disable Character Progression Control - this mod replaces it."
								   : "Not loaded (correct: this mod replaces it)." });
		plugin = EspLoaded("NjordlingerCombatGameplayOverhaul.esp");
		detections.push_back({ "NjordlingerCombatGameplayOverhaul.esp", plugin,
							   plugin ? "Loaded: the class keywords, globals and abilities are available."
									  : "Not loaded: classes still lock trees and crafting, but class abilities are missing. Enable the plugin." });
		for (const auto& d : detections)
		{
			if (d.present && d.name == "Character Progression Control") { logger::warn("compat: {} - {}", d.name, d.consequence); }
			else { logger::info("compat: {} present={} - {}", d.name, d.present, d.consequence); }
		}
	}

	const std::vector<Detection>& All() { return detections; }
	bool CharacterProgressionControlPresent() { return cpc; }
	bool PluginLoaded() { return plugin; }
}
