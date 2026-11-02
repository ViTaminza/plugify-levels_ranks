#pragma once
// levels_ranks — the plugin's own s2sdk safety layer.
//
// The plugin is pure s2sdk (docs/PLUGIN_STANDARD.md §7), so every safety rule the previous port
// got from its wrapper library lives here:
//
//   * Map gate (PITFALLS B1/B2). No client, entity or schema call is made before the first map
//     and none in the window from OnMapEnd until the next OnMapStart: s2sdk dereferences a null
//     client list there. InitMapGate() registers the s2sdk OnMapStart/OnMapEnd listeners in
//     OnPluginStart - but only when all four bindings (both Register and both Unregister) exist,
//     because a half-registered pair would survive `plg reload` (A7). OnMapStart opens the gate,
//     OnMapEnd drops it first and only then calls the plugin hook (which may only clear state).
//     PollMapGate() runs from OnPluginUpdate and latches readiness for a plugin loaded into a
//     running map: first update only, no OnMapEnd seen since start, the listener pair registered,
//     and s2sdk::GetCurrentMap() proves a map is up (non-empty and not "<empty>"). After an
//     OnMapEnd only a real OnMapStart re-opens the gate.
//   * Client checks (PITFALLS C2). s2sdk exports do not validate their arguments: every query
//     layers the gate, the slot range 0..63, IsClientConnected/IsClientInGame/!IsClientSourceTV,
//     a controller entity pointer and a valid controller handle (IsValidEntHandle +
//     IsValidEntPointer). Nothing below a failed layer reaches s2sdk.
//   * Entity handles (PITFALLS C1). A CEntityHandle is `index | serial << 15`: 0 is the world and
//     -1 is invalid, while a valid handle can be negative once the serial passes 65535. The test
//     is therefore `!= 0 && != -1` followed by s2sdk::IsValidEntHandle - never `> 0`.
//   * Owner identity (the menu layer's stale-request guard). CaptureOwner() records the SteamID64
//     and the controller handle/generation while the map is up; OwnerMatches() re-captures and
//     compares both. No raw entity pointer is kept across frames (PITFALLS C3).
//   * Schema offsets (PITFALLS C8). GetSchemaOffset is only called while the gate is open; only a
//     successful offset (>= 0) is cached. ClearSchemaOffsets() (OnPluginEnd) makes a reload resolve
//     the fields against the live map again.

#include <array>
#include <cstdint>
#include <string>

namespace levels_ranks {

// A plugin map-lifecycle hook, called by the OnMapStart/OnMapEnd listeners after the gate state
// has been updated. Plain function pointer (s2sdk listeners are C function pointers anyway) and
// noexcept in practice: the map-end hook runs while the client list is being torn down and must
// only drop state.
using MapLifecycleCallback = void (*)();

void SetMapLifecycleCallbacks(MapLifecycleCallback on_map_start, MapLifecycleCallback on_map_end);

// Registers the OnMapStart/OnMapEnd listeners. All four Register+Unregister slots must be
// present before anything is registered; with a partial API the gate stays shut.
void InitMapGate();

// Per-frame pump (OnPluginUpdate): opens the gate for a plugin that loaded into a running map.
void PollMapGate();

// Removes the map listeners and closes the gate; called from OnPluginEnd so `plg reload` leaves
// nothing pointing into the unloaded library (PITFALLS A7).
void ShutdownMapGate();

bool IsMapReady();
bool IsSlotInRange(int32_t slot);
// True when the four map-listener binding slots are present (diagnostics / tests).
bool MapListenerBindingsAvailable();

// --- guarded client queries (all of them need the map gate) ----------------------------------
bool IsClientConnected(int32_t slot);
bool IsClientAuthenticated(int32_t slot);
bool IsFakeClient(int32_t slot);
bool HasController(int32_t slot);
void* ControllerPtr(int32_t slot);
void* PawnPtr(int32_t slot);
bool HasPawn(int32_t slot);
std::string ClientName(int32_t slot);
std::string ClientAuthId(int32_t slot);
uint64_t ClientSteamId64(int32_t slot);
uint32_t ClientAccountId(int32_t slot);
// m_iTeamNum of the controller (1 byte), read at a map-gated cached schema offset like the
// original read CCSPlayerController::m_iTeamNum; 0 when the controller is gone.
int ClientTeam(int32_t slot);
// Current pawn health; 0 when there is no pawn.
int ClientHealth(int32_t slot);

// --- handles / schema -------------------------------------------------------------------------
// A usable (non-world, non-invalid, live) entity handle, only while a map is up.
bool IsUsableEntityHandle(int32_t handle);
// The map-gated schema lookup: -1 while the gate is closed or the field is unknown; only offsets
// >= 0 are cached.
int32_t SchemaOffset(const char* class_name, const char* field_name);
void ClearSchemaOffsets();

// --- owner identity ---------------------------------------------------------------------------
struct OwnerIdentity {
	int32_t slot = -1;
	uint64_t steamId = 0;
	int32_t controllerHandle = 0;

	constexpr bool IsSet() const {
		return slot >= 0 && steamId != 0 && controllerHandle != 0 && controllerHandle != -1;
	}
};

// Records the slot's verified owner. false (out is cleared) when there is no live map, no
// controller, the SteamID binding is missing or the engine answers 0 - the caller must drop the
// request rather than apply it to an unverified player.
bool CaptureOwner(int32_t slot, OwnerIdentity& out);
// Re-captures the slot and compares SteamID64 *and* controller handle/generation with `owner`.
bool OwnerMatches(const OwnerIdentity& owner);

// The g_bAdminAccess table of the original, bound to the player instead of the slot: a grant is
// valid only while the same SteamID64 and the same controller handle/generation still occupy the
// slot. A slot re-used by another player (or by the same player with a new controller entity)
// loses the access automatically, so a queued admin action can never inherit it.
class AdminAccess {
public:
	// Grants the slot's current owner; false (and the slot stays empty) when no verified owner can
	// be captured.
	bool Grant(int32_t slot) {
		if (!IsSlotInRange(slot)) {
			return false;
		}
		OwnerIdentity owner;
		if (!CaptureOwner(slot, owner)) {
			m_owners[static_cast<std::size_t>(slot)] = OwnerIdentity{};
			return false;
		}
		m_owners[static_cast<std::size_t>(slot)] = owner;
		return true;
	}

	void Revoke(int32_t slot) {
		if (IsSlotInRange(slot)) {
			m_owners[static_cast<std::size_t>(slot)] = OwnerIdentity{};
		}
	}

	// True only while the exact granted owner (SteamID64 + controller handle) is still in the slot.
	bool Has(int32_t slot) const {
		if (!IsSlotInRange(slot)) {
			return false;
		}
		const OwnerIdentity& owner = m_owners[static_cast<std::size_t>(slot)];
		return owner.IsSet() && OwnerMatches(owner);
	}

	void Clear() {
		for (OwnerIdentity& owner : m_owners) {
			owner = OwnerIdentity{};
		}
	}

private:
	std::array<OwnerIdentity, 64> m_owners{};
};

} // namespace levels_ranks
