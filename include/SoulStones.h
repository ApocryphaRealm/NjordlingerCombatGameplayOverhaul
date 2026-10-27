#pragma once

// Njordlinger Combat Gameplay Overhaul - the Soul Stones module (PLAN.md phase 4, Njordlinger 2.0; D17-D22).
//
// A soul stone is a word of the dragon tongue (60 words, five soul-gem tiers; the records are built from
// config/stones.json). An item's tier gives it stone slots (common 0, magic 1, epic 2, legendary 3, unique 3); staves
// take none. What sits in each slot is per ITEM INSTANCE: this module keeps a table in the co-save (record NJSS), keyed
// by the instance's ExtraUniqueID (assigned on first socketing), holding the stones in slot order and what the item
// carried before (its own enchantment's effects and custom name), so unsocketing the last stone gives the item back.
//
// The effect (D17, study 9.2): the engine's only per-instance effect carrier is ExtraEnchantment, so the module composes
// one enchantment per instance through the game's own created-enchantment manager (the Arcane Enchanter's path): the
// item's own enchantment (merged, never replaced), then each stone's effect for the host - melee weapon, bow or crossbow,
// armour - with the tier's magnitude, then the phrase effect when slots one to three spell one Shout in word order AND the
// player has unlocked all three words with dragon souls (D20; otherwise a "phrase you do not yet know" line), then the
// tier glow on a weapon (the best stone's tier colour). Effects that no enchantment can carry are applied by this module
// while the item is equipped: perks of entry points and abilities whose value is set from the tier, and procs on hit,
// on being hit and on a kill (cast a Shout's own spell, a helper spell, or damage). A Shout-only phrase on armour grants
// the Shout itself as a hidden clone (D21): its level is the run of the phrase's words, from the first, that sit in worn
// armour and that the player has LEARNED at a wall (the stone stands in for the soul); the clone goes when the stones do.
// The item's name ends with its words ("Elven Sword [Yol Fo -]") through ExtraTextDisplayData.
//
// Re-composition runs on equip, socketing, load and menu close (a word learned or unlocked changes phrases at once);
// it compares the wanted effects with the item's current ones and does nothing when they match, so it is cheap and the
// created enchantments are reused (the manager returns an existing one for the same effects).
//
// Threading: event sinks copy what they need and queue main-thread tasks (SKSE AddTask); the settings page and DevBench
// read a mutex-guarded snapshot and queue requests. No Scaleform anywhere in this module.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace SoulStones
{
	inline constexpr std::uint32_t kRecord = 'NJSS';

	// kDataLoaded: reads stones.json and resolves every form; registers the event sinks.
	void Install();

	// kPostLoadGame / kNewGame: matches the co-save table to the inventory, repairs lost keys from the items'
	// enchantments, re-composes every socketed item, and applies perks, abilities and granted Shouts.
	void OnGameLoaded();

	void OnSave(SKSE::SerializationInterface* a_intfc);
	void ReadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void OnRevert();

	enum class Host : int { kNone = -1, kMelee = 0, kRanged = 1, kArmour = 2 };

	struct StoneRef
	{
		int word = -1;   // index into the 60 words (stone-scheme order); -1 = an empty slot
		int tier = -1;   // 0 petty .. 4 grand
	};

	struct ItemView
	{
		std::string key;          // what Socket / Unsocket take: "<base>:<owner>:<uid>", or "<base>:plain" for a stack
		std::string name;
		Host host = Host::kNone;
		int slots = 0;
		int count = 1;            // a plain stack's size (socketing splits one off)
		bool equipped = false;
		std::array<StoneRef, 3> stones{};
		int phrase = -1;          // the phrase slots 1-3 spell in word order, or -1
		bool phraseKnown = false; // ... and the player has unlocked all three of its words
		// the preview card (the owner 22:39)
		std::string itemTier;     // Common, Magic, Epic, Legendary, Unique ("" = none)
		int naturalSlots = 0;     // the tier's slots; slots - naturalSlots were added by the runesmith
		float stat = 0.0F;        // weapon damage or armour rating of the base item
		std::vector<std::string> enchantLines;   // the item's own enchantment, effect by effect (name and magnitude)
	};

	struct StoneView
	{
		int word = 0;
		int tier = 0;
		int count = 0;
	};

	struct PhraseView
	{
		std::string shout;
		std::array<int, 3> words{};
		bool unlocked = false;    // all three words unlocked with dragon souls (the phrase works)
		int learned = 0;          // words learned at a wall (the blueprint: the page names only phrases with all three)
		int granted = 0;          // the clone Shout's level granted by worn stones (armour reading, D21), 0 = none
		std::array<bool, 3> wordLearned{};   // each word learned at a wall (the known-words tab, the owner 22:55)
		bool available = true;    // false: a soft-linked Shout whose plugin is not loaded
	};

	struct GemView                // D22 revised: a soul gem kind in the pack, as the runesmith grinds it
	{
		RE::FormID form = 0;
		std::string name;
		int filled = 0, empty = 0;
		int tier = 0;                 // the dust it becomes
		int dust = 0;                 // what grinding every copy gives
	};

	struct SlotOffer              // ENCHANTING-OFF.md 7: an item the runesmith can add a slot to
	{
		std::string key;
		std::string name;
		int slots = 0;                // now
		int added = 0;                // added so far
		int gemTier = 0;              // the next slot's gem: 0 lesser, 1 greater, 2 grand
	};

	struct Snapshot
	{
		bool loaded = false;
		std::string error;
		std::vector<std::string> words;       // word names, stone-scheme order
		std::vector<std::string> tiers;       // tier names
		std::vector<ItemView> items;          // eligible items in the player's inventory (slots > 0)
		std::vector<StoneView> stones;        // stones the player carries
		std::vector<PhraseView> phrases;
		std::array<int, 5> feeGems{};         // soul gem dust the player carries, by tier (the fee; D22 revised)
		std::vector<GemView> gems;            // gems he can grind
		std::vector<int> learnedWords;        // words learned at a wall: the stones he can cut
		int ingots = 0;                       // orichalcum ingots carried
		std::array<int, 5> craftDust{};       // dust a stone of each tier takes
		std::uint32_t dustMade = 0, stonesMade = 0;   // this save's tallies
		bool serviceOpen = false;             // socketing is open (the runesmith, or DevBench's test switch)
		bool canSocket = false;               // the socket buttons show (the runesmith's service is open)
		std::vector<SlotOffer> slotOffers;    // items below three slots
		int addIngots = 0;
		std::array<int, 3> addGems{};         // lesser / greater / grand gems carried (the add-slot cost)
		std::uint32_t tableEntries = 0;       // socketed instances in the co-save table
		std::uint32_t composes = 0;           // enchantments composed this session
		std::uint32_t procsFired = 0;
		std::vector<std::string> activePerks; // stone perks and abilities on the player now
		std::string lastResult;               // the last socket / unsocket result (for the page and DevBench)
		std::uint64_t revision = 0;           // bumps with every rebuild of this snapshot
	};

	Snapshot GetSnapshot();

	// Rebuilds the snapshot on the main thread (the page asks when it opens and after every action). Safe from any thread.
	void RequestRefresh();

	// Re-composes every socketed item and re-applies perks and Shouts (after a settings change). Safe from any thread.
	void RequestRecompose();

	// Socket a stone into a slot (1-based slot index 0..2) / take it out. Fee: one soul gem dust of the stone's tier (D22
	// revised); taking a stone out is free and returns it. Refused unless the service is open. Safe from any thread;
	// the result lands in the snapshot's lastResult.
	void RequestSocket(const std::string& a_itemKey, int a_slot, int a_word, int a_tier);
	void RequestUnsocket(const std::string& a_itemKey, int a_slot);

	// D22 revised: grind one (or every) copy of a soul gem kind into dust; cut a stone of a learned word (1 orichalcum
	// ingot + dust of the tier). Refused unless the service is open. Safe from any thread.
	void RequestGrind(RE::FormID a_gem, bool a_all);
	void RequestCraft(int a_word, int a_tier);
	// The tab the runesmith's line asked the window to open on (0 socket, 1 grind, 2 craft), once; -1 = none.
	int TakeRequestedTab();

	// D29: a dragon (a full set of one of its Shouts) or a named boss at level 30+ (one stone, 25%) killed by the player or a
	// follower leaves stones on the corpse; DevBench forces the next roll (op=stonesdrop).
	void ForceNextDrop(bool a_force);
	// ENCHANTING-OFF.md 7: the runesmith adds one stone slot to an item (both modes; up to three). Safe from any thread.
	void RequestAddSlot(const std::string& a_itemKey);
	// DevBench op=stonesitem: one instance's own and base enchantment effects (lastResult).
	void RequestItemDiag(const std::string& a_itemKey);

	// The runesmith opens the service (his dialogue after the quest); DevBench may open it for a test.
	void SetServiceOpen(bool a_open);
	bool IsServiceOpen();

	// An enchantment this module composed (it carries a soul-stone effect). Read-only table lookups: safe from any thread
	// once the table is loaded.
	bool IsComposedEnchantment(RE::EnchantmentItem* a_ench);

	// A Shout the stones grant (D21's hidden clones) - Shouting-off lets these through.
	bool IsStoneShout(RE::TESShout* a_shout);

	// The settings page registers how its runesmith window opens and closes (true = open); called on the main thread.
	void SetWindowOpener(std::function<void(bool)> a_fn);

	// DevBench: gives the player stones and fee gems (tests only), and the live state as JSON.
	void RequestGiveForTest(int a_word, int a_tier, int a_count);
	std::string StatusJson();
	std::string WordsJson();   // the 60 real words: form, flags, learned, unlocked (op=stoneswords)

	// The words' names (stone-scheme order) and the tier names, for the page.
	const std::vector<std::string>& WordNames();
	const std::vector<std::string>& TierNames();
	int PhraseOfWord(int a_word);
	std::string PhraseName(int a_phrase);
	std::string CellText(int a_word, Host a_host, int a_tier);
	std::string PhraseText(int a_phrase, Host a_host);            // the phrase's line in a host (English, from the plugin)   // a word's line in a host at a tier, <mag>/<dur> filled (English, from the plugin)
}
