// levels_ranks lifecycle tests — A1/A7 at the plugin boundary, no engine and no Plugify host.
//
// src/plugin.cpp is compiled into this TU (the same way as_admins/vip_fov tests do), so the real
// OnPluginStart / OnPluginEnd run with recording fakes for every __s2sdk_* binding. Covered:
//   * A1: a start with no bindings at all returns Ok (never Failed), registers nothing, logs and
//     survives OnPluginEnd;
//   * a complete start registers the four map-listener callbacks, the 12 game-event Pre hooks,
//     the two client listeners, the 22 unique console/chat command names and the reserved MenuId
//     0 - and OnPluginEnd takes every one of them back symmetrically (A7);
//   * a reload after OnPluginEnd registers exactly once again and leaves no listener behind;
//   * a partially available API degrades only the affected feature (missing menu bindings create
//     no menu; missing command bindings register no command) and still returns Ok.
//
// Run by ./test.sh (build/tests/lr_lifecycle_tests).

#include "../src/plugin.cpp"

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
std::vector<std::string> g_logs;
std::string g_configs_dir;
std::string g_data_dir;

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

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
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

plg::string FakeConfigsDir() {
	return plg::string(g_configs_dir.c_str());
}

plg::string FakeDataDir() {
	return plg::string(g_data_dir.c_str());
}

plg::string FakeLogsDir() {
	return plg::string((g_data_dir + "/logs").c_str());
}

// --- map listeners ----------------------------------------------------------------------------

int g_map_start_registers = 0;
int g_map_start_unregisters = 0;
int g_map_end_registers = 0;
int g_map_end_unregisters = 0;
s2sdk::OnMapStartCallback g_on_map_start = nullptr;
s2sdk::OnMapEndCallback g_on_map_end = nullptr;

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_on_map_start = callback;
	++g_map_start_registers;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback /*callback*/) {
	++g_map_start_unregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_on_map_end = callback;
	++g_map_end_registers;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback /*callback*/) {
	++g_map_end_unregisters;
}

// --- client listeners -------------------------------------------------------------------------

int g_auth_registers = 0;
int g_auth_unregisters = 0;
int g_disconnect_registers = 0;
int g_disconnect_unregisters = 0;

void FakeAuthRegister(s2sdk::OnClientAuthenticatedCallback /*callback*/) {
	++g_auth_registers;
}

void FakeAuthUnregister(s2sdk::OnClientAuthenticatedCallback /*callback*/) {
	++g_auth_unregisters;
}

void FakeDisconnectRegister(s2sdk::OnClientDisconnectCallback /*callback*/) {
	++g_disconnect_registers;
}

void FakeDisconnectUnregister(s2sdk::OnClientDisconnectCallback /*callback*/) {
	++g_disconnect_unregisters;
}

// --- events -----------------------------------------------------------------------------------

std::vector<std::string> g_hooked;
std::vector<std::string> g_unhooked;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback /*callback*/,
									s2sdk::HookMode mode) {
	Check(mode == s2sdk::HookMode::Pre, "every game event is hooked in Pre mode");
	g_hooked.push_back(ToStd(name));
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback /*callback*/,
									  s2sdk::HookMode /*mode*/) {
	g_unhooked.push_back(ToStd(name));
	return s2sdk::EventHookError::Okay;
}

// --- commands ---------------------------------------------------------------------------------

std::vector<std::string> g_console_commands;
std::vector<std::string> g_command_listeners;
int g_remove_commands = 0;
int g_remove_listeners = 0;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& /*description*/,
						   s2sdk::ConVarFlag flags, s2sdk::ConCommandCallback /*callback*/,
						   s2sdk::HookMode mode) {
	Check(mode == s2sdk::HookMode::Pre, "console commands are hooked in Pre mode");
	Check((static_cast<int64_t>(flags) & static_cast<int64_t>(s2sdk::ConVarFlag::Release)) != 0,
		  "every command carries Release");
	g_console_commands.push_back(ToStd(name));
	return true;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback /*callback*/) {
	++g_remove_commands;
	Check(!name.empty(), "RemoveCommand gets a name");
	return true;
}

bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback /*callback*/,
							s2sdk::HookMode /*mode*/) {
	g_command_listeners.push_back(ToStd(name));
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback /*callback*/,
							   s2sdk::HookMode /*mode*/) {
	++g_remove_listeners;
	(void)name;
	return true;
}

// --- fake clients (owner identity of the admin tests) -----------------------------------------

