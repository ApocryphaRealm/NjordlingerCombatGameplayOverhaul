#pragma once

// Character Progression Control - runtime byte-signature scanning.
//
// WHY THIS EXISTS, and it is not a preference:
//
// The retail SkyrimSE.exe is Steam-packed. It carries a .bind section and its entry point sits
// inside it, not in .text, which means the real code is ENCRYPTED ON DISK and only decrypted
// into memory when the game runs (measured 2026-09-03 - ordinary instruction sequences appear
// nowhere in the shipped file, while arbitrary byte probes match, which is only possible if the
// section is encrypted). So a byte signature cannot be checked ahead of time against the file;
// it can only be found in the running process.
//
// That settles the design. A patch that needs a signature scans for it at load, in memory, and
// the result is a fact the mod reports rather than an assumption it acts on:
//
//   * found exactly once  -> a verified address for THIS runtime, whatever version it is.
//   * found several times -> refused. An ambiguous signature is worse than none, because the
//                            wrong one of several matches is a crash with no clue attached.
//   * not found           -> refused, reported, and that patch group stays off while the rest of
//                            the mod carries on.
//
// A signature is data, not code: it is the one thing that has to change when a Windows or Skyrim
// update moves the game around, and nothing else in the mod needs to know.

#include <cstdint>
#include <string>
#include <vector>

namespace Signature
{
	struct Result
	{
		bool found = false;
		int matches = 0;            // how many places matched; anything but 1 is refused
		std::uintptr_t address = 0; // absolute address of the match, plus the signature's offset
		std::string note;           // a plain sentence for the Patches tab and the log
	};

	// Pattern text is "48 8B 01 FF 50 18 ?? ?? 0F 28", where ?? (or ?) is any byte.
	// a_offset is added to the match, for a signature that anchors near - rather than at - the
	// place to be patched.
	Result Find(const std::string& a_pattern, std::ptrdiff_t a_offset = 0);

	// Every place a signature matches, up to a_max, each plus a_offset. For a site the game carries in
	// more than one copy (the legendary eligibility check has two): the CALLER states how many copies it
	// expects and proves each one itself, so an unexpected count is still refused - by the caller.
	std::vector<std::uintptr_t> FindAll(const std::string& a_pattern, std::ptrdiff_t a_offset = 0, std::size_t a_max = 8);

	// The executable section actually searched, for the log.
	bool ModuleRange(std::uintptr_t& a_base, std::size_t& a_size);
}
