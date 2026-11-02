// Regression test for the player-typed game-event fields of levels_ranks (the P1 fix), engine-free:
// the __s2sdk_* event bindings are recording fakes and the event is a plain fixture.
//
// The bug: the original Pisex module read "userid"/"attacker"/"assister" with GetEventInt, whose
// value is the engine *user id* (e.g. 7), and used it as the 0..63 player slot. The fix reads them
// through EventPlayerSlot() (src/lr_event_fields.hpp), which uses GetEventPlayerSlot (the slot) and,
// for a player_pawn field the primary accessor cannot resolve (bomb_pickup.userid), a map-gated
// GetEventPlayerPawn + EntPointerToPlayerSlot fallback. It never falls back to GetEventInt.
//
// Covered:
//   * slot 3 while GetEventInt would answer 7 (player_death.userid/attacker/assister);
//   * -1/world versus slot 0 (0 is a valid slot, not a sentinel) and out-of-range answers;
//   * numeric round_end.winner/damage stay on GetEventInt and are never player-resolved;
//   * bomb_pickup's player_pawn field: primary miss -> validated pawn -> EntPointerToPlayerSlot,
//     plus invalid pawn / pawn without a player / missing bindings / throwing accessors;
//   * the explicit map gate: not one accessor is called while it is closed.
//
// Run by ./test.sh (build/tests/lr_event_fields_tests).

#include "../src/lr_event_fields.hpp"

#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using levels_ranks::EventPlayerSlot;

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

bool Contains(const std::vector<std::string>& keys, const std::string& key) {
	for (const std::string& entry : keys) {
		if (entry == key) {
			return true;
		}
	}
	return false;
}

// --- the fake game event -----------------------------------------------------------------------

struct FakeEvent {
	std::map<std::string, int32_t> ints;   // what GetEventInt answers per key
	std::map<std::string, int32_t> slots;  // what GetEventPlayerSlot answers per key
	std::map<std::string, void*> pawns;    // what GetEventPlayerPawn answers per key
	std::set<void*> valid_pawns;           // IsValidEntPointer answers true for these
	std::map<void*, int32_t> pawn_slots;   // EntPointerToPlayerSlot answers per pawn
	bool slot_throws = false;
	bool pawn_throws = false;
};

FakeEvent g_event;
void* const g_event_ptr = &g_event;

std::vector<std::string> g_int_keys;
std::vector<std::string> g_slot_keys;
std::vector<std::string> g_pawn_keys;
int g_valid_pointer_calls = 0;
int g_pawn_slot_calls = 0;

int32_t FakeGetEventInt(void* /*event*/, const plg::string& key) {
	const std::string field = ToStd(key);
	g_int_keys.push_back(field);
	const auto found = g_event.ints.find(field);
	return found != g_event.ints.end() ? found->second : 0;
}

int32_t FakeGetEventPlayerSlot(void* /*event*/, const plg::string& key) {
	const std::string field = ToStd(key);
	g_slot_keys.push_back(field);
	if (g_event.slot_throws) {
		throw std::runtime_error("slot accessor");
	}
	const auto found = g_event.slots.find(field);
	return found != g_event.slots.end() ? found->second : -1;
}

void* FakeGetEventPlayerPawn(void* /*event*/, const plg::string& key) {
	const std::string field = ToStd(key);
	g_pawn_keys.push_back(field);
	if (g_event.pawn_throws) {
		throw std::runtime_error("pawn accessor");
	}
	const auto found = g_event.pawns.find(field);
	return found != g_event.pawns.end() ? found->second : nullptr;
}

bool FakeIsValidEntPointer(void* entity) {
	++g_valid_pointer_calls;
	return g_event.valid_pawns.count(entity) != 0;
}

int32_t FakeEntPointerToPlayerSlot(void* entity) {
	++g_pawn_slot_calls;
	const auto found = g_event.pawn_slots.find(entity);
	return found != g_event.pawn_slots.end() ? found->second : -1;
}

void InstallFakes() {
	__s2sdk_GetEventInt = &FakeGetEventInt;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_GetEventPlayerPawn = &FakeGetEventPlayerPawn;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_EntPointerToPlayerSlot = &FakeEntPointerToPlayerSlot;
}

void ResetEvent() {
	g_event = FakeEvent{};
	g_int_keys.clear();
	g_slot_keys.clear();
	g_pawn_keys.clear();
	g_valid_pointer_calls = 0;
	g_pawn_slot_calls = 0;
}

void* Pawn(uintptr_t address) {
	return reinterpret_cast<void*>(address);
}

// --- A. the slot, not the user id ---------------------------------------------------------------

