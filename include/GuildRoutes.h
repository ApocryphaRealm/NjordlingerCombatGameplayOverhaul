#pragma once

// Njordlinger Combat Gameplay Overhaul - guild and faction routes by class (PLAN.md D30; see source/GuildRoutes.cpp).

#include <string>
#include <vector>

namespace GuildRoutes
{
	struct Route
	{
		std::string id;
		std::string name;
		int group = -1;   // ClassData group index
	};

	void Install();           // kDataLoaded
	void Apply();             // main thread: NJCGO_RoutesOn follows the switches (Classes::Apply calls it)
	bool Active();            // the "Routes by class" switch and the class master switch
	std::vector<Route> Routes();
}
