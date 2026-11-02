// levels_ranks runtime tests — no engine and no Plugify host. The plugin's own safety/module layer
// (src/lr_gate.cpp, src/lr_menus.cpp, src/lr_commands.cpp) is linked and every __s2sdk_* /
// __hudmenu_* binding slot is a recording fake.
//
// Covered:
//   * PITFALLS B1/B2 — the map gate: nothing opens until OnMapStart, the mid-map latch works only
//     on the first update, with all four map-listener bindings, no OnMapEnd since start and a real
//     GetCurrentMap(); OnMapEnd closes it and only a real OnMapStart re-opens it; not one client,
//     entity or schema call runs while the gate is shut;
//   * PITFALLS C1/C2 — slot range, connected/in-game/SourceTV, controller pointer+handle,
//     0/-1 invalid versus a negative (serial bit 31) valid handle, "<unknown>" names, the team
//     schema offset cached only after a successful lookup;
//   * PITFALLS C10/A7 — the native menu lifecycle: id 0 reserved, five items per page, disabled
//     items not selectable, digit 6/7 of an unseen page ignored, exit/back/timeout, every ended
//     display destroyed from ProcessMenuRequests (never from the handler), owner SteamID64 +
//     controller-handle revalidation, the map-end CancelClientMenu(Disconnect) + GetClientMenu
//     recheck + SetMenuType/GetMenuType fallback and the A7_UNRESOLVED report;
//   * PITFALLS C11/A7 — console versus chat contexts, trigger-less say dispatch, server-only
//     commands accepting exactly caller -1, ownership-aware RemoveCommand/RemoveCommandListener
//     teardown, and a clean reload.
//
// Run by ./test.sh (build/tests/lr_runtime_tests).

#include <hudmenu/hudmenu.hpp>
#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lr_commands.hpp"
#include "lr_gate.hpp"
#include "lr_menus.hpp"
#include "lr_phrases.hpp"

// The plugify globals a plugin defines through PLUGIFY_PLUGIN. Only the logger and the directory
// getters are touched by the linked modules; the tests provide them.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

using namespace levels_ranks;

int g_checks = 0;
int g_failures = 0;
std::vector<std::string> g_logs;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const std::string& what) {
	Check(actual == expected,
		  what + " (got " + std::to_string(actual) + ", want " + std::to_string(expected) + ")");
}

void CheckEqStr(std::string_view actual, std::string_view expected, const std::string& what) {
	Check(actual == expected,
		  what + " (got \"" + std::string(actual) + "\", want \"" + std::string(expected) + "\")");
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

plg::string ToPlg(std::string_view text) {
	return plg::string(text.data(), text.size());
}

void FakeLog(std::string_view message, plg::Severity /*severity*/, const plg::source_location& /*loc*/) {
	g_logs.emplace_back(message);
}

bool Logged(const std::string& needle) {
	for (const std::string& line : g_logs) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

plg::vector<plg::string> Args(std::initializer_list<const char*> words) {
	plg::vector<plg::string> args;
	for (const char* word : words) {
		args.emplace_back(word);
	}
	return args;
}

void ResetLogs() {
	g_logs.clear();
}

// --- the fake map lifecycle -------------------------------------------------------------------

s2sdk::OnMapStartCallback g_on_map_start = nullptr;
s2sdk::OnMapEndCallback g_on_map_end = nullptr;
int g_map_start_registers = 0;
int g_map_start_unregisters = 0;
int g_map_end_registers = 0;
int g_map_end_unregisters = 0;
std::string g_map_name = "de_dust2";

plg::string FakeGetCurrentMap() {
	return ToPlg(g_map_name);
}

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_on_map_start = callback;
	++g_map_start_registers;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_on_map_start == callback) {
		g_on_map_start = nullptr;
	}
	++g_map_start_unregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_on_map_end = callback;
	++g_map_end_registers;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_on_map_end == callback) {
		g_on_map_end = nullptr;
	}
	++g_map_end_unregisters;
}

void EngineMapStart() {
	if (g_on_map_start != nullptr) {
		g_on_map_start();
	}
}

void EngineMapEnd() {
	if (g_on_map_end != nullptr) {
		g_on_map_end();
	}
}

// --- the fake client/entity/schema layer ------------------------------------------------------

struct FakeClient {
	bool connected = false;
	bool in_game = false;
	bool source_tv = false;
	bool controller = false;
	bool pawn = false;
	int32_t handle = 7; // CEntityHandle; 0/-1 invalid, a negative serial is valid
	uint64_t steamid = 0;
	std::string name;
	std::string auth = "[U:1:100]";
	int team = 2;
	int health = 100;
};

std::array<FakeClient, 64> g_clients;
std::set<void*> g_valid_pointers;
std::set<int32_t> g_valid_handles;
std::array<void*, 64> g_controller_pointer{};
int g_client_calls = 0;
int g_schema_lookups = 0;
int32_t g_team_offset = 10;

FakeClient& ClientAt(int32_t slot) {
	static FakeClient empty;
	if (slot < 0 || slot >= static_cast<int32_t>(g_clients.size())) {
		return empty;
	}
	return g_clients[static_cast<std::size_t>(slot)];
}

void ResetClients() {
	for (FakeClient& client : g_clients) {
		client = FakeClient{};
	}
	for (auto& pointer : g_controller_pointer) {
		pointer = nullptr;
	}
	g_valid_pointers.clear();
	g_valid_handles.clear();
	g_client_calls = 0;
	g_schema_lookups = 0;
}

void MakeLiveClient(int32_t slot, uint64_t steamid, std::string name, int32_t handle = 7) {
	FakeClient& client = g_clients[static_cast<std::size_t>(slot)];
	client.connected = true;
	client.in_game = true;
	client.controller = true;
	client.pawn = true;
	client.handle = handle;
	client.steamid = steamid;
	client.name = std::move(name);
	// A stable per-slot address; IsValidEntPointer answers true while the slot's controller lives.
	g_controller_pointer[static_cast<std::size_t>(slot)] =
		reinterpret_cast<void*>(0x100000 + static_cast<uintptr_t>(slot) * 0x100);
	g_valid_pointers.insert(g_controller_pointer[static_cast<std::size_t>(slot)]);
	g_valid_handles.insert(handle);
}

