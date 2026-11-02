#pragma once
// Player-typed versus numeric fields of the hooked game events (the P1 fix of levels_ranks).
//
// s2sdk has two different accessors for a game-event field:
//   * GetEventInt(key)       - the raw stored value. For a player-typed field this is the engine
//                              *user id* ("userid"), which is NOT the 0..63 player slot the core
//                              indexes its arrays with;
//   * GetEventPlayerSlot(key) - the 0..63 slot of the player behind the field, -1
//                              (INVALID_PLAYER_SLOT_INDEX) when the field holds no player.
//
// The gameevents dump types the fields this plugin reads as:
//   player_death.userid / attacker / assister       player_controller_and_pawn
//   player_hurt.userid / attacker                   player_controller_and_pawn
//   weapon_fire.userid                              player_controller_and_pawn
//   bomb_planted / bomb_defused / bomb_dropped      player_controller_and_pawn
//   hostage_killed / hostage_rescued.userid         player_controller_and_pawn
//   round_mvp.userid                                player_controller
//   bomb_pickup.userid                              player_pawn   (no controller in the field)
//
// A player_pawn field is not guaranteed to resolve through GetEventPlayerSlot (the field holds
// the pawn, while the primary accessor works off the controller pair). A field the primary
// accessor cannot resolve is therefore retried through GetEventPlayerPawn +
// EntPointerToPlayerSlot. That fallback
//   * runs only while the plugin's map gate is open (PITFALLS B2: IsValidEntPointer and
//     EntPointerToPlayerSlot are entity calls and crash between OnMapEnd and the next OnMapStart),
//   * validates the pawn pointer before converting it,
//   * never falls back to GetEventInt - a user id is not a slot.
//
// Numeric fields (round_end.winner, damage, headshot, ...) are not player-typed and keep
// GetEventInt untouched; nothing in this header reads them.

#include <plg/string.hpp>

#include <s2sdk/s2sdk.hpp>

#include "lr_core.hpp" // kMaxSlots

namespace levels_ranks {

// The slot of a player-typed event field ("userid"/"attacker"/"assister"/...): -1 when the field
// holds no player, the field is not player-typed, the accessors are unavailable or the map gate is
// closed. The plugin passes IsMapReady(); the tests pass their own gate explicitly.
inline int EventPlayerSlot(void* event, const char* key, bool map_ready) {
	if (event == nullptr || key == nullptr || !map_ready) {
		return -1;
	}

	// Primary: GetEventPlayerSlot. player_controller_and_pawn and player_controller fields resolve
	// here; a player_pawn field may answer -1 (or an out-of-range index) instead.
	if (__s2sdk_GetEventPlayerSlot != nullptr) {
		try {
			const int32_t slot = s2sdk::GetEventPlayerSlot(event, plg::string(key));
			if (slot >= 0 && slot < kMaxSlots) {
				return static_cast<int>(slot);
			}
		} catch (...) {
			// The fallback below may still resolve the field.
		}
	}

	// player_pawn fallback: the pawn of the field, validated, converted by the engine. Strictly
	// behind the map gate - both calls are entity calls (PITFALLS B2).
	if (__s2sdk_GetEventPlayerPawn == nullptr || __s2sdk_IsValidEntPointer == nullptr ||
		__s2sdk_EntPointerToPlayerSlot == nullptr) {
		return -1;
	}
	try {
		void* const pawn = s2sdk::GetEventPlayerPawn(event, plg::string(key));
		if (pawn == nullptr || !s2sdk::IsValidEntPointer(pawn)) {
			return -1;
		}
		const int32_t slot = s2sdk::EntPointerToPlayerSlot(pawn);
		return slot >= 0 && slot < kMaxSlots ? static_cast<int>(slot) : -1;
	} catch (...) {
		return -1;
	}
}

} // namespace levels_ranks
