#include "PCH.h"

// Njordlinger Combat Gameplay Overhaul - the item card of a base-enchanted item (combined run 765a).
//
// Evidence (op=stonesitem on Iron Sword of Fire with a Yol stone): the instance's ExtraEnchantment held the base's Fire
// Damage AND the Yol line, yet the card showed only the base's line. The game's InventoryEntryData::GetEnchantment
// (Address Library 15788 / 16026) returns the base form's enchantment before an instance one, so every reader of it -
// the item card among them - saw the base. This hook asks first: if an extra list of the entry carries an enchantment
// this mod composed (it holds a soul-stone effect), that one is the item's enchantment; otherwise the game's answer.
// Items without our stones are untouched (stock and player-enchanted items read exactly as before).
//
// The entry is detoured: the first whole instructions (at least 5 bytes) are copied into a thunk that jumps back, and
// only prologue shapes this file can decode are accepted - anything else, or another plugin's branch already there,
// is refused with a log line and the card stays vanilla (rule: refuse, never guess).

#include "SoulStones.h"

#include "utils/Logger.h"

#include <cstring>

namespace CardEnchantment
{
	namespace
	{
		using GetEnchantment_t = RE::EnchantmentItem* (*)(RE::InventoryEntryData*);
		REL::Relocation<GetEnchantment_t> g_original;

		RE::EnchantmentItem* Hook(RE::InventoryEntryData* a_this)
		{
			if (a_this && a_this->extraLists)
			{
				for (auto* x : *a_this->extraLists)
				{
					auto* xe = x ? x->GetByType<RE::ExtraEnchantment>() : nullptr;
					if (xe && xe->enchantment && SoulStones::IsComposedEnchantment(xe->enchantment)) { return xe->enchantment; }
				}
			}
			return g_original(a_this);
		}

		// Length of one instruction from a small set of prologue shapes (no rip-relative operand), 0 = not understood.
		std::size_t InstructionLength(const std::uint8_t* p)
		{
			if (p[0] >= 0x50 && p[0] <= 0x57) { return 1; }                                      // push r
			if (p[0] == 0x40 && p[1] >= 0x50 && p[1] <= 0x57) { return 2; }                      // push r (REX)
			if (p[0] == 0x41 && p[1] >= 0x50 && p[1] <= 0x57) { return 2; }                      // push r8-r15
			if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8B && (p[2] == 0xC4 || p[2] == 0xDC)) { return 3; }   // mov rax/r11, rsp
			if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) { return 4; }                      // sub rsp, imm8
			if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) { return 7; }                      // sub rsp, imm32
			if (p[0] == 0x48 && p[1] == 0x89 && (p[2] == 0x5C || p[2] == 0x6C || p[2] == 0x74 || p[2] == 0x7C || p[2] == 0x4C || p[2] == 0x54) && p[3] == 0x24) { return 5; }   // mov [rsp+imm8], r
			if (p[0] == 0x48 && p[1] == 0x89 && (p[2] == 0x58 || p[2] == 0x68 || p[2] == 0x70 || p[2] == 0x78 || p[2] == 0x48 || p[2] == 0x50)) { return 4; }   // mov [rax+imm8], r
			if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xD9) { return 3; }                      // mov rbx, rcx
			if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x41) { return 4; }                      // mov rax, [rcx+imm8] (11b9: 48 8B 41 08)
			if (p[0] == 0x48 && p[1] == 0x85 && p[2] == 0xC0) { return 3; }                      // test rax, rax (11b9: 48 85 C0)
			return 0;
		}
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(15788, 16026) };
		const auto entry = target.address();
		const auto* code = reinterpret_cast<const std::uint8_t*>(entry);
		if (code[0] == 0xE9 || code[0] == 0xFF)
		{
			logger::warn("item card: another plugin has already hooked InventoryEntryData::GetEnchantment (0x{:X}) - a base-enchanted item's card keeps showing its base enchantment only", entry - REL::Module::get().base());
			return;
		}
		std::size_t len = 0;
		while (len < 5)
		{
			const auto n = InstructionLength(code + len);
			if (n == 0)
			{
				logger::warn("item card: the start of InventoryEntryData::GetEnchantment is not a known shape ({:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}) - nothing written; a base-enchanted item's card shows its base enchantment only",
					code[0], code[1], code[2], code[3], code[4], code[5], code[6]);
				return;
			}
			len += n;
		}
		auto& trampoline = SKSE::GetTrampoline();
		auto* thunk = static_cast<std::uint8_t*>(trampoline.allocate(len + 14));
		if (!thunk) { logger::warn("item card: no trampoline space - nothing written"); return; }
		std::memcpy(thunk, code, len);
		thunk[len] = 0xFF; thunk[len + 1] = 0x25; std::memset(thunk + len + 2, 0, 4);
		const std::uintptr_t resume = entry + len;
		std::memcpy(thunk + len + 6, &resume, 8);
		g_original = REL::Relocation<GetEnchantment_t>(reinterpret_cast<std::uintptr_t>(thunk));
		trampoline.write_branch<5>(entry, Hook);
		logger::info("item card: InventoryEntryData::GetEnchantment prefers a soul-stone enchantment of the instance ({} prologue bytes moved)", len);
	}
}