bool g_client_connected = false;
bool g_client_in_game = false;
bool g_client_source_tv = false;
bool g_client_controller = false;
int32_t g_client_handle = 7;
uint64_t g_client_steamid = 76561198000000000ULL;
void* g_client_pointer = reinterpret_cast<void*>(0x515000);
std::string g_client_name = "Admin";

bool FakeClientConnected(int32_t slot) {
	return slot == 0 && g_client_connected;
}

bool FakeClientInGame(int32_t slot) {
	return slot == 0 && g_client_in_game;
}

bool FakeClientSourceTV(int32_t slot) {
	return slot == 0 && g_client_source_tv;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	return slot == 0 && g_client_controller ? g_client_pointer : nullptr;
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	return slot == 0 && g_client_controller ? g_client_handle : -1;
}

bool FakeIsValidEntHandle(int32_t handle) {
	return handle == g_client_handle && handle != 0 && handle != -1;
}

bool FakeIsValidEntPointer(void* pointer) {
	return pointer == g_client_pointer;
}

plg::string FakeClientName(int32_t slot) {
	return slot == 0 ? plg::string(g_client_name.c_str()) : plg::string();
}

uint64_t FakeGetClientSteamID64(int32_t slot);

void InstallClientBindings() {
	__s2sdk_IsClientConnected = &FakeClientConnected;
	__s2sdk_IsClientInGame = &FakeClientInGame;
	__s2sdk_IsClientSourceTV = &FakeClientSourceTV;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_GetClientName = &FakeClientName;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
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

// --- menus ------------------------------------------------------------------------------------

s2sdk::MenuId g_next_menu_id = 0;
std::vector<s2sdk::MenuId> g_created_menus;
std::vector<s2sdk::MenuId> g_destroyed_menus;

s2sdk::MenuId FakeCreateMenu(const plg::string& /*title*/, s2sdk::MenuHandlerCallback /*handler*/,
							 const plg::string& /*type*/) {
	const s2sdk::MenuId id = g_next_menu_id++;
	g_created_menus.push_back(id);
	return id;
}

bool FakeDestroyMenu(s2sdk::MenuId id) {
	g_destroyed_menus.push_back(id);
	return true;
}

int32_t FakeAddMenuItem(s2sdk::MenuId /*id*/, const plg::string& /*info*/, const plg::string& /*display*/,
						s2sdk::MenuItemStyle /*style*/) {
	return 0;
}

bool FakeDisplayMenu(s2sdk::MenuId /*id*/, int32_t /*slot*/, int32_t /*time*/) {
	return true;
}

s2sdk::MenuId FakeGetClientMenu(int32_t /*slot*/) {
	return 0;
}

bool FakeCancelClientMenu(int32_t /*slot*/, s2sdk::MenuCancelReason /*reason*/) {
	return true;
}

bool FakeSetMenuPagination(s2sdk::MenuId /*id*/, int32_t /*items*/) {
	return true;
}

bool FakeSetMenuExitButton(s2sdk::MenuId /*id*/, bool /*enabled*/) {
	return true;
}

bool FakeSetMenuExitBackButton(s2sdk::MenuId /*id*/, bool /*enabled*/) {
	return true;
}

bool FakeSetMenuCloseOnSelect(s2sdk::MenuId /*id*/, bool /*enabled*/) {
	return true;
}

bool FakeSetMenuType(s2sdk::MenuId /*id*/, const plg::string& /*type*/) {
	return true;
}

plg::string FakeGetMenuType(s2sdk::MenuId /*id*/) {
	return plg::string();
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	return slot == 0 ? g_client_steamid : 0;
}

// --- install / reset ---------------------------------------------------------------------------

void InstallMapBindings() {
	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;
}

void InstallClientListenerBindings() {
	__s2sdk_OnClientAuthenticated_Register = &FakeAuthRegister;
	__s2sdk_OnClientAuthenticated_Unregister = &FakeAuthUnregister;
	__s2sdk_OnClientDisconnect_Register = &FakeDisconnectRegister;
	__s2sdk_OnClientDisconnect_Unregister = &FakeDisconnectUnregister;
}

void InstallEventBindings() {
	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
}

void InstallCommandBindings() {
	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;
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
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = &FakeGetMenuType;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
}

void ResetCounters() {
	g_map_start_registers = 0;
	g_map_start_unregisters = 0;
	g_map_end_registers = 0;
	g_map_end_unregisters = 0;
	g_auth_registers = 0;
	g_auth_unregisters = 0;
	g_disconnect_registers = 0;
	g_disconnect_unregisters = 0;
	g_hooked.clear();
	g_unhooked.clear();
	g_console_commands.clear();
	g_command_listeners.clear();
	g_remove_commands = 0;
	g_remove_listeners = 0;
	g_created_menus.clear();
	g_destroyed_menus.clear();
	g_next_menu_id = 0;
}

void TestStartWithoutBindings() {
	std::printf("-- A1: a start without any binding\n");
	ResetCounters();
	const plg::PluginResult start = g_lr_plugin.OnPluginStart();
	Check(start.GetCode() == plg::PluginCode::Ok, "OnPluginStart returns Ok, never Failed");
	CheckEqInt(static_cast<int64_t>(g_console_commands.size()), 0, "no command registered");
	CheckEqInt(static_cast<int64_t>(g_hooked.size()), 0, "no event hooked");
	CheckEqInt(g_map_start_registers, 0, "no map listener registered");
	CheckEqInt(g_auth_registers, 0, "no auth listener registered");
	CheckEqInt(static_cast<int64_t>(g_created_menus.size()), 0, "no menu created");
	Check(Logged("bindings are incomplete") || Logged("disabled"), "the missing bindings are logged");

	g_lr_plugin.OnPluginEnd();
	Check(true, "OnPluginEnd is safe without a start");
	g_lr_plugin.OnPluginEnd();
	Check(true, "OnPluginEnd is safe twice");
}

void TestFullLifecycle() {
	std::printf("-- A1/A7: full registration and symmetric teardown\n");
	ResetCounters();
	InstallMapBindings();
	InstallClientListenerBindings();
	InstallEventBindings();
	InstallCommandBindings();
	InstallMenuBindings();

	g_lr_plugin.OnPluginStart();
	CheckEqInt(g_map_start_registers, 1, "OnMapStart registered");
	CheckEqInt(g_map_end_registers, 1, "OnMapEnd registered");
	CheckEqInt(g_auth_registers, 1, "OnClientAuthenticated registered");
	CheckEqInt(g_disconnect_registers, 1, "OnClientDisconnect registered");
	CheckEqInt(static_cast<int64_t>(g_hooked.size()), 12, "all twelve game events hooked");
	CheckEqInt(static_cast<int64_t>(g_console_commands.size()), 22,
			   "22 unique console/chat command names");
	CheckEqInt(static_cast<int64_t>(g_command_listeners.size()), 2, "say/say_team listeners");
	CheckEqInt(static_cast<int64_t>(g_created_menus.size()), 1, "MenuId 0 is reserved");

	g_lr_plugin.OnPluginEnd();
	CheckEqInt(g_map_start_unregisters, 1, "OnMapStart unregistered");
	CheckEqInt(g_map_end_unregisters, 1, "OnMapEnd unregistered");
	CheckEqInt(g_auth_unregisters, 1, "OnClientAuthenticated unregistered");
	CheckEqInt(g_disconnect_unregisters, 1, "OnClientDisconnect unregistered");
	CheckEqInt(static_cast<int64_t>(g_unhooked.size()), 12, "all twelve game events unhooked");
	CheckEqInt(g_remove_commands, 22, "every owned command removed");
	CheckEqInt(g_remove_listeners, 2, "the say/say_team listeners removed");
	CheckEqInt(static_cast<int64_t>(g_destroyed_menus.size()), 1, "the reserved MenuId 0 destroyed");

	// A reload starts cleanly and registers exactly once again; the teardown is symmetric again.
	ResetCounters();
	g_lr_plugin.OnPluginStart();
	CheckEqInt(g_map_start_registers, 1, "reload: OnMapStart registered");
	CheckEqInt(static_cast<int64_t>(g_hooked.size()), 12, "reload: events hooked");
	CheckEqInt(static_cast<int64_t>(g_console_commands.size()), 22, "reload: commands registered");
	CheckEqInt(static_cast<int64_t>(g_created_menus.size()), 1, "reload: MenuId 0 reserved");
	g_lr_plugin.OnPluginEnd();
	CheckEqInt(g_map_start_unregisters, 1, "reload: OnMapStart unregistered");
	CheckEqInt(static_cast<int64_t>(g_unhooked.size()), 12, "reload: events unhooked");
	CheckEqInt(g_remove_commands, 22, "reload: commands removed");
	CheckEqInt(static_cast<int64_t>(g_destroyed_menus.size()), 1, "reload: MenuId 0 destroyed");

	// A second OnPluginEnd must not touch anything again.
	g_lr_plugin.OnPluginEnd();
	CheckEqInt(g_remove_commands, 22, "a second OnPluginEnd removes nothing again");
}

void TestAdminBinding() {
	std::printf("-- admin grants follow SteamID64 + controller handle\n");
	ResetCounters();
	InstallMapBindings();
	InstallClientListenerBindings();
	InstallEventBindings();
	InstallCommandBindings();
	InstallMenuBindings();
	InstallClientBindings();
	g_client_connected = true;
	g_client_in_game = true;
	g_client_controller = true;
	g_client_source_tv = false;
	g_client_handle = 7;
	g_client_steamid = 76561198000000000ULL;

	g_lr_plugin.OnPluginStart();
	EngineMapStart();
	Check(IsMapReady(), "the admin test map is up");
	Check(!g_lr_plugin.HasAdminAccess(0), "no grant without mm_lvl_give_admin");

	g_lr_plugin.SetAdminAccess(0, true);
	Check(g_lr_plugin.HasAdminAccess(0), "the grant belongs to the verified owner");
	g_lr_plugin.ShowAdminMenu(0);
	ProcessMenuRequests();
	CheckEqInt(static_cast<int64_t>(g_created_menus.size()), 2,
			   "the admin menu opened for the granted owner (dummy + real)");

	// Revoked before a second open: ShowAdminMenu must not create anything.
	g_lr_plugin.SetAdminAccess(0, false);
	g_lr_plugin.ShowAdminMenu(0);
	ProcessMenuRequests();
	CheckEqInt(static_cast<int64_t>(g_created_menus.size()), 2,
			   "no admin menu after the grant was revoked");

	// A grant followed by a different SteamID64 in the same slot: the queued page is dropped.
	g_lr_plugin.SetAdminAccess(0, true);
	g_lr_plugin.ShowAdminMenu(0);
	g_client_steamid = 76561198000000099ULL;
	ProcessMenuRequests();
	CheckEqInt(static_cast<int64_t>(g_created_menus.size()), 2,
			   "a queued admin page is dropped when the slot is re-used");
	Check(!g_lr_plugin.HasAdminAccess(0), "the re-used slot lost the grant");

	// The same SteamID64 with a new controller generation is a different owner as well.
	g_client_steamid = 76561198000000000ULL;
	g_lr_plugin.SetAdminAccess(0, true);
	g_client_handle = 8;
	Check(!g_lr_plugin.HasAdminAccess(0), "a new controller handle loses the grant");

	EngineMapEnd();
	g_client_connected = false;
	g_client_in_game = false;
	g_client_controller = false;
	g_lr_plugin.OnPluginEnd();
}

void TestPartialApi() {
	std::printf("-- A1: a partial API only disables the affected feature\n");
	ResetCounters();
	InstallMapBindings();
	InstallClientListenerBindings();
	InstallEventBindings();
	InstallCommandBindings();
	InstallMenuBindings();
	// A menu without the teardown/type pair must not create any menu (not even id 0).
	__s2sdk_GetMenuType = nullptr;
	g_lr_plugin.OnPluginStart();
	CheckEqInt(static_cast<int64_t>(g_created_menus.size()), 0, "no menu without the complete set");
	CheckEqInt(static_cast<int64_t>(g_hooked.size()), 12, "the events still work");
	CheckEqInt(static_cast<int64_t>(g_console_commands.size()), 22, "the commands still work");
	g_lr_plugin.OnPluginEnd();
	CheckEqInt(static_cast<int64_t>(g_destroyed_menus.size()), 0, "nothing to destroy");
	InstallMenuBindings();

	// A command layer without RemoveCommand registers nothing (A7) but keeps the rest alive.
	ResetCounters();
	__s2sdk_RemoveCommand = nullptr;
	g_lr_plugin.OnPluginStart();
	CheckEqInt(static_cast<int64_t>(g_console_commands.size()), 0, "no command without the pair");
	CheckEqInt(static_cast<int64_t>(g_hooked.size()), 12, "the events still work");
	g_lr_plugin.OnPluginEnd();
	InstallCommandBindings();
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("levels_ranks lifecycle tests\n");

	g_configs_dir = "/tmp/lr-lifecycle-configs";
	g_data_dir = "/tmp/lr-lifecycle-data";
	std::error_code error;
	std::filesystem::create_directories(g_configs_dir, error);
	std::filesystem::create_directories(g_data_dir + "/levels_ranks", error);
	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeConfigsDir;
	plg::GetDataDir = &FakeDataDir;
	plg::GetLogsDir = &FakeLogsDir;

	TestStartWithoutBindings();
	TestFullLifecycle();
	TestAdminBinding();
	TestPartialApi();

	std::printf("lr_lifecycle_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