bool FakeIsClientConnected(int32_t slot) {
	++g_client_calls;
	return ClientAt(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_client_calls;
	return ClientAt(slot).in_game;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_client_calls;
	return ClientAt(slot).source_tv;
}

bool FakeIsClientAuthorized(int32_t slot) {
	++g_client_calls;
	return ClientAt(slot).connected;
}

bool FakeIsFakeClient(int32_t slot) {
	++g_client_calls;
	return ClientAt(slot).steamid == 0;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_client_calls;
	return ClientAt(slot).controller ? g_controller_pointer[static_cast<std::size_t>(slot)] : nullptr;
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++g_client_calls;
	return ClientAt(slot).controller ? ClientAt(slot).handle : -1;
}

bool FakeIsValidEntHandle(int32_t handle) {
	++g_client_calls;
	return g_valid_handles.count(handle) != 0;
}

bool FakeIsValidEntPointer(void* pointer) {
	++g_client_calls;
	return g_valid_pointers.count(pointer) != 0;
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_client_calls;
	return ClientAt(slot).steamid;
}

uint32_t FakeGetClientAccountId(int32_t slot) {
	++g_client_calls;
	return static_cast<uint32_t>(ClientAt(slot).steamid % 1000000);
}

plg::string FakeGetClientName(int32_t slot) {
	++g_client_calls;
	return ToPlg(ClientAt(slot).name);
}

plg::string FakeGetClientAuthId(int32_t slot) {
	++g_client_calls;
	return ToPlg(ClientAt(slot).auth);
}

void* FakeGetClientPawn(int32_t slot) {
	++g_client_calls;
	return ClientAt(slot).pawn ? reinterpret_cast<void*>(0x200000 + static_cast<uintptr_t>(slot)) : nullptr;
}

int32_t FakeGetClientHealth(int32_t slot) {
	++g_client_calls;
	return ClientAt(slot).health;
}

int32_t FakeGetSchemaOffset(const plg::string& class_name, const plg::string& field_name) {
	++g_client_calls;
	++g_schema_lookups;
	if (class_name == "CBaseEntity" && field_name == "m_iTeamNum") {
		return g_team_offset;
	}
	return -1;
}

int64_t FakeGetEntData2(void* /*entity*/, int32_t offset, int32_t /*size*/) {
	++g_client_calls;
	return offset == g_team_offset ? 3 : 0;
}

// --- the fake menu manager --------------------------------------------------------------------

struct FakeMenu {
	std::string title;
	std::string type = "chat";
	std::vector<std::pair<std::string, std::string>> items; // info, display
	std::vector<s2sdk::MenuItemStyle> styles;
	int32_t pagination = 0;
	bool exit_button = false;
	bool exit_back = false;
	bool close_on_select = true;
	s2sdk::MenuHandlerCallback handler = nullptr;
	bool alive = true;
};

std::vector<FakeMenu> g_menus;
s2sdk::MenuId g_next_menu_id = 0;
std::array<s2sdk::MenuId, 64> g_displayed{};
std::array<int32_t, 64> g_offset{};
bool g_refuse_cancel = false;
bool g_refuse_set_type = false;
bool g_hud_registered = false;
std::string g_player_type_choice;
int g_destroy_calls = 0;
std::vector<s2sdk::MenuId> g_destroyed;
int g_menu_handler_calls = 0;
bool g_in_menu_handler = false;

FakeMenu* MenuAt(s2sdk::MenuId id) {
	return id < g_menus.size() ? &g_menus[static_cast<std::size_t>(id)] : nullptr;
}

s2sdk::MenuId FakeCreateMenu(const plg::string& title, s2sdk::MenuHandlerCallback handler,
							 const plg::string& type) {
	const s2sdk::MenuId id = g_next_menu_id++;
	if (id >= g_menus.size()) {
		g_menus.resize(id + 1);
	}
	FakeMenu& menu = g_menus[static_cast<std::size_t>(id)];
	menu.title = ToStd(title);
	menu.type = type.empty() ? std::string("chat") : ToStd(type);
	menu.handler = handler;
	menu.alive = true;
	return id;
}

bool FakeDestroyMenu(s2sdk::MenuId id) {
	FakeMenu* menu = MenuAt(id);
	if (menu == nullptr || !menu->alive) {
		return false;
	}
	++g_destroy_calls;
	g_destroyed.push_back(id);
	// DestroyMenu cancels a viewer through the handler's Cancel/End path.
	for (int32_t slot = 0; slot < 64; ++slot) {
		if (g_displayed[static_cast<std::size_t>(slot)] != id) {
			continue;
		}
		g_displayed[static_cast<std::size_t>(slot)] = 0;
		if (menu->handler != nullptr) {
			++g_menu_handler_calls;
			g_in_menu_handler = true;
			menu->handler(id, s2sdk::MenuAction::Cancel, slot,
						  static_cast<int32_t>(s2sdk::MenuCancelReason::Destroyed));
			menu->handler(id, s2sdk::MenuAction::End, slot, 0);
			g_in_menu_handler = false;
		}
	}
	menu->alive = false;
	return true;
}

int32_t FakeAddMenuItem(s2sdk::MenuId id, const plg::string& info, const plg::string& display,
						s2sdk::MenuItemStyle style) {
	FakeMenu* menu = MenuAt(id);
	if (menu == nullptr) {
		return -1;
	}
	menu->items.emplace_back(ToStd(info), ToStd(display));
	menu->styles.push_back(style);
	return static_cast<int32_t>(menu->items.size()) - 1;
}

bool FakeSetMenuPagination(s2sdk::MenuId id, int32_t items) {
	if (FakeMenu* menu = MenuAt(id)) {
		menu->pagination = items;
		return true;
	}
	return false;
}

bool FakeSetMenuExitButton(s2sdk::MenuId id, bool enabled) {
	if (FakeMenu* menu = MenuAt(id)) {
		menu->exit_button = enabled;
		return true;
	}
	return false;
}

bool FakeSetMenuExitBackButton(s2sdk::MenuId id, bool enabled) {
	if (FakeMenu* menu = MenuAt(id)) {
		menu->exit_back = enabled;
		return true;
	}
	return false;
}

bool FakeSetMenuCloseOnSelect(s2sdk::MenuId id, bool enabled) {
	if (FakeMenu* menu = MenuAt(id)) {
		menu->close_on_select = enabled;
		return true;
	}
	return false;
}

bool FakeDisplayMenu(s2sdk::MenuId id, int32_t slot, int32_t /*time*/) {
	FakeMenu* menu = MenuAt(id);
	if (menu == nullptr || !menu->alive || slot < 0 || slot >= 64) {
		return false;
	}
	g_displayed[static_cast<std::size_t>(slot)] = id;
	return true;
}

bool FakeCancelClientMenu(int32_t slot, s2sdk::MenuCancelReason reason) {
	if (g_refuse_cancel) {
		return false;
	}
	if (slot < 0 || slot >= 64) {
		return false;
	}
	const s2sdk::MenuId id = g_displayed[static_cast<std::size_t>(slot)];
	if (id == 0) {
		return true;
	}
	g_displayed[static_cast<std::size_t>(slot)] = 0;
	FakeMenu* menu = MenuAt(id);
	if (menu != nullptr && menu->handler != nullptr) {
		++g_menu_handler_calls;
		g_in_menu_handler = true;
		menu->handler(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(reason));
		menu->handler(id, s2sdk::MenuAction::End, slot, 0);
		g_in_menu_handler = false;
	}
	return true;
}

s2sdk::MenuId FakeGetClientMenu(int32_t slot) {
	return slot >= 0 && slot < 64 ? g_displayed[static_cast<std::size_t>(slot)] : 0;
}

int32_t FakeGetClientMenuOffset(int32_t slot) {
	return slot >= 0 && slot < 64 ? g_offset[static_cast<std::size_t>(slot)] : 0;
}

bool FakeSetMenuType(s2sdk::MenuId id, const plg::string& type) {
	if (g_refuse_set_type) {
		return false;
	}
	FakeMenu* menu = MenuAt(id);
	if (menu == nullptr || !menu->alive) {
		return false;
	}
	menu->type = ToStd(type);
	return true;
}

plg::string FakeGetMenuType(s2sdk::MenuId id) {
	FakeMenu* menu = MenuAt(id);
	return menu != nullptr && menu->alive ? ToPlg(menu->type) : plg::string();
}

bool FakeIsMenuTypeRegistered(const plg::string& type) {
	return g_hud_registered && ToStd(type) == "hud";
}

void ResetMenus() {
	g_menus.clear();
	g_next_menu_id = 0;
	g_displayed.fill(0);
	g_offset.fill(0);
	g_refuse_cancel = false;
	g_refuse_set_type = false;
	g_hud_registered = false;
	g_player_type_choice.clear();
	g_destroy_calls = 0;
	g_destroyed.clear();
	g_menu_handler_calls = 0;
}

// --- the fake command layer ------------------------------------------------------------------

struct FakeCommand {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	bool owned = true; // AddConsoleCommand answered true
};

std::vector<FakeCommand> g_commands;
std::map<std::string, s2sdk::ConCommandCallback> g_listeners;
std::set<std::string> g_existing_commands;
int g_remove_command_calls = 0;
int g_remove_listener_calls = 0;

void FakeAddConsoleCommand(const plg::string& name, const plg::string& /*description*/,
						   s2sdk::ConVarFlag /*flags*/, s2sdk::ConCommandCallback callback,
						   s2sdk::HookMode /*mode*/, bool* created) {
	const std::string text = ToStd(name);
	const bool owned = g_existing_commands.count(text) == 0;
	g_commands.push_back(FakeCommand{text, callback, owned});
	if (created != nullptr) {
		*created = owned;
	}
}

bool FakeAddConsoleCommandExport(const plg::string& name, const plg::string& description,
								 s2sdk::ConVarFlag flags, s2sdk::ConCommandCallback callback,
								 s2sdk::HookMode mode) {
	bool created = false;
	FakeAddConsoleCommand(name, description, flags, callback, mode, &created);
	return created;
}

bool FakeRemoveCommand(const plg::string& /*name*/, s2sdk::ConCommandCallback /*callback*/) {
	++g_remove_command_calls;
	return true;
}

bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							s2sdk::HookMode /*mode*/) {
	g_listeners[ToStd(name)] = callback;
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback /*callback*/,
							   s2sdk::HookMode /*mode*/) {
	++g_remove_listener_calls;
	g_listeners.erase(ToStd(name));
	return true;
}

