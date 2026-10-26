#include "PCH.h"

#include "Signature.h"

#include "utils/Logger.h"

#include <format>

namespace Signature
{
	namespace
	{
		constexpr int kWildcard = -1;

		std::vector<int> Parse(const std::string& a_pattern, bool& a_ok)
		{
			std::vector<int> out;
			a_ok = true;
			std::size_t i = 0;
			while (i < a_pattern.size())
			{
				while (i < a_pattern.size() && a_pattern[i] == ' ') { ++i; }
				if (i >= a_pattern.size()) { break; }

				if (a_pattern[i] == '?')
				{
					out.push_back(kWildcard);
					++i;
					if (i < a_pattern.size() && a_pattern[i] == '?') { ++i; }
					continue;
				}

				int value = 0, digits = 0;
				while (i < a_pattern.size() && digits < 2)
				{
					const char c = a_pattern[i];
					int d;
					if (c >= '0' && c <= '9') { d = c - '0'; }
					else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
					else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
					else { break; }
					value = value * 16 + d;
					++i;
					++digits;
				}
				if (digits == 0) { a_ok = false; return out; }
				out.push_back(value);
			}
			return out;
		}
	}

	bool ModuleRange(std::uintptr_t& a_base, std::size_t& a_size)
	{
		// The game module as the loader mapped it - by this point the Steam stub has decrypted
		// .text, so what is in memory is the real code.
		const auto& module = REL::Module::get();
		const auto section = module.segment(REL::Segment::textx);
		if (section.address() == 0 || section.size() == 0) { return false; }
		a_base = section.address();
		a_size = section.size();
		return true;
	}

	Result Find(const std::string& a_pattern, std::ptrdiff_t a_offset)
	{
		Result result;

		bool parsed = false;
		const auto pattern = Parse(a_pattern, parsed);
		if (!parsed || pattern.empty())
		{
			result.note = "the signature could not be read";
			logger::error("signature \"{}\" is malformed", a_pattern);
			return result;
		}

		std::uintptr_t base = 0;
		std::size_t size = 0;
		if (!ModuleRange(base, size))
		{
			result.note = "the game's executable section could not be located";
			return result;
		}

		const auto* bytes = reinterpret_cast<const std::uint8_t*>(base);
		const std::size_t last = size - pattern.size();

		std::uintptr_t first = 0;
		// Every match is counted, not just the first: an ambiguous signature must be refused, and
		// that cannot be known by stopping at the first hit. The count stops at a handful because
		// two is already too many.
		for (std::size_t i = 0; i <= last && result.matches < 4; ++i)
		{
			bool hit = true;
			for (std::size_t j = 0; j < pattern.size(); ++j)
			{
				const int p = pattern[j];
				if (p != kWildcard && bytes[i + j] != static_cast<std::uint8_t>(p)) { hit = false; break; }
			}
			if (!hit) { continue; }
			if (result.matches == 0) { first = base + i; }
			++result.matches;
		}

		if (result.matches == 0)
		{
			result.note = "this signature is not present in this build of the game, so the patch "
						  "is off and that part of the game is untouched";
			logger::warn("signature not found: {}", a_pattern);
			return result;
		}
		if (result.matches > 1)
		{
			result.note = std::format("this signature matches in {} places, which is too ambiguous "
									  "to patch safely, so the patch is off", result.matches);
			logger::warn("signature matched {} times and was refused: {}", result.matches, a_pattern);
			return result;
		}

		result.found = true;
		result.address = static_cast<std::uintptr_t>(static_cast<std::ptrdiff_t>(first) + a_offset);
		result.note = std::format("found once, at game offset 0x{:X}", result.address - base + 0x1000);
		logger::info("signature verified at 0x{:X} (module + 0x{:X})", result.address, result.address - base);
		return result;
	}

	std::vector<std::uintptr_t> FindAll(const std::string& a_pattern, std::ptrdiff_t a_offset, std::size_t a_max)
	{
		std::vector<std::uintptr_t> hits;
		bool parsed = false;
		const auto pattern = Parse(a_pattern, parsed);
		if (!parsed || pattern.empty()) { logger::error("signature \"{}\" is malformed", a_pattern); return hits; }
		std::uintptr_t base = 0;
		std::size_t size = 0;
		if (!ModuleRange(base, size) || size < pattern.size()) { return hits; }
		const auto* bytes = reinterpret_cast<const std::uint8_t*>(base);
		const std::size_t last = size - pattern.size();
		for (std::size_t i = 0; i <= last && hits.size() < a_max; ++i)
		{
			bool hit = true;
			for (std::size_t j = 0; j < pattern.size(); ++j)
			{
				const int p = pattern[j];
				if (p != kWildcard && bytes[i + j] != static_cast<std::uint8_t>(p)) { hit = false; break; }
			}
			if (hit) { hits.push_back(static_cast<std::uintptr_t>(static_cast<std::ptrdiff_t>(base + i) + a_offset)); }
		}
		logger::debug("signature matched {} time(s): {}", hits.size(), a_pattern);
		return hits;
	}
}
