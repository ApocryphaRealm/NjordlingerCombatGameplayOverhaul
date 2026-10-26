#pragma once

namespace DevBenchTool
{
	// Registers "njcgo.control" with DevBench when present. Call with false at kPostLoad and
	// true at kDataLoaded.
	void Init(bool a_lastAttempt);
}