void TestPlayerSlotIsNotUserId() {
	std::printf("-- player-typed fields resolve the slot, not the user id\n");
	ResetEvent();
	g_event.slots["userid"] = 3;
	g_event.ints["userid"] = 7;
	g_event.slots["attacker"] = 5;
	g_event.ints["attacker"] = 13;
	g_event.slots["assister"] = 9;
	g_event.ints["assister"] = 21;

	Check(EventPlayerSlot(g_event_ptr, "userid", true) == 3,
		  "player_death.userid resolves to slot 3, not to the user id 7");
	Check(EventPlayerSlot(g_event_ptr, "attacker", true) == 5,
		  "player_death.attacker resolves to slot 5, not to the user id 13");
	Check(EventPlayerSlot(g_event_ptr, "assister", true) == 9,
		  "player_death.assister resolves to slot 9, not to the user id 21");
	Check(g_int_keys.empty(), "GetEventInt is never asked for a player-typed field");
	Check(Contains(g_slot_keys, "userid"), "the slot accessor is what resolved the field");
	Check(s2sdk::GetEventInt(g_event_ptr, plg::string("userid")) == 7,
		  "the raw GetEventInt value is the user id 7 - the value the old code used as a slot");
}

// --- B. world versus slot 0 --------------------------------------------------------------------

void TestWorldAndSlotZero() {
	std::printf("-- world (-1) versus slot 0\n");

	// The world (no player behind the field): -1, never slot 0.
	ResetEvent();
	g_event.slots["userid"] = -1;
	g_event.ints["userid"] = 0; // a user id 0 must not turn into slot 0 either
	Check(EventPlayerSlot(g_event_ptr, "userid", true) == -1, "a world userid answers -1");
	Check(Contains(g_slot_keys, "userid"), "the primary slot accessor was tried");
	Check(Contains(g_pawn_keys, "userid"), "the player_pawn fallback was tried before giving up");
	Check(g_valid_pointer_calls == 0, "a null pawn pointer is not validated");
	Check(g_pawn_slot_calls == 0, "a null pawn pointer is not converted");
	Check(g_int_keys.empty(), "the world case never reads the user id");

	// Slot 0 is a real player slot: 0 must survive the range check.
	ResetEvent();
	g_event.slots["userid"] = 0;
	g_event.ints["userid"] = 7;
	Check(EventPlayerSlot(g_event_ptr, "userid", true) == 0, "slot 0 is accepted as a player slot");
	Check(EventPlayerSlot(g_event_ptr, "userid", true) >= 0, "slot 0 is not the -1 sentinel");
	Check(g_int_keys.empty(), "slot 0 does not need the user id");

	// An out-of-range primary answer is not a slot; the fallback still gets its chance.
	ResetEvent();
	g_event.slots["userid"] = 64;
	Check(EventPlayerSlot(g_event_ptr, "userid", true) == -1, "an out-of-range primary answer is rejected");
	Check(Contains(g_pawn_keys, "userid"), "the fallback is attempted for an unresolved primary answer");

	// ... and an out-of-range pawn answer is rejected too.
	void* const pawn = Pawn(0x7001);
	ResetEvent();
	g_event.slots["userid"] = -1;
	g_event.pawns["userid"] = pawn;
	g_event.valid_pawns.insert(pawn);
	g_event.pawn_slots[pawn] = 64;
	Check(EventPlayerSlot(g_event_ptr, "userid", true) == -1, "an out-of-range pawn slot is rejected");
}

// --- C. numeric fields stay numeric ------------------------------------------------------------

void TestNumericWinnerStaysInt() {
	std::printf("-- round_end.winner / damage stay on GetEventInt\n");
	ResetEvent();
	g_event.ints["winner"] = 3;
	g_event.ints["dmg_health"] = 27;
	g_event.slots["winner"] = -1; // the player accessor cannot resolve a numeric field

	Check(s2sdk::GetEventInt(g_event_ptr, plg::string("winner")) == 3, "round_end.winner is read with GetInt");
	Check(s2sdk::GetEventInt(g_event_ptr, plg::string("dmg_health")) == 27, "damage is read with GetInt");
	Check(Contains(g_int_keys, "winner") && Contains(g_int_keys, "dmg_health"),
		  "both numeric fields went through GetEventInt");
	Check(EventPlayerSlot(g_event_ptr, "winner", true) == -1,
		  "the player accessor cannot resolve a numeric field");
	Check(g_int_keys.size() == 2, "the player accessor never asked GetEventInt");
}

// --- D. the player_pawn field (bomb_pickup.userid) ---------------------------------------------

