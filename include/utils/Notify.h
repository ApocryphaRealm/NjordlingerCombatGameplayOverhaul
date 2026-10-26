#pragma once

// A corner notification on both build lines (CommonLibSSE-NG 7.x has no RE::DebugNotification; CPC's pattern).
namespace notify
{
	inline void Show(const char* a_text)
	{
		if (!a_text || !*a_text) { return; }
#if RUNTIME_LINE == 17
		RE::SendHUDMessage::ShowHUDMessage(a_text, nullptr, true);
#else
		RE::DebugNotification(a_text);
#endif
	}
}
