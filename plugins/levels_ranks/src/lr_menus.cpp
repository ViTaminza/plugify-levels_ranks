#include "lr_menus.hpp"

#include <hudmenu/hudmenu.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lr_phrases.hpp"

namespace levels_ranks {
namespace {

constexpr int32_t kSlots = 64;

struct OpenMenu {
	s2sdk::MenuId id = 0;
	OwnerIdentity owner;
	std::vector<MenuItemPlan> items; // index-aligned with the s2sdk item list
	MenuContext context;
	MenuSelectHandler onSelect = nullptr;
	MenuBackHandler onBack = nullptr;
	void* user = nullptr;
};

struct PendingAction {
	enum class Kind {
		Select,
		Back,
	};
	Kind kind = Kind::Select;
	OwnerIdentity owner;
	MenuContext context;
	std::string info;
	int32_t item = 0;
};

struct PendingShow {
	OwnerIdentity owner;
	MenuPlan plan;
};

std::array<OpenMenu, kSlots> g_open;
std::vector<PendingShow> g_shows;
std::vector<s2sdk::MenuId> g_destroy;

// True while a menu handler is running: the engine calls handlers with the menu manager's
// recursive mutex held, so menu/client/schema mutations are queued for ProcessMenuRequests().
bool g_in_handler = false;
// This plugin created the reserved s2sdk menu id 0 (see StartMenus) and must destroy it before
// unloading, or its handler points into unloaded code after `plg reload`.
bool g_reserved_id_zero = false;
// One error line per library instance when the lifecycle bindings are incomplete.
bool g_bindings_error_logged = false;

void ReportMissingBindings() {
	if (g_bindings_error_logged) {
		return;
	}
	g_bindings_error_logged = true;
	LogError("the s2sdk menu bindings are incomplete (create/add/display/get/cancel/destroy, "
			 "set/get type, the display setters or GetClientSteamID64 are missing) - the LR menus "
			 "are disabled");
}

void ClearSlot(int32_t slot) {
	g_open[static_cast<std::size_t>(slot)] = OpenMenu{};
}

bool IsTrackedMenu(s2sdk::MenuId id) {
	for (const OpenMenu& state : g_open) {
		if (state.id == id) {
			return true;
		}
	}
	return false;
}

void QueueMenuDestroy(s2sdk::MenuId id) {
	if (id == 0) {
		return; // s2sdk's "no menu"
	}
	for (const s2sdk::MenuId queued : g_destroy) {
		if (queued == id) {
			return; // Cancel and End both report the same session end
		}
	}
	g_destroy.push_back(id);
}

// Ends every display of `id` with MenuCancelReason::Disconnect (s2sdk skips the menu backend for
// that reason, so no client call happens). Returns true only when no client state references the
// id any more - the precondition for a safe DestroyMenu().
bool EnsureNotDisplayed(s2sdk::MenuId id) {
	if (__s2sdk_GetClientMenu == nullptr || __s2sdk_CancelClientMenu == nullptr) {
		return false;
	}
	for (int32_t slot = 0; slot < kSlots; ++slot) {
		s2sdk::MenuId current = 0;
		try {
			current = s2sdk::GetClientMenu(slot);
		} catch (...) {
			return false;
		}
		if (current != id) {
			continue;
		}
		try {
			s2sdk::CancelClientMenu(slot, s2sdk::MenuCancelReason::Disconnect);
		} catch (...) {
		}
		// Confirm the session is really gone before DestroyMenu may close the backend: this covers
		// both a refused cancel and an engine that reports success without clearing the state.
		try {
			if (s2sdk::GetClientMenu(slot) == id) {
				return false;
			}
		} catch (...) {
			return false;
		}
	}
	return true;
}

// The last-resort map-down teardown: a displayed session that refuses
// CancelClientMenu(Disconnect) is switched to a built-in digit backend whose close callback is a
// no-op (ChatMenu_Close / ConsoleMenu_Close), verified with GetMenuType, then destroyed. This is
// safe with the map down and removes the handler pointer from the manager before unload.
bool ForceSafeDestroy(s2sdk::MenuId id) {
	if (__s2sdk_SetMenuType == nullptr || __s2sdk_GetMenuType == nullptr ||
		__s2sdk_DestroyMenu == nullptr) {
		return false;
	}
	constexpr const char* kSafeTypes[] = {"chat", "console"};
	for (const char* type : kSafeTypes) {
		bool switched = false;
		try {
			switched = s2sdk::SetMenuType(id, plg::string(type));
		} catch (...) {
			switched = false;
		}
		if (!switched) {
			// The menu no longer exists: there is no callback left to unload.
			try {
				if (s2sdk::GetMenuType(id).empty()) {
					return true;
				}
			} catch (...) {
			}
			continue;
		}
		plg::string actual;
		try {
			actual = s2sdk::GetMenuType(id);
		} catch (...) {
			continue;
		}
		if (std::string_view(actual.data(), actual.size()) != type) {
			continue;
		}
		try {
			s2sdk::DestroyMenu(id); // Destroyed -> the builtin no-op close, no client call
			return true;
		} catch (...) {
			return false;
		}
	}
	return false;
}

// Runs outside the menu manager's lock. `allow_backend_close` is true only while the map is up
// (the unload-mid-map path), where DestroyMenu may safely close the backend display. With the gate
// closed the id goes through the confirmed-cancel path first and the builtin-type escape second.
void DestroyQueuedMenus(bool allow_backend_close) {
	if (g_destroy.empty()) {
		return;
	}
	std::vector<s2sdk::MenuId> ids = std::move(g_destroy);
	g_destroy.clear();
	for (const s2sdk::MenuId id : ids) {
		if (IsTrackedMenu(id)) {
			g_destroy.push_back(id); // a current display: ApplyShow/Shutdown owns it
			continue;
		}
		if (__s2sdk_DestroyMenu == nullptr) {
			g_destroy.push_back(id);
			continue;
		}
		const bool safe = id == 0 || EnsureNotDisplayed(id);
		if (!safe) {
			if (allow_backend_close && IsMapReady()) {
				try {
					s2sdk::DestroyMenu(id); // map is up: the backend close is safe
				} catch (...) {
				}
			} else if (!ForceSafeDestroy(id)) {
				g_destroy.push_back(id); // retried on the next update / at map start
			}
			continue;
		}
		try {
			s2sdk::DestroyMenu(id);
		} catch (...) {
			g_destroy.push_back(id);
		}
	}
}

// Must be called outside a handler and only while the map is up (DestroyMenu may close the backend
// display). Clears the slot first: DestroyMenu re-enters the menu manager and calls the handler's
// Cancel/End, which then sees a stale id and does nothing.
void DestroySlotMenu(int32_t slot) {
	const OpenMenu state = g_open[static_cast<std::size_t>(slot)];
	ClearSlot(slot);
	if (state.id != 0 && __s2sdk_DestroyMenu != nullptr) {
		try {
			s2sdk::DestroyMenu(state.id);
		} catch (...) {
		}
	}
}

// The action keeps the function pointers of the display it came from, so a replaced menu cannot
// redirect a selection to a handler of another menu.
struct ActionWithHandlers {
	PendingAction action;
	MenuSelectHandler on_select = nullptr;
	MenuBackHandler on_back = nullptr;
	void* user = nullptr;
};

std::vector<ActionWithHandlers> g_action_queue;

void MenuHandler(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t slot, int32_t param) {
	// Everything the handler asks for goes through the queues (see g_in_handler). The flag is
	// saved/restored rather than cleared, so a re-entering handler does not clear it for the outer
	// one.
	struct HandlerScope {
		bool previous;
		HandlerScope() : previous(g_in_handler) { g_in_handler = true; }
		~HandlerScope() { g_in_handler = previous; }
	} scope;
	try {
		if (!IsSlotInRange(slot)) {
			return;
		}
		OpenMenu& state = g_open[static_cast<std::size_t>(slot)];
		if (state.id != id) {
			return; // stale: the plugin already closed/replaced it
		}
		switch (action) {
		case s2sdk::MenuAction::Select: {
			if (param < 0 || static_cast<std::size_t>(param) >= state.items.size()) {
				return;
			}
			// s2sdk accepts digits 1..7 even when the page shows five items and maps them to
			// currentOffset + digit - 1, so 6/7 select an item of the next page that was never
			// displayed. Pisex numbered the five visible items 1..5.
			int32_t offset = param - param % kMenusPerPage;
			if (__s2sdk_GetClientMenuOffset != nullptr) {
				try {
					offset = s2sdk::GetClientMenuOffset(slot); // read-only, recursive mutex
				} catch (...) {
				}
			}
			const int32_t item = param - offset + 1;
			if (item < 1 || item > kMenusPerPage) {
				return; // digit 6/7 (next page) or a stale index of another page
			}
			const MenuItemPlan& plan = state.items[static_cast<std::size_t>(param)];
			if (plan.disabled) {
				return;
			}
			g_action_queue.push_back(ActionWithHandlers{
				PendingAction{PendingAction::Kind::Select, state.owner, state.context, plan.info, item},
				state.onSelect, state.onBack, state.user});
			break;
		}
		case s2sdk::MenuAction::Cancel: {
			const s2sdk::MenuId menu_id = state.id;
			const OwnerIdentity owner = state.owner;
			const MenuContext context = state.context;
			const MenuSelectHandler state_on_select = state.onSelect;
			const MenuBackHandler state_on_back = state.onBack;
			void* const state_user = state.user;
			ClearSlot(slot);
			QueueMenuDestroy(menu_id);
			// Pisex's exit/back values: the exit button (item 9) was ignored by the original, the
			// exit-back button (item 7) re-opened the previous page; a timeout/disconnect drops it.
			if (static_cast<s2sdk::MenuCancelReason>(param) == s2sdk::MenuCancelReason::ExitBack) {
				g_action_queue.push_back(ActionWithHandlers{
					PendingAction{PendingAction::Kind::Back, owner, context, {}, 0}, state_on_select,
					state_on_back, state_user});
			}
			break;
		}
		case s2sdk::MenuAction::End: {
			const s2sdk::MenuId menu_id = state.id;
			ClearSlot(slot);
			QueueMenuDestroy(menu_id);
			break;
		}
		case s2sdk::MenuAction::Start:
			break;
		}
	} catch (...) {
		// Never unwind into the engine; the manager's lock must be released cleanly.
	}
}

void ApplyActionWithHandlers(const ActionWithHandlers& entry) {
	const PendingAction& action = entry.action;
	if (!IsMapReady() || !OwnerMatches(action.owner)) {
		return; // stale owner: never apply to the current slot occupant
	}
	if (action.context.target.IsSet() && !OwnerMatches(action.context.target)) {
		return; // the target left / the slot was re-used
	}
	try {
		if (action.kind == PendingAction::Kind::Select) {
			if (entry.on_select != nullptr) {
				entry.on_select(entry.user, action.owner, action.context, action.info, action.item);
			}
		} else if (entry.on_back != nullptr) {
			entry.on_back(entry.user, action.owner, action.context);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("a queued menu action threw: ") + ex.what());
	} catch (...) {
		LogError("a queued menu action threw an unknown exception");
	}
}

void ApplyShow(PendingShow& show) {
	if (!IsMapReady()) {
		return;
	}
	if (!MenuBindingsAvailable()) {
		ReportMissingBindings();
		return;
	}
	if (!OwnerMatches(show.owner)) {
		return; // the request's player left / the slot was re-used: drop it
	}
	if (show.plan.context.target.IsSet() && !OwnerMatches(show.plan.context.target)) {
		return;
	}
	DestroySlotMenu(show.owner.slot); // replace whatever this client had open

	s2sdk::MenuId id = 0;
	try {
		id = s2sdk::CreateMenu(plg::string(show.plan.title), &MenuHandler,
							   plg::string(ResolveMenuTypeName(show.owner.slot)));
	} catch (...) {
		id = 0;
	}
	if (id == 0) {
		// Only possible when the id-0 reservation did not happen (or the host ignored it). 0 is
		// both a real id and s2sdk's "no menu": the menu cannot be tracked or safely destroyed
		// without risking another plugin's id-0 menu, so it is left alone.
		LogError("s2sdk returned MenuId 0 - the LR menu was not displayed");
		return;
	}

	OpenMenu& state = g_open[static_cast<std::size_t>(show.owner.slot)];
	state.id = id;
	state.owner = show.owner;
	state.context = show.plan.context;
	state.onSelect = show.plan.onSelect;
	state.onBack = show.plan.onBack;
	state.user = show.plan.user;
	for (const MenuItemPlan& item : show.plan.items) {
		int32_t index = -1;
		try {
			index = s2sdk::AddMenuItem(id, plg::string(item.info), plg::string(item.text),
									   item.disabled ? s2sdk::MenuItemStyle::Disabled
													 : s2sdk::MenuItemStyle::Default);
		} catch (...) {
			index = -1;
		}
		if (index >= 0) {
			state.items.push_back(item); // index-aligned with the s2sdk item list
		}
	}
	try {
		s2sdk::SetMenuPagination(id, show.plan.itemsPerPage);
		s2sdk::SetMenuExitButton(id, show.plan.exitButton);
		s2sdk::SetMenuExitBackButton(id, show.plan.exitBackButton);
		// Pisex kept the menu open after a selection; the callback decides what happens next.
		s2sdk::SetMenuCloseOnSelect(id, false);
	} catch (...) {
	}

	bool displayed = false;
	try {
		displayed = s2sdk::DisplayMenu(id, show.owner.slot, show.plan.timeoutSeconds);
	} catch (...) {
		displayed = false;
	}
	if (!displayed) {
		LogError("s2sdk could not display the LR menu");
		DestroySlotMenu(show.owner.slot);
	}
}

// Must be called outside the menu manager's lock (from OnPluginUpdate). Every tracked menu is
// cleared and queued; the queue drains with the safe-cancel/type-escape path used on unload.
void CloseAllNoMap() {
	g_action_queue.clear();
	g_shows.clear();
	for (int32_t slot = 0; slot < kSlots; ++slot) {
		const s2sdk::MenuId id = g_open[static_cast<std::size_t>(slot)].id;
		if (id == 0) {
			continue;
		}
		ClearSlot(slot);
		QueueMenuDestroy(id);
	}
	DestroyQueuedMenus(false);
	// A Cancel handler runs inside the drain above and may have queued a back/selection action;
	// with the map going down that action must not survive into the next map's first update.
	g_action_queue.clear();
}

bool IsHudRegistered() {
	if (__s2sdk_IsMenuTypeRegistered == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsMenuTypeRegistered(plg::string("hud"));
	} catch (...) {
		return false;
	}
}

bool IsWasdRegistered() {
	if (__s2sdk_IsMenuTypeRegistered == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsMenuTypeRegistered(plg::string("wasd"));
	} catch (...) {
		return false;
	}
}

// The player's own "!menu" choice from the optional hudmenu plugin. Empty when hudmenu is not
// installed, the slot has no live client or anything went wrong.
std::string PlayerTypeChoice(int32_t slot) {
	if (__hudmenu_GetClientMenuType == nullptr || !IsMapReady()) {
		return {};
	}
	try {
		const plg::string answer = hudmenu::GetClientMenuType(slot);
		return std::string(answer.data(), answer.size());
	} catch (...) {
		return {};
	}
}

void ReleaseReservedMenuId() {
	if (!g_reserved_id_zero) {
		return;
	}
	g_reserved_id_zero = false;
	if (__s2sdk_DestroyMenu == nullptr) {
		return;
	}
	try {
		s2sdk::DestroyMenu(0); // never displayed (0 is "no menu"), so no backend close
	} catch (...) {
	}
}

// Pisexcompat's ResolveS2sdkMenuTypeName for a slot that never forces a type: the player's own
// "!menu" choice wins, otherwise the plugin default HUD_LAYOUT maps to the "hud" backend when it
// is registered and to the built-in "button" type otherwise.
std::string_view ResolveTypeName(std::string_view player_choice, bool hud_registered,
								 bool wasd_registered) {
	if (!player_choice.empty()) {
		if (player_choice == "chat" || player_choice == "centerhtml" || player_choice == "button") {
			return player_choice;
		}
		if (player_choice == "hud") {
			return hud_registered ? std::string_view("hud") : std::string_view("button");
		}
		if (player_choice == "wasd") {
			return wasd_registered ? std::string_view("wasd") : std::string_view("button");
		}
	}
	return hud_registered ? std::string_view("hud") : std::string_view("button");
}

} // namespace

bool MenuBindingsAvailable() {
	// The whole lifecycle and the owner identity, before ANY menu is registered/created:
	//   create/add/display  - the menu itself,
	//   get/cancel/destroy  - the only safe teardown path (s2sdk never destroys a menu itself),
	//   set/get menu type   - the map-down escape to a builtin backend with a no-op close,
	//   the display setters - the Pisex pagination/exit/back/close-on-select behaviour,
	//   GetClientSteamID64  - the owner identity every queued request is re-validated against.
	return __s2sdk_CreateMenu != nullptr && __s2sdk_AddMenuItem != nullptr &&
		   __s2sdk_DisplayMenu != nullptr && __s2sdk_GetClientMenu != nullptr &&
		   __s2sdk_CancelClientMenu != nullptr && __s2sdk_DestroyMenu != nullptr &&
		   __s2sdk_SetMenuPagination != nullptr && __s2sdk_SetMenuExitButton != nullptr &&
		   __s2sdk_SetMenuExitBackButton != nullptr && __s2sdk_SetMenuCloseOnSelect != nullptr &&
		   __s2sdk_SetMenuType != nullptr && __s2sdk_GetMenuType != nullptr &&
		   __s2sdk_GetClientSteamID64 != nullptr;
}

void StartMenus() {
	g_action_queue.clear();
	g_shows.clear();
	g_destroy.clear();
	for (OpenMenu& state : g_open) {
		state = OpenMenu{};
	}
	g_in_handler = false;
	g_bindings_error_logged = false; // one line per plugin instance

	if (!MenuBindingsAvailable()) {
		// No reserved id 0 either: without the teardown bindings no menu of this plugin may exist.
		ReportMissingBindings();
		return;
	}

	// s2sdk's MenuManager starts ids at 0 but also uses 0 for "no menu", so the first menu created
	// in the process could never be displayed/tracked. Create and keep one dummy menu if we got
	// id 0 (cheap; done once per process by whichever plugin runs first).
	if (g_reserved_id_zero) {
		return;
	}
	try {
		const s2sdk::MenuId id =
			s2sdk::CreateMenu(plg::string("levels_ranks:reserved-id-0"), &MenuHandler, plg::string("chat"));
		if (id != 0) {
			s2sdk::DestroyMenu(id); // id 0 was already taken elsewhere: nothing to keep
		} else {
			g_reserved_id_zero = true;
		}
	} catch (...) {
	}
}

bool EnqueueMenu(const OwnerIdentity& owner, MenuPlan plan) {
	if (!owner.IsSet() || !IsMapReady()) {
		return false; // no verified owner / no map: nothing to show
	}
	if (!MenuBindingsAvailable()) {
		ReportMissingBindings();
		return false;
	}
	if (!OwnerMatches(owner)) {
		return false; // the player left or the slot was re-used between menu build and queueing
	}
	if (plan.context.target.IsSet() && !OwnerMatches(plan.context.target)) {
		return false;
	}
	// Reuse the slot's queued show instead of piling up replacements (a fast second selection).
	for (PendingShow& queued : g_shows) {
		if (queued.owner.slot == owner.slot) {
			queued.owner = owner;
			queued.plan = std::move(plan);
			return true;
		}
	}
	g_shows.push_back(PendingShow{owner, std::move(plan)});
	return true;
}

void ProcessMenuRequests() {
	if (g_in_handler) {
		return; // never called from a handler; the queue is drained from OnPluginUpdate
	}
	if (!MenuBindingsAvailable()) {
		g_action_queue.clear();
		g_shows.clear();
		g_destroy.clear();
		return;
	}
	if (!IsMapReady()) {
		// The changelevel window: no client, menu display or schema call. Ended menus are still
		// retried through the safe Disconnect/destroy path so they do not stay in the manager.
		g_action_queue.clear();
		g_shows.clear();
		DestroyQueuedMenus(false);
		return;
	}

	// Actions first: a selection stores the cookie, applies the FOV and re-opens the menu (which
	// only queues a new show). Then ended menus are freed, then queued shows are created - all
	// outside the menu manager's lock.
	std::vector<ActionWithHandlers> actions = std::move(g_action_queue);
	g_action_queue.clear();
	for (const ActionWithHandlers& action : actions) {
		ApplyActionWithHandlers(action);
	}
	DestroyQueuedMenus(false);

	std::vector<PendingShow> shows = std::move(g_shows);
	g_shows.clear();
	for (PendingShow& show : shows) {
		ApplyShow(show);
	}
	DestroyQueuedMenus(false);
}

void CloseMenusOnMapEnd() {
	if (!MenuBindingsAvailable()) {
		g_action_queue.clear();
		g_shows.clear();
		g_destroy.clear();
		for (OpenMenu& state : g_open) {
			state = OpenMenu{};
		}
		return;
	}
	CloseAllNoMap();
}

void ShutdownMenus() {
	g_action_queue.clear();
	g_shows.clear();
	if (IsMapReady() && MenuBindingsAvailable()) {
		// Mid-map unload (`plg reload`): DestroyMenu closes the backend display, which is safe
		// while the map and the client list exist - and it hides the HUD menu properly.
		for (int32_t slot = 0; slot < kSlots; ++slot) {
			if (g_open[static_cast<std::size_t>(slot)].id != 0) {
				DestroySlotMenu(slot);
			}
		}
		DestroyQueuedMenus(true);
	} else {
		// Map down (or bindings incomplete): cancel-confirmed first, then the builtin no-op-close
		// type escape; a menu neither path can remove is reported as A7_UNRESOLVED.
		CloseAllNoMap();
		if (!g_destroy.empty()) {
			DestroyQueuedMenus(false);
			if (!g_destroy.empty()) {
				LogError("A7_UNRESOLVED: could not unload a menu safely (cancel unconfirmed and "
						 "the builtin-type fallback failed) - it stays queued instead of forcing an "
						 "unsafe backend close");
			}
		}
	}
	ReleaseReservedMenuId();
	g_action_queue.clear();
	g_shows.clear();
	g_in_handler = false;
}

std::string ResolveMenuTypeName(int32_t slot) {
	const bool hud_registered = IsHudRegistered();
	const bool wasd_registered = IsWasdRegistered();
	const std::string player_choice = IsMapReady() ? PlayerTypeChoice(slot) : std::string();
	return std::string(ResolveTypeName(player_choice, hud_registered, wasd_registered));
}

bool IsMenuOpen(int32_t slot) {
	if (!IsSlotInRange(slot)) {
		return false;
	}
	const s2sdk::MenuId id = g_open[static_cast<std::size_t>(slot)].id;
	if (id == 0 || !IsMapReady() || __s2sdk_GetClientMenu == nullptr) {
		return false;
	}
	try {
		return s2sdk::GetClientMenu(slot) == id;
	} catch (...) {
		return false;
	}
}

} // namespace levels_ranks