void TestPlayerPawnFallback() {
	std::printf("-- player_pawn fields (bomb_pickup.userid) and the guarded fallback\n");
	void* const pawn = Pawn(0x9005);

	// The primary accessor misses a player_pawn field: the pawn path answers the slot.
	ResetEvent();
	g_event.slots["userid"] = -1;
	g_event.ints["userid"] = 71; // the raw user id, which must never be used
	g_event.pawns["userid"] = pawn;
	g_event.valid_pawns.insert(pawn);
	g_event.pawn_slots[pawn] = 5;
	Check(EventPlayerSlot(g_event_ptr, "userid", true) == 5,
		  "bomb_pickup.userid (player_pawn) resolves through the pawn");
	Check(Contains(g_slot_keys, "userid"), "the primary slot accessor was tried first");
	Check(Contains(g_pawn_keys, "userid"), "the pawn accessor resolved the field");
	Check(g_valid_pointer_calls == 1, "the pawn pointer was validated");
	Check(g_pawn_slot_calls == 1, "the pawn was converted by EntPointerToPlayerSlot");
	Check(g_int_keys.empty(), "the fallback never uses GetEventInt");

	// A pawn pointer that is not a valid entity is dropped before the conversion.
	ResetEvent();
	g_event.slots["userid"] = -1;
	g_event.pawns["userid"] = pawn;
	g_event.pawn_slots[pawn] = 5; // would answer 5 if it were called
	Check(EventPlayerSlot(g_event_ptr, "userid", true) == -1,
		  "a pawn that is not a valid entity pointer is dropped");
	Check(g_valid_pointer_calls == 1, "the invalid pointer was checked");
	Check(g_pawn_slot_calls == 0, "EntPointerToPlayerSlot is not called for an invalid pawn");

	// A valid pawn without a controller/player slot.
	ResetEvent();
	g_event.slots["userid"] = -1;
	g_event.pawns["userid"] = pawn;
	g_event.valid_pawns.insert(pawn);
	g_event.pawn_slots[pawn] = -1;
	Check(EventPlayerSlot(g_event_ptr, "userid", true) == -1, "a pawn without a player answers -1");

	// An older s2sdk without the primary binding: the pawn fallback still resolves the field.
	auto* const saved_slot_binding = __s2sdk_GetEventPlayerSlot;
	__s2sdk_GetEventPlayerSlot = nullptr;
	ResetEvent();
	g_event.pawns["userid"] = pawn;
	g_event.valid_pawns.insert(pawn);
	g_event.pawn_slots[pawn] = 5;
	g_event.ints["userid"] = 71;
	Check(EventPlayerSlot(g_event_ptr, "userid", true) == 5,
		  "without the primary binding the pawn fallback resolves a player_pawn field");
	Check(g_slot_keys.empty(), "the missing primary binding was not called");
	Check(g_int_keys.empty(), "no GetEventInt fallback without the primary binding");
	__s2sdk_GetEventPlayerSlot = saved_slot_binding;

	// A resolved primary still works when the fallback bindings are missing.
	auto* const saved_pawn_binding = __s2sdk_GetEventPlayerPawn;
	auto* const saved_valid_binding = __s2sdk_IsValidEntPointer;
	auto* const saved_convert_binding = __s2sdk_EntPointerToPlayerSlot;
	__s2sdk_GetEventPlayerPawn = nullptr;
	__s2sdk_IsValidEntPointer = nullptr;
	__s2sdk_EntPointerToPlayerSlot = nullptr;
	ResetEvent();
	g_event.slots["userid"] = 3;
	Check(EventPlayerSlot(g_event_ptr, "userid", true) == 3,
		  "a player_controller field resolves without the pawn fallback bindings");
	__s2sdk_GetEventPlayerPawn = saved_pawn_binding;
	__s2sdk_IsValidEntPointer = saved_valid_binding;
	__s2sdk_EntPointerToPlayerSlot = saved_convert_binding;

	// A throwing primary accessor degrades to the pawn fallback instead of crashing the hook.
	ResetEvent();
	g_event.slot_throws = true;
	g_event.pawns["userid"] = pawn;
	g_event.valid_pawns.insert(pawn);
	g_event.pawn_slots[pawn] = 5;
	Check(EventPlayerSlot(g_event_ptr, "userid", true) == 5,
		  "a throwing primary accessor degrades to the pawn fallback");

	// A throwing pawn accessor fails soft.
	ResetEvent();
	g_event.slots["userid"] = -1;
	g_event.pawn_throws = true;
	Check(EventPlayerSlot(g_event_ptr, "userid", true) == -1, "a throwing pawn accessor answers -1");
}

// --- E. the map gate ---------------------------------------------------------------------------

void TestMapGate() {
	std::printf("-- the map gate\n");
	ResetEvent();
	g_event.slots["userid"] = 3;
	g_event.ints["userid"] = 7;
	Check(EventPlayerSlot(g_event_ptr, "userid", false) == -1,
		  "no field is read while the map gate is closed");
	Check(g_slot_keys.empty() && g_pawn_keys.empty() && g_int_keys.empty(),
		  "not one event accessor is called while no map is up");

	Check(EventPlayerSlot(nullptr, "userid", true) == -1, "a null event answers -1");
	Check(EventPlayerSlot(g_event_ptr, nullptr, true) == -1, "a null key answers -1");
	Check(g_slot_keys.empty() && g_pawn_keys.empty() && g_int_keys.empty(),
		  "nothing is called for an invalid request");
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("levels_ranks event-field tests (fake s2sdk bindings, no engine)\n");

	InstallFakes();

	TestPlayerSlotIsNotUserId();
	TestWorldAndSlotZero();
	TestNumericWinnerStaysInt();
	TestPlayerPawnFallback();
	TestMapGate();

	std::printf("lr_event_fields_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
