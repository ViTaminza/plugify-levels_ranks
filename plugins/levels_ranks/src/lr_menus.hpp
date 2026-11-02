#pragma once
// levels_ranks — the native s2sdk menu layer (the Pisex cs2-menus tree on the s2sdk menu manager).
//
// The original built a fresh Pisex Menu for every page and the previous port let pisexcompat
// translate it onto s2sdk. This file talks to the s2sdk menu API directly, keeping the visible
// behaviour and the safety rules that s2sdk leaves to the plugin:
//
//   * five items per page (cs2-menus), an exit button, an exit-back button, close-on-select off
//     (the callback re-opens the next page itself) and a 60 s timeout (cs2-menus "MenuTime"),
//   * item texts are plain text (s2sdk's backends do not need cs2-menus' XML escaping),
//   * the menu type follows the player's own "!menu" choice through the optional hudmenu plugin
//     (GetClientMenuType) with the exact pisexcompat mapping (HUD_LAYOUT -> "hud" when the backend
//     is registered, otherwise "button"),
//   * every menu/action request carries the verified OwnerIdentity captured when it was created
//     (SteamID64 + controller handle/generation). ApplyShow/ApplyAction re-capture and compare it
//     (and the optional target identity) before CreateMenu/DisplayMenu or a plugin action; a
//     request whose owner changed is dropped, never applied to a player that re-used the slot,
//   * the complete menu binding set (create/add/display/get/cancel/destroy, the display setters,
//     SetMenuType/GetMenuType and GetClientSteamID64) must be present BEFORE the first
//     registration/create, including the reserved MenuId 0. Without it no menu is created at all,
//   * s2sdk NEVER destroys a menu by itself: every ended display (select/replace, exit, back,
//     timeout, disconnect) is queued and destroyed in ProcessMenuRequests() on a later
//     OnPluginUpdate, outside the menu manager's lock. The handler itself only records an action,
//   * s2sdk uses MenuId 0 for "no menu" but assigns it to the first menu of the process, so
//     StartMenus() burns it with a dummy menu and ShutdownMenus() destroys it before unload,
//   * at map end / unload while the map is down a displayed session is first ended with
//     CancelClientMenu(Disconnect) - s2sdk skips the menu backend for that reason. DestroyMenu is
//     called only after the client state no longer references the id. If the cancel cannot be
//     confirmed, the menu is switched to the built-in "chat"/"console" backend (SetMenuType +
//     GetMenuType verification; their close callbacks are no-ops) and destroyed through it, so no
//     client call happens and no handler pointer stays in the manager (PITFALLS B2/C10, A7). A
//     menu that even that escape cannot remove is reported as A7_UNRESOLVED at unload.

#include <cstdint>
#include <string>
#include <vector>

#include "lr_gate.hpp"

namespace levels_ranks {

inline constexpr int32_t kMenusPerPage = 5;
inline constexpr int32_t kMenuTimeoutSeconds = 60;

struct MenuItemPlan {
	std::string info;  // the Pisex "back" value the callback receives
	std::string text;  // the display text
	bool disabled = false;
};

// The identity a non-menu action applies to (the give/take target). Empty for pages that only
// navigate. Re-validated by the menu layer and again by the plugin before the action runs.
struct MenuContext {
	OwnerIdentity target;
};

using MenuSelectHandler = void (*)(void* user, const OwnerIdentity& owner, const MenuContext& context,
								   const std::string& info, int32_t item);
using MenuBackHandler = void (*)(void* user, const OwnerIdentity& owner, const MenuContext& context);

struct MenuPlan {
	std::string title;
	std::vector<MenuItemPlan> items;
	bool exitButton = true;
	bool exitBackButton = true;
	int32_t itemsPerPage = kMenusPerPage;
	int32_t timeoutSeconds = kMenuTimeoutSeconds;
	MenuContext context;
	MenuSelectHandler onSelect = nullptr;
	MenuBackHandler onBack = nullptr;
	void* user = nullptr;
};

// True when every s2sdk binding the menu lifecycle and the owner identity depend on is present.
bool MenuBindingsAvailable();

// Resets the menu state and burns MenuId 0. Only acts when the full binding set is present;
// touches only the menu manager (no client, no map, no schema).
void StartMenus();

// Queues a display for the next ProcessMenuRequests(); never creates a menu inside the caller, so
// a menu callback cannot re-enter the menu manager. Dropped when the owner is not/no longer
// verified or no map is up.
bool EnqueueMenu(const OwnerIdentity& owner, MenuPlan plan);

// Applies queued menu actions and destroys ended menus; called from OnPluginUpdate.
void ProcessMenuRequests();

// Drops pending work and destroys every menu of this plugin when the map goes down. Displayed
// sessions are ended through CancelClientMenu(Disconnect) first; a menu whose cancel cannot be
// confirmed uses the builtin no-op-close type escape or stays queued for a later safe attempt.
void CloseMenusOnMapEnd();

// Destroys every menu, drops the pending queues and releases the reserved id 0; OnPluginEnd.
void ShutdownMenus();

// The s2sdk menu type this slot's menus are rendered with (see the header comment).
std::string ResolveMenuTypeName(int32_t slot);

// True when this plugin still has a menu displayed to the slot.
bool IsMenuOpen(int32_t slot);

} // namespace levels_ranks
