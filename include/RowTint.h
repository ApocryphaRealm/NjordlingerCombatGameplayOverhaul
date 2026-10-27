#pragma once

// Njordlinger Combat Gameplay Overhaul - inventory rows tinted by item tier (see source/RowTint.cpp).

#include <cstdint>

namespace RowTint
{
	void Install();   // kDataLoaded
	std::uint32_t Bars();
}