s2sdk::ConCommandCallback CommandCallbackFor(const std::string& name) {
	for (const FakeCommand& command : g_commands) {
		if (command.name == name) {
			return command.callback;
		}
	}
	return nullptr;
}

s2sdk::ResultType DispatchCommand(const std::string& name, s2sdk::ConCommandContext context,
								  int32_t caller, std::initializer_list<const char*> words) {
	s2sdk::ConCommandCallback callback = CommandCallbackFor(name);
	if (callback == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	return callback(caller, context, Args(words));
}

// --- binding install --------------------------------------------------------------------------

void InstallMapBindings() {
	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;
	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;
}

void InstallClientBindings() {
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsClientAuthorized = &FakeIsClientAuthorized;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientAccountId = &FakeGetClientAccountId;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientAuthId = &FakeGetClientAuthId;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_GetClientHealth = &FakeGetClientHealth;
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetEntData2 = &FakeGetEntData2;
}

void InstallMenuBindings() {
	__s2sdk_CreateMenu = &FakeCreateMenu;
	__s2sdk_DestroyMenu = &FakeDestroyMenu;
	__s2sdk_AddMenuItem = &FakeAddMenuItem;
	__s2sdk_DisplayMenu = &FakeDisplayMenu;
	__s2sdk_GetClientMenu = &FakeGetClientMenu;
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;
	__s2sdk_SetMenuPagination = &FakeSetMenuPagination;
	__s2sdk_SetMenuExitButton = &FakeSetMenuExitButton;
	__s2sdk_SetMenuExitBackButton = &FakeSetMenuExitBackButton;
	__s2sdk_SetMenuCloseOnSelect = &FakeSetMenuCloseOnSelect;
	__s2sdk_GetClientMenuOffset = &FakeGetClientMenuOffset;
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = &FakeGetMenuType;
	__s2sdk_IsMenuTypeRegistered = &FakeIsMenuTypeRegistered;
}

void InstallCommandBindings() {
	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommandExport;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;
}

void InstallAll() {
	InstallMapBindings();
	InstallClientBindings();
	InstallMenuBindings();
	InstallCommandBindings();
}

// --- the map gate -----------------------------------------------------------------------------

int g_lifecycle_starts = 0;
int g_lifecycle_ends = 0;

void OnLifecycleStart() {
	++g_lifecycle_starts;
}

void OnLifecycleEnd() {
	++g_lifecycle_ends;
}

void ResetGate() {
	ShutdownMapGate();
	g_map_start_registers = 0;
	g_map_start_unregisters = 0;
	g_map_end_registers = 0;
	g_map_end_unregisters = 0;
	g_lifecycle_starts = 0;
	g_lifecycle_ends = 0;
	g_map_name = "de_dust2";
}

void TestGate() {
	std::printf("-- map gate\n");
	ResetGate();
	ResetClients();
	SetMapLifecycleCallbacks(&OnLifecycleStart, &OnLifecycleEnd);
	Check(!IsMapReady(), "the gate starts shut");
	Check(MapListenerBindingsAvailable(), "all four map bindings are present");
	InitMapGate();
	CheckEqInt(g_map_start_registers, 1, "OnMapStart registered");
	CheckEqInt(g_map_end_registers, 1, "OnMapEnd registered");

	// A plugin loaded before the first map: the first update has no name, so no latch.
	g_map_name = "";
	PollMapGate();
	Check(!IsMapReady(), "an empty GetCurrentMap keeps the gate shut");
	g_map_name = "<empty>";
	Check(!IsMapReady(), "the <empty> placeholder keeps the gate shut");

	// The engine's real OnMapStart is the reliable opener.
	EngineMapStart();
	Check(IsMapReady(), "OnMapStart opens the gate");
	CheckEqInt(g_lifecycle_starts, 1, "the plugin map-start hook ran");

	// OnMapEnd closes it and never queries a client.
	EngineMapEnd();
	Check(!IsMapReady(), "OnMapEnd closes the gate");
	CheckEqInt(g_lifecycle_ends, 1, "the plugin map-end hook ran");
	const int calls_after_end = g_client_calls;
	g_map_name = "de_dust2";
	PollMapGate();
	Check(!IsMapReady(), "the poll after OnMapEnd does not re-open the gate");
	CheckEqInt(g_client_calls, calls_after_end, "no client call in the map-change window");

	// The next real OnMapStart re-opens it.
	EngineMapStart();
	Check(IsMapReady(), "the next OnMapStart re-opens the gate");
	CheckEqInt(g_lifecycle_starts, 2, "the map-start hook ran again");

	// Mid-map reload: first update, no OnMapEnd, real map name.
	ResetGate();
	EngineMapEnd(); // no listener now: harmless
	SetMapLifecycleCallbacks(&OnLifecycleStart, &OnLifecycleEnd);
	InitMapGate();
	g_map_name = "de_nuke";
	PollMapGate();
	Check(IsMapReady(), "the first update latches readiness for a mid-map load");
	CheckEqInt(g_lifecycle_starts, 1, "the latched map start reaches the plugin");
	PollMapGate(); // a second update must not re-trigger
	CheckEqInt(g_lifecycle_starts, 1, "the latch runs once");

	// An OnMapEnd seen before the first update forbids the latch.
	ResetGate();
	SetMapLifecycleCallbacks(&OnLifecycleStart, &OnLifecycleEnd);
	InitMapGate();
	EngineMapEnd();
	g_map_name = "de_nuke";
	PollMapGate();
	Check(!IsMapReady(), "no mid-map latch after an OnMapEnd was seen");

	// Shutdown removes the listeners.
	ShutdownMapGate();
	CheckEqInt(g_map_start_unregisters, 1, "OnMapStart unregistered");
	CheckEqInt(g_map_end_unregisters, 1, "OnMapEnd unregistered");
	Check(!IsMapReady(), "the gate is shut after shutdown");
}

void TestGateMissingBinding() {
	std::printf("-- map gate with an incomplete binding set\n");
	ResetGate();
	InstallMapBindings();
	__s2sdk_OnMapEnd_Unregister = nullptr;
	ResetClients();
	Check(!MapListenerBindingsAvailable(), "a missing unregister disables the pair");
	InitMapGate();
	CheckEqInt(g_map_start_registers, 0, "nothing is registered without the complete pair");
	CheckEqInt(g_map_end_registers, 0, "nothing is registered without the complete pair");
	g_map_name = "de_dust2";
	PollMapGate();
	Check(!IsMapReady(), "the gate stays shut without the listener pair");
	ShutdownMapGate();
	InstallMapBindings();
}

// --- client guards ----------------------------------------------------------------------------

void TestClientGuards() {
	std::printf("-- client/entity guards\n");
	ResetGate();
	SetMapLifecycleCallbacks(nullptr, nullptr);
	InstallAll();
	InitMapGate();
	ResetClients();
	EngineMapStart();
	Check(IsMapReady(), "map up");

	// Not connected / not in game / SourceTV / no controller: no s2sdk call may escape.
	Check(!HasController(0), "no client -> no controller");
	FakeClient broken = FakeClient{};
	broken.connected = true;
	broken.in_game = false;
	broken.controller = true;
	g_clients[1] = broken;
	Check(!HasController(1), "not in game -> no controller");
	broken.in_game = true;
	broken.source_tv = true;
	g_clients[2] = broken;
	Check(!HasController(2), "SourceTV -> no controller");

	MakeLiveClient(3, 76561198000000003ULL, "Alice", 7);
	Check(HasController(3), "a live client has a controller");
	CheckEqInt(ClientSteamId64(3), 76561198000000003LL, "SteamID64");
	CheckEqStr(ClientName(3), "Alice", "name");

	// A negative handle (serial bit 31) is valid; 0 and -1 are not.
	g_clients[3].handle = static_cast<int32_t>(0x80000007u);
	g_valid_handles.insert(g_clients[3].handle);
	Check(IsUsableEntityHandle(g_clients[3].handle), "a negative CEntityHandle is valid");
	Check(HasController(3), "a negative controller handle passes the guard");
	g_clients[3].handle = 0;
	Check(!IsUsableEntityHandle(0), "handle 0 is the world and is rejected");
	g_clients[3].handle = -1;
	Check(!IsUsableEntityHandle(-1), "handle -1 is invalid");
	g_clients[3].handle = 7;
	g_valid_handles.insert(7);

	// "<unknown>" is not a player name.
	g_clients[3].name = "<unknown>";
	CheckEqStr(ClientName(3), "", "the s2sdk <unknown> placeholder is empty");

	// Owner identity: SteamID64 + controller handle/generation.
	g_clients[3].name = "Alice";
	OwnerIdentity owner;
	Check(CaptureOwner(3, owner), "the owner is captured");
	Check(owner.IsSet(), "the owner is set");
	Check(OwnerMatches(owner), "the same player still matches");
	g_clients[3].steamid = 76561198000000004ULL;
	Check(!OwnerMatches(owner), "a different SteamID64 is rejected");
	g_clients[3].steamid = owner.steamId;
	g_clients[3].handle = 8;
	g_valid_handles.insert(8);
	Check(!OwnerMatches(owner), "a different controller generation is rejected");
	g_clients[3].handle = owner.controllerHandle;
	Check(OwnerMatches(owner), "the original owner matches again");

	// Team via the controller's m_iTeamNum schema offset; the lookup is cached.
	CheckEqInt(ClientTeam(3), 3, "team read at the schema offset");
	const int lookups = g_schema_lookups;
	CheckEqInt(ClientTeam(3), 3, "team read again");
	CheckEqInt(g_schema_lookups, lookups, "a successful offset is cached");

	// With the gate shut not one client/entity/schema call is made.
	const int calls_before = g_client_calls;
	EngineMapEnd();
	Check(!IsMapReady(), "gate shut");
	Check(!HasController(3), "no controller with the gate shut");
	CheckEqInt(ClientSteamId64(3), 0, "no SteamID64 with the gate shut");
	CheckEqStr(ClientName(3), "", "no name with the gate shut");
	CheckEqInt(ClientTeam(3), 0, "no team with the gate shut");
	CheckEqInt(ClientHealth(3), 0, "no health with the gate shut");
	CheckEqInt(SchemaOffset("CBaseEntity", "m_iTeamNum"), -1, "no schema lookup with the gate shut");
	CheckEqInt(g_client_calls, calls_before, "not one s2sdk client/entity/schema call");

	ShutdownMapGate();
}

// --- admin access -----------------------------------------------------------------------------

MenuItemPlan Item(std::string info, std::string text, bool disabled = false);

struct AdminProbe {
	AdminAccess* access = nullptr;
	bool target_ok = true;
	int give_exp = 0;
	int admin_menu_opens = 0;
};

void AdminProbeSelect(void* user, const OwnerIdentity& owner, const MenuContext& context,
					  const std::string& /*info*/, int32_t /*item*/) {
	auto* probe = static_cast<AdminProbe*>(user);
	// The exact gate GiveTakeValuesSelect uses: the grant must still belong to this owner and the
	// target must still be the verified player the menu was opened for.
	if (!probe->access->Has(owner.slot) || !context.target.IsSet() ||
		!OwnerMatches(context.target) || !probe->target_ok) {
		return;
	}
	++probe->admin_menu_opens;
	++probe->give_exp;
}

bool OpenAdminPage(AdminAccess& access, const OwnerIdentity& owner, MenuPlan plan) {
	if (!access.Has(owner.slot)) {
		return false;
	}
	return EnqueueMenu(owner, std::move(plan));
}

void TestAdminAccess() {
	std::printf("-- admin access is bound to SteamID64 + controller handle\n");
	ResetGate();
	SetMapLifecycleCallbacks(nullptr, nullptr);
	InstallAll();
	InitMapGate();
	ResetClients();
	ResetMenus();
	EngineMapStart();
	MakeLiveClient(0, 76561198000000000ULL, "Admin", 7);
	MakeLiveClient(1, 76561198000000001ULL, "Target", 8);
	StartMenus();

	AdminAccess access;
	OwnerIdentity owner;
	OwnerIdentity target;
	Check(CaptureOwner(0, owner), "the admin owner is captured");
	Check(CaptureOwner(1, target), "the target owner is captured");
	Check(access.Grant(0), "the grant is accepted for a verified owner");
	Check(access.Has(0), "the grant is valid");
	access.Revoke(0);
	Check(!access.Has(0), "a revoked grant is invalid");

	Check(access.Grant(0), "grant again");
	g_clients[0].steamid = 76561198000000099ULL;
	Check(!access.Has(0), "a re-used slot with another SteamID64 loses the grant");
	g_clients[0].steamid = owner.steamId;
	g_clients[0].handle = 42;
	g_valid_handles.insert(42);
	Check(!access.Has(0), "the same SteamID64 with a new controller handle loses the grant");
	g_clients[0].handle = owner.controllerHandle;
	Check(access.Has(0), "the original owner still has the grant");

	AdminProbe probe;
	probe.access = &access;
	MenuContext context;
	context.target = target;
	MenuPlan plan;
	plan.title = "Give/Take";
	plan.items = {Item("", "Target (0)", true), Item("100", "100")};
	plan.onSelect = &AdminProbeSelect;
	plan.user = &probe;
	plan.context = context;

	Check(OpenAdminPage(access, owner, plan), "the give/take page is queued for the admin");
	ProcessMenuRequests();
	const s2sdk::MenuId displayed = g_displayed[0];
	Check(displayed != 0, "the give/take page is displayed");
	const int menus_before = static_cast<int>(g_menus.size());

	// The grant is revoked between the menu display and the click: the action must not run.
	access.Revoke(0);
	FakeMenu* menu = MenuAt(displayed);
	menu->handler(displayed, s2sdk::MenuAction::Select, 0, 1);
	ProcessMenuRequests();
	CheckEqInt(probe.give_exp, 0, "a revoked grant produces 0 GiveExp calls");
	Check(!access.Has(0), "the revoked grant stays invalid");

	// With the grant revoked, nothing can open the admin page either.
	Check(!OpenAdminPage(access, owner, plan), "no admin page opens without the grant");
	ProcessMenuRequests();
	CheckEqInt(static_cast<int>(g_menus.size()), menus_before, "no admin menu was opened after the revoke");

	// The same at the action queue: a page queued for the old owner is dropped when the slot is
	// re-used before the select runs.
	Check(access.Grant(0), "grant again for the re-use case");
	Check(OwnerMatches(owner), "the owner is still the original one");
	Check(EnqueueMenu(owner, plan), "a page is queued for the original owner");
	g_clients[0].steamid = 76561198000000101ULL;
	g_clients[0].handle = 55;
	g_valid_handles.insert(55);
	ProcessMenuRequests();
	CheckEqInt(static_cast<int>(g_menus.size()), menus_before, "a queued page for a re-used slot is dropped");
	CheckEqInt(probe.give_exp, 0, "a re-used slot produces 0 GiveExp calls");

	g_clients[0].steamid = owner.steamId;
	g_clients[0].handle = owner.controllerHandle;
	access.Revoke(0);
	ShutdownMenus();
	ShutdownMapGate();
}

// --- menus ------------------------------------------------------------------------------------

MenuItemPlan Item(std::string info, std::string text, bool disabled) {
	return MenuItemPlan{std::move(info), std::move(text), disabled};
}

struct MenuEvents {
	int selects = 0;
	int backs = 0;
	std::string info;
	int32_t item = -1;
};

void OnMenuSelect(void* user, const OwnerIdentity& /*owner*/, const MenuContext& /*context*/,
				  const std::string& info, int32_t item) {
	auto* events = static_cast<MenuEvents*>(user);
	++events->selects;
	events->info = info;
	events->item = item;
}

void OnMenuBack(void* user, const OwnerIdentity& /*owner*/, const MenuContext& /*context*/) {
	++static_cast<MenuEvents*>(user)->backs;
}

MenuPlan SamplePlan(void* user, MenuContext context = {}) {
	MenuPlan plan;
	plan.title = "Test Title";
	plan.items = {Item("", "Info row", true), Item("1", "First"), Item("2", "Second")};
	plan.exitButton = true;
	plan.exitBackButton = true;
	plan.onSelect = &OnMenuSelect;
	plan.onBack = &OnMenuBack;
	plan.user = user;
	plan.context = context;
	return plan;
}

void TestMenus() {
	std::printf("-- native menus\n");
	ResetGate();
	SetMapLifecycleCallbacks(nullptr, nullptr);
	InstallAll();
	InitMapGate();
	ResetClients();
	ResetMenus();
	EngineMapStart();
	MakeLiveClient(0, 76561198000000000ULL, "Alice", 7);
	MakeLiveClient(1, 76561198000000001ULL, "Bob", 8);
	MakeLiveClient(2, 76561198000000002ULL, "Carol", 9);

	StartMenus();
	Check(MenuBindingsAvailable(), "the complete menu binding set is present");
	CheckEqInt(static_cast<int64_t>(g_menus.size()), 1, "MenuId 0 is reserved by a dummy menu");
	CheckEqStr(g_menus[0].title, "levels_ranks:reserved-id-0", "the reserved menu title");
	CheckEqStr(g_menus[0].type, "chat", "the reserved menu uses the chat backend");

	OwnerIdentity owner;
	Check(CaptureOwner(0, owner), "owner captured for the menu");

	MenuEvents events;
	Check(EnqueueMenu(owner, SamplePlan(&events)), "the menu is queued");
	CheckEqInt(static_cast<int64_t>(g_menus.size()), 1, "nothing is created before the update");
	ProcessMenuRequests();
	CheckEqInt(static_cast<int64_t>(g_menus.size()), 2, "the real menu got id >= 1");
	FakeMenu& menu = g_menus[1];
	CheckEqStr(menu.title, "Test Title", "title");
	CheckEqStr(menu.type, "button", "no hud backend -> the HUD_LAYOUT fallback is button");
	CheckEqInt(static_cast<int64_t>(menu.items.size()), 3, "all items added");
	Check(menu.styles[0] == s2sdk::MenuItemStyle::Disabled, "info rows are Disabled");
	Check(menu.styles[1] == s2sdk::MenuItemStyle::Default, "action rows are Default");
	CheckEqInt(menu.pagination, 5, "five items per page");
	Check(menu.exit_button, "exit button on");
	Check(menu.exit_back, "exit-back button on");
	Check(!menu.close_on_select, "close-on-select off, the callback re-opens");
	CheckEqInt(g_displayed[0], 1, "the menu is displayed to the owner");

	// Selecting the visible item 1 dispatches only after ProcessMenuRequests.
	menu.handler(1, s2sdk::MenuAction::Select, 0, 1);
	CheckEqInt(events.selects, 0, "the handler only queues the action");
	ProcessMenuRequests();
	CheckEqInt(events.selects, 1, "the action ran outside the handler");
	CheckEqStr(events.info, "1", "the Pisex back value reaches the callback");
	CheckEqInt(events.item, 2, "the visible item number (index 1 -> item 2)");
	CheckEqInt(g_destroy_calls, 0, "a still-displayed menu is not destroyed");

	// Disabled items cannot be selected.
	menu.handler(1, s2sdk::MenuAction::Select, 0, 0);
	ProcessMenuRequests();
	CheckEqInt(events.selects, 1, "a disabled row is ignored");

	// Digits 6/7 beyond the five visible items are ignored (offset 0).
	menu.handler(1, s2sdk::MenuAction::Select, 0, 5);
	ProcessMenuRequests();
	CheckEqInt(events.selects, 1, "item 6 of a five-item page is ignored");

	// Exit-back -> the back handler.
	menu.handler(1, s2sdk::MenuAction::Cancel, 0, static_cast<int32_t>(s2sdk::MenuCancelReason::ExitBack));
	CheckEqInt(events.backs, 0, "back is queued");
	ProcessMenuRequests();
	CheckEqInt(events.backs, 1, "the back handler ran");
	CheckEqInt(g_displayed[0], 0, "the display is over");
	CheckEqInt(g_destroy_calls, 1, "the ended menu was destroyed outside the handler");
	CheckEqInt(static_cast<int64_t>(g_destroyed.size()), 1, "exactly one DestroyMenu");
	CheckEqInt(g_destroyed[0], 1, "the real menu was destroyed");

	// The handler never calls DestroyMenu itself (g_in_menu_handler is false there).
	Check(!g_in_menu_handler, "no menu call from inside the handler");

	// Exit/timeout/disconnect: no callback, but the menu is destroyed.
	Check(EnqueueMenu(owner, SamplePlan(&events)), "queue again");
	ProcessMenuRequests();
	const s2sdk::MenuId second = g_displayed[0];
	Check(second != 0, "second menu displayed");
	FakeMenu* second_menu = MenuAt(second);
	second_menu->handler(second, s2sdk::MenuAction::Cancel, 0,
						 static_cast<int32_t>(s2sdk::MenuCancelReason::Timeout));
	ProcessMenuRequests();
	CheckEqInt(events.selects, 1, "a timeout does not select");
	CheckEqInt(events.backs, 1, "a timeout does not go back");
	CheckEqInt(g_destroy_calls, 2, "the timed-out menu was destroyed");

	// Owner revalidation: the slot is re-used before the queued show runs.
	Check(EnqueueMenu(owner, SamplePlan(&events)), "queue for Alice");
	g_clients[0].steamid = 76561198000000099ULL;
	g_clients[0].handle = 77;
	g_valid_handles.insert(77);
	const int menus_before = static_cast<int>(g_menus.size());
	ProcessMenuRequests();
	CheckEqInt(static_cast<int>(g_menus.size()), menus_before, "a stale owner does not create a menu");
	g_clients[0].steamid = owner.steamId;
	g_clients[0].handle = owner.controllerHandle;

	// Owner revalidation of a queued action: an action queued for Alice is dropped after a
	// different player takes the slot.
	Check(EnqueueMenu(owner, SamplePlan(&events)), "queue an action");
	ProcessMenuRequests();
	const s2sdk::MenuId third = g_displayed[0];
	FakeMenu* third_menu = MenuAt(third);
	third_menu->handler(third, s2sdk::MenuAction::Select, 0, 1); // queue
	g_clients[0].steamid = 76561198000000100ULL;
	g_clients[0].handle = 78;
	g_valid_handles.insert(78);
	ProcessMenuRequests();
	CheckEqInt(events.selects, 1, "the action of a re-used slot was dropped");
	g_clients[0].steamid = owner.steamId;
	g_clients[0].handle = owner.controllerHandle;

	// The optional hudmenu choice decides the type; "hud" without the backend -> button.
	g_player_type_choice = "hud";
	__hudmenu_GetClientMenuType = [](int32_t) -> plg::string { return ToPlg(g_player_type_choice); };
	CheckEqStr(ResolveMenuTypeName(0), "button", "hud choice without the hud backend -> button");
	g_hud_registered = true;
	CheckEqStr(ResolveMenuTypeName(0), "hud", "hud choice with the backend -> hud");
	g_player_type_choice = "chat";
	CheckEqStr(ResolveMenuTypeName(0), "chat", "the chat choice is passed through");
	g_player_type_choice = "wasd";
	CheckEqStr(ResolveMenuTypeName(0), "button", "wasd without the backend -> button");
	g_player_type_choice.clear();
	__hudmenu_GetClientMenuType = nullptr;

	ShutdownMenus();
	CheckEqInt(static_cast<int64_t>(g_destroyed.size()), g_destroy_calls, "every destroyed menu is recorded");
	ShutdownMapGate();
}

void TestMenuMapEndTeardown() {
	std::printf("-- menu map-end / unload teardown\n");
	ResetGate();
	SetMapLifecycleCallbacks(nullptr, nullptr);
	InstallAll();
	InitMapGate();
	ResetClients();
	ResetMenus();
	EngineMapStart();
	MakeLiveClient(0, 76561198000000000ULL, "Alice", 7);
	StartMenus();

	OwnerIdentity owner;
	Check(CaptureOwner(0, owner), "owner captured");
	MenuEvents events;
	Check(EnqueueMenu(owner, SamplePlan(&events)), "queue");
	ProcessMenuRequests();
	const s2sdk::MenuId id = g_displayed[0];
	Check(id != 0, "menu displayed");

	// Map end: CancelClientMenu(Disconnect) + GetClientMenu recheck + DestroyMenu.
	EngineMapEnd();
	CloseMenusOnMapEnd();
	CheckEqInt(g_displayed[0], 0, "the display was ended");
	CheckEqInt(g_destroy_calls, 1, "the displayed menu was destroyed (the reserved id 0 survives a map change)");
	ShutdownMenus();

	// A refused cancel: the builtin chat/console type escape must remove it before unload.
	ResetMenus();
	EngineMapStart();
	ResetClients();
	MakeLiveClient(0, 76561198000000000ULL, "Alice", 7);
	StartMenus();
	Check(CaptureOwner(0, owner), "owner captured again");
	Check(EnqueueMenu(owner, SamplePlan(&events)), "queue again");
	ProcessMenuRequests();
	const s2sdk::MenuId refused = g_displayed[0];
	Check(refused != 0, "menu displayed again");
	g_refuse_cancel = true;
	EngineMapEnd();
	CloseMenusOnMapEnd();
	CheckEqInt(g_displayed[0], 0, "the type escape ended the display");
	CheckEqInt(g_destroy_calls, 1, "the type escape destroyed the real menu");
	FakeMenu* refused_menu = MenuAt(refused);
	Check(refused_menu == nullptr || !refused_menu->alive, "the refused menu is gone");
	ShutdownMenus();

	// A menu neither path can remove is reported as A7_UNRESOLVED at unload.
	ResetMenus();
	ResetLogs();
	EngineMapStart();
	ResetClients();
	MakeLiveClient(0, 76561198000000000ULL, "Alice", 7);
	StartMenus();
	Check(CaptureOwner(0, owner), "owner captured for the unresolved case");
	Check(EnqueueMenu(owner, SamplePlan(&events)), "queue the unresolved case");
	ProcessMenuRequests();
	const s2sdk::MenuId unresolved = g_displayed[0];
	Check(unresolved != 0, "menu displayed for the unresolved case");
	g_refuse_cancel = true;
	g_refuse_set_type = true;
	EngineMapEnd();
	ShutdownMenus();
	Check(Logged("A7_UNRESOLVED"), "the unremovable menu is reported as A7_UNRESOLVED");
	CheckEqInt(g_displayed[0], unresolved, "the unresolved menu was not destroyed unsafely");
	// Clean the fixture: the fake manager is reset for the following tests.
	ResetMenus();
	ShutdownMapGate();
}

// --- commands ---------------------------------------------------------------------------------

int g_server_calls = 0;
int g_menu_calls = 0;
int g_chat_calls = 0;
int32_t g_last_caller = -99;
bool g_last_from_chat = false;

void ServerHandler(void* /*user*/, int32_t caller, bool from_chat, const CommandArgs& /*args*/) {
	++g_server_calls;
	g_last_caller = caller;
	g_last_from_chat = from_chat;
}

void MenuHandlerCommand(void* /*user*/, int32_t caller, bool from_chat, const CommandArgs& /*args*/) {
	++g_menu_calls;
	g_last_caller = caller;
	g_last_from_chat = from_chat;
}

void ChatHandler(void* /*user*/, int32_t caller, bool from_chat, const CommandArgs& /*args*/) {
	++g_chat_calls;
	g_last_caller = caller;
	g_last_from_chat = from_chat;
}

void TestCommands() {
	std::printf("-- console/chat commands\n");
	InstallAll();
	ResetGate();
	SetMapLifecycleCallbacks(nullptr, nullptr);
	InitMapGate();
	ResetClients();
	ResetMenus();
	EngineMapStart();
	g_commands.clear();
	g_listeners.clear();
	g_existing_commands.clear();
	g_remove_command_calls = 0;
	g_remove_listener_calls = 0;
	g_server_calls = 0;
	g_menu_calls = 0;
	g_chat_calls = 0;
	g_last_caller = -99;
	g_last_from_chat = false;

	const CommandSpec specs[] = {
		{"mm_test", false, true, &ServerHandler},
		{"mm_menu", false, false, &MenuHandlerCommand},
		{"!test", true, false, &ChatHandler},
		{"test", true, false, &ChatHandler},
	};
	RegisterCommands(specs, sizeof(specs) / sizeof(specs[0]), nullptr);
	CheckEqInt(static_cast<int64_t>(g_commands.size()), 3, "three unique command names registered");
	Check(g_listeners.count("say") == 1 && g_listeners.count("say_team") == 1,
		  "the trigger-less say/say_team listeners are installed");

	// Server-only command: exactly caller -1.
	auto result = DispatchCommand("mm_test", s2sdk::ConCommandContext::Console, -1, {"mm_test"});
	Check(result == s2sdk::ResultType::Continue, "the server-console call continues");
	CheckEqInt(g_server_calls, 1, "caller -1 reaches the handler");
	CheckEqInt(g_last_caller, -1, "the handler sees -1");
	result = DispatchCommand("mm_test", s2sdk::ConCommandContext::Console, -2, {"mm_test"});
	Check(result == s2sdk::ResultType::Handled, "caller -2 is swallowed");
	CheckEqInt(g_server_calls, 1, "caller -2 does not reach the handler");
	result = DispatchCommand("mm_test", s2sdk::ConCommandContext::Console, 5, {"mm_test"});
	Check(result == s2sdk::ResultType::Handled, "a client caller is swallowed");
	CheckEqInt(g_server_calls, 1, "a client caller does not reach the server command");

	// A client-executable console command answers the console context.
	result = DispatchCommand("mm_menu", s2sdk::ConCommandContext::Console, 5, {"mm_menu"});
	Check(result == s2sdk::ResultType::Continue, "a console command continues");
	CheckEqInt(g_menu_calls, 1, "the client console command ran");
	CheckEqInt(g_last_caller, 5, "with the client slot");

	// A chat name in the Console context falls through (Pisex had separate namespaces).
	result = DispatchCommand("test", s2sdk::ConCommandContext::Console, 5, {"test"});
	Check(result == s2sdk::ResultType::Continue, "chat-only name: console context continues");
	CheckEqInt(g_chat_calls, 0, "chat-only name: console context does not run the handler");

	// The triggered chat path (s2sdk strips the trigger and dispatches it as a command).
	result = DispatchCommand("test", s2sdk::ConCommandContext::Chat, 5, {"test"});
	Check(result == s2sdk::ResultType::Handled, "a chat command is consumed");
	CheckEqInt(g_chat_calls, 1, "the chat handler ran");
	Check(g_last_from_chat, "the handler sees the chat context");

	// The trigger-less say hook.
	auto say = g_listeners["say"];
	result = say(5, s2sdk::ConCommandContext::Chat, Args({"say", "test"}));
	Check(result == s2sdk::ResultType::Handled, "the bare chat word is consumed");
	CheckEqInt(g_chat_calls, 2, "the bare chat handler ran");
	result = say(5, s2sdk::ConCommandContext::Chat, Args({"say", "!test"}));
	Check(result == s2sdk::ResultType::Continue, "a triggered line belongs to s2sdk's path");
	CheckEqInt(g_chat_calls, 2, "no double dispatch for a triggered line");
	result = say(5, s2sdk::ConCommandContext::Chat, Args({"say", "hello"}));
	Check(result == s2sdk::ResultType::Continue, "an unknown word is left alone");

	// Teardown: owned -> RemoveCommand, pre-existing -> RemoveCommandListener.
	g_existing_commands.insert("mm_menu");
	g_commands.clear();
	g_listeners.clear();
	UnregisterCommands(); // clear the first registration without counting it
	g_remove_command_calls = 0;
	g_remove_listener_calls = 0;
	RegisterCommands(specs, sizeof(specs) / sizeof(specs[0]), nullptr);
	UnregisterCommands();
	CheckEqInt(g_remove_command_calls, 2, "two owned names removed with RemoveCommand");
	CheckEqInt(g_remove_listener_calls, 3, "the pre-existing name and say/say_team removed with listeners");
	Check(g_listeners.empty(), "no listener remains after unload");
	UnregisterCommands();
	CheckEqInt(g_remove_command_calls, 2, "a second unload is a no-op");

	// A missing binding pair registers nothing (A7).
	ShutdownMapGate();
	InstallCommandBindings();
	g_commands.clear();
	g_listeners.clear();
	__s2sdk_RemoveCommand = nullptr;
	RegisterCommands(specs, sizeof(specs) / sizeof(specs[0]), nullptr);
	CheckEqInt(static_cast<int64_t>(g_commands.size()), 0, "no command is registered without the pair");
	InstallCommandBindings();
	UnregisterCommands();
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("levels_ranks runtime tests\n");

	plg::Log = &FakeLog;
	InstallAll();
	ResetClients();
	ResetMenus();
	ResetGate();

	TestGate();
	TestGateMissingBinding();
	TestClientGuards();
	TestAdminAccess();
	TestMenus();
	TestMenuMapEndTeardown();
	TestCommands();

	std::printf("lr_runtime_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
