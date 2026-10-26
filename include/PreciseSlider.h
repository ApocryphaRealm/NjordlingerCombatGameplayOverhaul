#pragma once

// ============================================================================================================
// A precise slider for a page drawn through SKSE Menu Framework's ImGuiMCP wrapper (rule 68, the owner 2026-09-29: "they
// don't jump more than one numerical unit per D-pad nudge"). Dear ImGui moves a keyboard / gamepad nudge by 1% of the
// slider's range whenever the format shows decimals, and also for a whole-number slider wider than 100 - so a 1..10000
// gold slider stepped 100 per press.
//
// The Oblivion Remastered reference (precise::SliderFloat on ImGui itself) tells a nudge apart by the active id's input
// source. A consumer of the framework cannot read ImGui's internal context through ImGuiMCP, so this one asks the exported
// input state instead: the change is a nudge when a nudge key is down (the arrow keys, the D-pad, or the left-stick nav
// keys - the Apocrypha Menu Framework feeds the RIGHT stick of a held slider into those) and the left mouse button is
// not. A mouse drag and a number typed into the slider (no arrow is down while typing) are left alone; held, the nudge
// repeats as ImGui repeats it, one unit at a time.
//
// 1.7.5 (test d844): every slider is drawn with ImGuiSliderFlags_AlwaysClamp, so a number typed into it (Ctrl+click) is
// clamped to the slider's range INSIDE ImGui. Without the flag ImGui 1.90.8 leaves a typed value unclamped, and while
// the text field is open it copies the typed text back over the value every frame the two differ - a caller that
// clamped the value itself (DEM's Scale against its runtime cap) got a 'changed' slider every frame, and the
// minimap was re-applied at frame rate until Skyrim died in Scaleform.
// ============================================================================================================

#include "SKSEMenuFramework.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace precise
{
	// one unit of the last digit the format shows ("%.2f" -> 0.01); 1 when there is no precision
	inline double Step(const char* a_format)
	{
		const char* p = a_format ? std::strstr(a_format, "%.") : nullptr;
		if (p && p[2] >= '0' && p[2] <= '9') {
			return std::pow(10.0, -(p[2] - '0'));
		}
		return 1.0;
	}

	inline bool Nudging()
	{
		using namespace ImGuiMCP;
		if (IsMouseDown(ImGuiMouseButton_Left)) {
			return false;
		}
		for (const ImGuiKey key : { ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
				 ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight }) {
			if (IsKeyDown(key)) {
				return true;
			}
		}
		return false;
	}

	inline bool SliderFloat(const char* a_label, float* a_v, float a_min, float a_max, const char* a_format)
	{
		const float before = *a_v;
		if (!ImGuiMCP::SliderFloat(a_label, a_v, a_min, a_max, a_format, ImGuiMCP::ImGuiSliderFlags_AlwaysClamp)) {
			return false;
		}
		*a_v = std::clamp(*a_v, a_min, a_max);   // the typed value stays inside the range whatever the framework's ImGui does
		if (*a_v != before && Nudging()) {
			const double step = Step(a_format);
			double v = static_cast<double>(before) + (*a_v > before ? step : -step);
			v = std::round(v / step) * step;   // on the grid the format shows
			*a_v = static_cast<float>(std::clamp(v, static_cast<double>(a_min), static_cast<double>(a_max)));
		}
		return true;
	}

	inline bool SliderInt(const char* a_label, int* a_v, int a_min, int a_max, const char* a_format = "%d")
	{
		const int before = *a_v;
		if (!ImGuiMCP::SliderInt(a_label, a_v, a_min, a_max, a_format, ImGuiMCP::ImGuiSliderFlags_AlwaysClamp)) {
			return false;
		}
		*a_v = std::clamp(*a_v, a_min, a_max);
		if (*a_v != before && Nudging()) {
			*a_v = std::clamp(before + (*a_v > before ? 1 : -1), a_min, a_max);
		}
		return true;
	}
}
