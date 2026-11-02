#include "lr_gate.hpp"

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace levels_ranks {
namespace {

bool g_map_ready = false;
bool g_map_end_seen = false;
bool g_listeners_registered = false;
bool g_first_update_done = false;
bool g_start_registered = false;
bool g_end_registered = false;
bool g_bindings_error_logged = false;

MapLifecycleCallback g_on_map_start = nullptr;
MapLifecycleCallback g_on_map_end = nullptr;

void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(std::string("[levels_ranks] ") + std::string(message), plg::Severity::Error,
				 plg::source_location::current());
	}
}

// s2sdk listener callbacks are plain function pointers called on the game thread. Nothing here
// may throw and no client or entity call is made - only state changes.
void OnMapStartListener() {
	g_map_ready = true;
	g_map_end_seen = false;
	if (g_on_map_start != nullptr) {
		g_on_map_start();
	}
}

void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart the client list is torn down and every
	// client query crashes - the gate is dropped first and the hook only clears state.
	g_map_ready = false;
	g_map_end_seen = true;
	if (g_on_map_end != nullptr) {
		g_on_map_end();
	}
}

void UnregisterMapListeners() {
	g_listeners_registered = false;
	if (g_start_registered && __s2sdk_OnMapStart_Unregister != nullptr) {
		try {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		} catch (...) {
		}
	}
	g_start_registered = false;
	if (g_end_registered && __s2sdk_OnMapEnd_Unregister != nullptr) {
		try {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		} catch (...) {
		}
	}
	g_end_registered = false;
}

bool ClientBindingsPresent() {
	return __s2sdk_IsClientConnected != nullptr && __s2sdk_IsClientInGame != nullptr &&
		   __s2sdk_IsClientSourceTV != nullptr;
}

bool ControllerBindingsPresent() {
	return __s2sdk_PlayerSlotToEntPointer != nullptr && __s2sdk_PlayerSlotToEntHandle != nullptr &&
		   __s2sdk_IsValidEntHandle != nullptr && __s2sdk_IsValidEntPointer != nullptr;
}

bool Connected(int32_t slot) {
	if (__s2sdk_IsClientConnected == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot);
	} catch (...) {
		return false;
	}
}

// class + '\0' + field, so one small table holds every field without an allocation per lookup.
std::string OffsetKey(const char* class_name, const char* field_name) {
	std::string key(class_name);
	key.push_back('\0');
	key.append(field_name);
	return key;
}

struct SchemaOffsetCache {
	std::string key;
	int32_t offset = -1;
};

std::vector<SchemaOffsetCache> g_schema_offsets;

} // namespace

void SetMapLifecycleCallbacks(MapLifecycleCallback on_map_start, MapLifecycleCallback on_map_end) {
	g_on_map_start = on_map_start;
	g_on_map_end = on_map_end;
}

bool MapListenerBindingsAvailable() {
	return __s2sdk_OnMapStart_Register != nullptr && __s2sdk_OnMapStart_Unregister != nullptr &&
		   __s2sdk_OnMapEnd_Register != nullptr && __s2sdk_OnMapEnd_Unregister != nullptr;
}

void InitMapGate() {
	g_first_update_done = false;
	if (g_listeners_registered) {
		return;
	}
	if (!MapListenerBindingsAvailable()) {
		if (!g_bindings_error_logged) {
			g_bindings_error_logged = true;
			LogError("the s2sdk map listener bindings are incomplete (OnMapStart/OnMapEnd Register+"
					 "Unregister) - the plugin stays disabled until a reload");
		}
		return;
	}
	// Both Register+Unregister pairs are required; a half-registered pair would leave a callback
	// pointing into the unloaded library after `plg reload` (PITFALLS A7).
	// Mark both as attempted before the calls: if one registration throws after the host already
	// stored part of the pair, the teardown below must still call the matching Unregister (calling
	// an Unregister for a listener that was never stored is a no-op).
	g_start_registered = true;
	try {
		s2sdk::OnMapStart_Register(&OnMapStartListener);
	} catch (...) {
		LogError("could not register the s2sdk OnMapStart listener");
	}
	g_end_registered = true;
	try {
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
	} catch (...) {
		LogError("could not register the s2sdk OnMapEnd listener");
	}
	g_listeners_registered = g_start_registered && g_end_registered;
	if (!g_listeners_registered) {
		// A half-registered pair cannot close the gate safely; remove what slipped through.
		UnregisterMapListeners();
	}
}

void PollMapGate() {
	// A plugin loaded (`plg reload`) into a running map never saw OnMapStart. The first update can
	// latch readiness, but only with proof: the listener pair is registered, no OnMapEnd has been
	// seen since OnPluginStart, and s2sdk already reports a real map name. After OnMapEnd only a
	// real OnMapStart re-opens the gate (the host keeps pumping updates through a changelevel).
	if (!g_first_update_done) {
		g_first_update_done = true;
		if (!g_map_ready && !g_map_end_seen && g_listeners_registered &&
			__s2sdk_GetCurrentMap != nullptr) {
			try {
				const plg::string map = s2sdk::GetCurrentMap();
				const std::string_view name(map.data(), map.size());
				if (!name.empty() && name != "<empty>") {
					g_map_ready = true;
									if (g_on_map_start != nullptr) {
						g_on_map_start();
					}
				}
			} catch (...) {
			}
		}
	}
}

void ShutdownMapGate() {
	UnregisterMapListeners();
	g_map_ready = false;
	g_map_end_seen = false;
	g_first_update_done = false;
	g_bindings_error_logged = false;
	g_on_map_start = nullptr;
	g_on_map_end = nullptr;
	ClearSchemaOffsets();
}

bool IsMapReady() {
	return g_map_ready;
}

bool IsSlotInRange(int32_t slot) {
	return slot >= 0 && slot < 64;
}

bool IsClientConnected(int32_t slot) {
	if (!g_map_ready || !IsSlotInRange(slot) || !ClientBindingsPresent()) {
		return false;
	}
	return Connected(slot);
}

bool IsClientAuthenticated(int32_t slot) {
	if (!g_map_ready || !IsSlotInRange(slot) || !Connected(slot) ||
		__s2sdk_IsClientAuthorized == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientAuthorized(slot);
	} catch (...) {
		return false;
	}
}

bool IsFakeClient(int32_t slot) {
	if (!g_map_ready || !IsSlotInRange(slot) || !Connected(slot) || __s2sdk_IsFakeClient == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsFakeClient(slot);
	} catch (...) {
		return true; // treat an unavailable answer as "not a real player"
	}
}

bool IsUsableEntityHandle(int32_t handle) {
	if (!g_map_ready) {
		return false;
	}
	// 0 is the world and -1 is INVALID_EHANDLE_INDEX; a valid handle can be negative once the
	// serial counter passes 65535 (PITFALLS C1). Never use `> 0` here.
	if (handle == 0 || handle == -1 || __s2sdk_IsValidEntHandle == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsValidEntHandle(handle);
	} catch (...) {
		return false;
	}
}

bool HasController(int32_t slot) {
	if (!g_map_ready || !IsSlotInRange(slot) || !ClientBindingsPresent() ||
		!ControllerBindingsPresent()) {
		return false;
	}
	try {
		if (!s2sdk::IsClientConnected(slot) || !s2sdk::IsClientInGame(slot) ||
			s2sdk::IsClientSourceTV(slot)) {
			return false;
		}
		void* const pointer = s2sdk::PlayerSlotToEntPointer(slot);
		if (pointer == nullptr) {
			return false;
		}
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (!IsUsableEntityHandle(handle)) {
			return false;
		}
		return s2sdk::IsValidEntPointer(pointer);
	} catch (...) {
		return false;
	}
}

void* ControllerPtr(int32_t slot) {
	if (!HasController(slot) || __s2sdk_PlayerSlotToEntPointer == nullptr) {
		return nullptr;
	}
	try {
		return s2sdk::PlayerSlotToEntPointer(slot);
	} catch (...) {
		return nullptr;
	}
}

void* PawnPtr(int32_t slot) {
	if (!HasController(slot) || __s2sdk_GetClientPawn == nullptr) {
		return nullptr;
	}
	try {
		return s2sdk::GetClientPawn(slot);
	} catch (...) {
		return nullptr;
	}
}

bool HasPawn(int32_t slot) {
	return PawnPtr(slot) != nullptr;
}

std::string ClientName(int32_t slot) {
	if (!HasController(slot) || __s2sdk_GetClientName == nullptr) {
		return {};
	}
	try {
		const plg::string name = s2sdk::GetClientName(slot);
		std::string text(name.data(), name.size());
		if (text == "<unknown>") {
			return {}; // the s2sdk placeholder is not a player name
		}
		return text;
	} catch (...) {
		return {};
	}
}

std::string ClientAuthId(int32_t slot) {
	if (!IsClientConnected(slot) || __s2sdk_GetClientAuthId == nullptr) {
		return {};
	}
	try {
		const plg::string auth = s2sdk::GetClientAuthId(slot);
		return std::string(auth.data(), auth.size());
	} catch (...) {
		return {};
	}
}

uint64_t ClientSteamId64(int32_t slot) {
	if (!IsClientConnected(slot) || __s2sdk_GetClientSteamID64 == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

uint32_t ClientAccountId(int32_t slot) {
	if (!IsClientConnected(slot) || __s2sdk_GetClientAccountId == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientAccountId(slot);
	} catch (...) {
		return 0;
	}
}

int ClientTeam(int32_t slot) {
	// The team lives on the controller (CBaseEntity::m_iTeamNum, what the original read). s2sdk's
	// GetClientTeam goes through the pawn and warns for a client without one, so the controller
	// field is read at its map-gated cached offset instead (PITFALLS C6/C8).
	if (!HasController(slot) || __s2sdk_GetEntData2 == nullptr) {
		return 0;
	}
	const int32_t offset = SchemaOffset("CBaseEntity", "m_iTeamNum");
	if (offset < 0) {
		return 0;
	}
	void* const controller = ControllerPtr(slot);
	if (controller == nullptr) {
		return 0;
	}
	try {
		return static_cast<int>(s2sdk::GetEntData2(controller, offset, 1) & 0xFF);
	} catch (...) {
		return 0;
	}
}

int ClientHealth(int32_t slot) {
	if (!HasPawn(slot) || __s2sdk_GetClientHealth == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientHealth(slot);
	} catch (...) {
		return 0;
	}
}

int32_t SchemaOffset(const char* class_name, const char* field_name) {
	if (!g_map_ready || class_name == nullptr || field_name == nullptr ||
		__s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	const std::string key = OffsetKey(class_name, field_name);
	for (const SchemaOffsetCache& entry : g_schema_offsets) {
		if (entry.key == key) {
			return entry.offset;
		}
	}
	int32_t offset = -1;
	try {
		offset = s2sdk::GetSchemaOffset(plg::string(class_name), plg::string(field_name));
	} catch (...) {
		offset = -1;
	}
	if (offset >= 0) {
		g_schema_offsets.push_back(SchemaOffsetCache{key, offset});
	}
	return offset;
}

void ClearSchemaOffsets() {
	g_schema_offsets.clear();
}

bool CaptureOwner(int32_t slot, OwnerIdentity& out) {
	out = OwnerIdentity{};
	if (!g_map_ready || !IsSlotInRange(slot) || !HasController(slot)) {
		return false;
	}
	if (__s2sdk_GetClientSteamID64 == nullptr || __s2sdk_PlayerSlotToEntHandle == nullptr ||
		__s2sdk_IsValidEntHandle == nullptr) {
		return false;
	}
	const uint64_t steam_id = ClientSteamId64(slot);
	if (steam_id == 0) {
		return false;
	}
	int32_t handle = -1;
	try {
		handle = s2sdk::PlayerSlotToEntHandle(slot);
	} catch (...) {
		return false;
	}
	if (!IsUsableEntityHandle(handle)) {
		return false;
	}
	out.slot = slot;
	out.steamId = steam_id;
	out.controllerHandle = handle;
	return true;
}

bool OwnerMatches(const OwnerIdentity& owner) {
	if (!owner.IsSet()) {
		return false;
	}
	OwnerIdentity current;
	return CaptureOwner(owner.slot, current) && current.steamId == owner.steamId &&
		   current.controllerHandle == owner.controllerHandle;
}

} // namespace levels_ranks
