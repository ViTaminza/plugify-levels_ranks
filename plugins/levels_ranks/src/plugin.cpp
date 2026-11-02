// levels_ranks — engine side of the plugin. This file ports levels_ranks.cpp of the Pisex
// cs2-lvl_ranks plugin onto lr_core (settings, ranks, statistics, SQL) and the native s2sdk API.
//
// The port keeps the structure of the original:
//   * LoadConfig()          -> LoadSettingsFile() (the plugin-local KV1 reader fills the core's
//                              Settings through the KeyValuesView adapter below);
//   * AllPluginsLoaded()    -> EnsureCore() + RegisterListeners() + RegisterCommands() +
//                              RegisterGameEvents();
//   * the IServerGameClients ClientPutInServer/ClientDisconnect hooks -> the s2sdk listeners plus
//                              a once-per-second reconcile (s2sdk's OnClientAuthenticated is not
//                              reliable, see perm_connector);
//   * the IUtilsApi event hooks -> s2sdk::HookEvent() with the same handler bodies;
//   * the LRApi methods      -> the exported methods at the bottom of this file.
//
// Game-event fields (P1 fix): every player-typed field (userid/attacker/assister of player_death,
// player_hurt, weapon_fire, bomb_*, hostage_*, round_mvp) is read through EventPlayerSlot()
// (lr_event_fields.hpp), because GetEventInt answers the engine user id, not the 0..63 slot the
// core indexes with. bomb_pickup.userid is a player_pawn field, so a field the primary slot
// accessor cannot resolve falls back to GetEventPlayerPawn + EntPointerToPlayerSlot behind the map
// gate. Numeric fields (round_end.winner, damage, headshot, ...) keep GetEventInt.
//
// Threading: everything runs on the game thread (the plugify pump, s2sdk callbacks, OnPluginUpdate).
// The database is plgdb (one worker thread, results delivered from Poll()).

#include "lr_plugin.hpp"

#include "lr_commands.hpp"
#include "lr_event_fields.hpp"
#include "lr_keyvalues.hpp"
#include "lr_kv1.hpp"
#include "lr_menus.hpp"
#include "lr_paths.hpp"
#include "lr_phrases.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <permissions/permissions.hpp>

namespace levels_ranks {

LrPlugin g_lr_plugin;

namespace {

std::string_view TrimSpaces(std::string_view text) {
	const auto begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string_view::npos) {
		return {};
	}
	const auto end = text.find_last_not_of(" \t\r\n");
	return text.substr(begin, end - begin + 1);
}

// engine->GetPlayerNetworkIDString() + ConvertSteamID() of the original: CS2 reports the SteamID
// in the "[U:1:accountid]" form and the table of the original stores the "STEAM_1:x:y" one. A
// value that already is in STEAM_1 form is passed through unchanged.
std::string ConvertSteamID(std::string_view input) {
	const std::string trimmed(TrimSpaces(input));
	if (trimmed.empty()) {
		return {};
	}
	if (trimmed.rfind("STEAM_", 0) == 0) {
		return trimmed;
	}
	std::string text = trimmed;
	text.erase(std::remove(text.begin(), text.end(), '['), text.end());
	text.erase(std::remove(text.begin(), text.end(), ']'), text.end());

	std::vector<std::string> parts;
	std::size_t start = 0;
	while (start <= text.size()) {
		const std::size_t colon = text.find(':', start);
		if (colon == std::string::npos) {
			parts.push_back(text.substr(start));
			break;
		}
		parts.push_back(text.substr(start, colon - start));
		start = colon + 1;
	}
	if (parts.size() < 3) {
		return text;
	}
	int account = 0;
	const std::string_view account_text = TrimSpaces(parts[2]);
	const auto result = std::from_chars(account_text.data(), account_text.data() + account_text.size(), account);
	if (result.ec != std::errc{}) {
		return text;
	}
	return std::string("STEAM_1:") + (account % 2 == 0 ? "0:" : "1:") + std::to_string(account / 2);
}

// One registration per consumer plugin and event; registering again replaces, exactly like the
// PluginId-keyed maps of the original.
template <typename Callback>
void AddListener(std::vector<Callback>& list, Callback callback) {
	if (callback == nullptr) {
		return;
	}
	if (std::find(list.begin(), list.end(), callback) == list.end()) {
		list.push_back(callback);
	}
}

template <typename Callback>
void RemoveListener(std::vector<Callback>& list, Callback callback) {
	list.erase(std::remove(list.begin(), list.end(), callback), list.end());
}

// The cookie value as text: the modules and the original stored plain strings.
std::string AnyToString(const plg::any& value) {
	std::string out;
	plg::visit(
		[&out](const auto& entry) {
			using T = std::decay_t<decltype(entry)>;
			if constexpr (std::is_same_v<T, plg::string>) {
				out.assign(entry.data(), entry.size());
			} else if constexpr (std::is_same_v<T, bool>) {
				out = entry ? "1" : "0";
			} else if constexpr (std::is_arithmetic_v<T>) {
				out = std::to_string(entry);
			}
		},
		value);
	return out;
}

// The strength bonuses of the streak kills (GiveExpForStreakKills of the original).
constexpr const char* kStreakPhrases[] = {"DoubleKill", "TripleKill", "Domination",   "Rampage",
										  "MegaKill",   "Ownage",     "UltraKill",    "KillingSpree",
										  "MonsterKill", "Unstoppable", "GodLike"};

const char* const kGameEvents[] = {
	"player_death",  "bomb_planted", "bomb_defused",  "bomb_dropped", "bomb_pickup",  "round_start",
	"round_end",     "round_mvp",    "hostage_killed", "hostage_rescued", "weapon_fire", "player_hurt"};

void GiveExpForStreak(LrPlugin& plugin, int slot);

// --- the s2sdk listeners -------------------------------------------------------------------------
// The original hooked IServerGameClients::ClientPutInServer / ClientDisconnect; s2sdk reports the
// same points through these two listeners. OnClientAuthenticated may not fire for real players
// (see perm_connector), so ReconcileSlots() is the path that actually loads everyone.

void OnClientAuthenticatedTrampoline(int32_t player_slot, uint64_t steamid) {
	try {
		g_lr_plugin.OnClientAuthenticated(player_slot, steamid);
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: exception in OnClientAuthenticated: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: unknown exception in OnClientAuthenticated");
	}
}

void OnClientDisconnectTrampoline(int32_t player_slot, s2sdk::NetworkDisconnectionReason reason) {
	(void)reason;
	try {
		g_lr_plugin.OnClientDisconnect(player_slot);
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: exception in OnClientDisconnect: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: unknown exception in OnClientDisconnect");
	}
}

s2sdk::ResultType EventTrampoline(const plg::string& name, void* event, bool /*dont_broadcast*/) {
	try {
		g_lr_plugin.OnGameEvent(name, event);
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: exception in a game event handler: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: unknown exception in a game event handler");
	}
	// A Pre hook must never swallow the event from the engine or from other plugins.
	return s2sdk::ResultType::Continue;
}

void MapStartLifecycle() {
	try {
		g_lr_plugin.OnMapStartHook();
	} catch (...) {
	}
}

void MapEndLifecycle() {
	try {
		g_lr_plugin.OnMapEndHook();
	} catch (...) {
	}
}

} // namespace

// --- phrases ------------------------------------------------------------------------------------

std::string LrPlugin::Phrase(std::string_view key) const {
	const auto it = m_phrases.find(key);
	if (it != m_phrases.end()) {
		return it->second;
	}
	// The original's map created an empty entry for an unknown key (printing just the prefix); the
	// key itself is friendlier and equally never crashing.
	return std::string(key);
}

void LrPlugin::PrintToConsole(std::string_view text) const {
	LogInfo(StripColorTags(text));
}

void LrPlugin::PrintToChatRaw(int slot, std::string_view text) const {
	if (slot < 0) {
		PrintToConsole(text);
		return;
	}
	if (!IsMapReady() || !IsSlotInRange(slot) || __s2sdk_PrintToChat == nullptr) {
		return;
	}
	try {
		s2sdk::PrintToChat(slot, plg::string(Colorize(text)));
	} catch (...) {
	}
}

void LrPlugin::PrintToChat(int slot, std::string_view text) const {
	const std::string line = Phrase("Prefix") + " " + std::string(text);
	if (slot < 0) {
		PrintToConsole(line);
		return;
	}
	if (!IsMapReady() || !IsSlotInRange(slot) || __s2sdk_PrintToChat == nullptr) {
		return;
	}
	try {
		s2sdk::PrintToChat(slot, plg::string(Colorize(line)));
	} catch (...) {
	}
}

void LrPlugin::PrintToChatAll(std::string_view text) const {
	const std::string line = Phrase("Prefix") + " " + std::string(text);
	if (!IsMapReady() || __s2sdk_PrintToChatAll == nullptr) {
		return;
	}
	try {
		s2sdk::PrintToChatAll(plg::string(Colorize(line)));
	} catch (...) {
	}
}

// --- configuration ------------------------------------------------------------------------------

bool LrPlugin::LoadSettingsFile() {
	KeyValues kv("LR_Settings");
	if (!kv.LoadFromFile(ResolveConfigPath("addons/configs/levels_ranks/settings.ini"))) {
		LogError("levels_ranks: cannot read addons/configs/levels_ranks/settings.ini - keeping the "
				 "built-in defaults");
		return false;
	}

	std::string problem;
	KeyValuesView view(kv);
	Settings parsed = LoadSettings(view, &problem);
	if (!problem.empty()) {
		// The original unloaded the plugin here; a Failed OnPluginStart would stop *every*
		// Plugify plugin, so the port fails soft and keeps the defaults of the missing block.
		LogError("levels_ranks: settings.ini: " + problem);
	}
	m_settings = std::move(parsed);
	m_table_name = m_settings.table_name;
	if (m_core) {
		m_core->SetSettings(m_settings);
		m_table_name = m_core->GetSettings().table_name;
	}
	return true;
}

bool LrPlugin::LoadPhrases() {
	PhraseMap loaded;
	if (!LoadPhrasesFile("addons/translations/lr_core.phrases.txt", loaded)) {
		LogError("levels_ranks: cannot read lr_core.phrases.txt - phrase keys are printed as-is");
	}
	PhraseMap ranks;
	if (!LoadPhrasesFile("addons/translations/lr_core_ranks.phrases.txt", ranks)) {
		LogError("levels_ranks: cannot read lr_core_ranks.phrases.txt - rank names are printed as-is");
	}
	m_phrases.clear();
	m_phrases.insert(loaded.begin(), loaded.end());
	m_phrases.insert(ranks.begin(), ranks.end());
	return !m_phrases.empty();
}

void LrPlugin::ReloadConfig() {
	LoadSettingsFile();
	LoadPhrases();
	if (m_core) {
		m_table_name = m_core->GetSettings().table_name;
	}
}

// --- database -----------------------------------------------------------------------------------

bool LrPlugin::EnsureCore() {
	plgdb::Config database;
	database.driver = plgdb::Driver::MySQL;
	database.port = 3306;
	database.database = "levels_ranks";
	bool have_config = false;

	KeyValues kv("Databases");
	if (kv.LoadFromFile(ResolveConfigPath("addons/configs/databases.cfg"))) {
		if (KeyValues* entry = kv.FindKey("levels_ranks", false)) {
			have_config = true;
			if (const std::string driver = entry->GetString("driver", "mysql"); driver == "sqlite") {
				database.driver = plgdb::Driver::SQLite;
			}
			database.host = entry->GetString("host", "127.0.0.1");
			database.port = static_cast<uint16_t>(std::max(1, std::min(65535, entry->GetInt("port", 3306))));
			database.user = entry->GetString("user", "");
			database.password = entry->GetString("pass", "");
			database.database = entry->GetString("database", "levels_ranks");
			if (database.driver == plgdb::Driver::SQLite) {
				database.sqlitePath = ResolveDataPath(entry->GetString("sqlite_path", "levels_ranks.sqlite3"));
			}
		}
	}
	if (!have_config) {
		// Fail soft: the original unloaded the plugin; a local SQLite file keeps the server
		// running and is the only thing a test setup needs.
		LogError("levels_ranks: no 'levels_ranks' entry in addons/configs/databases.cfg - falling "
				 "back to a local SQLite database");
		database.driver = plgdb::Driver::SQLite;
		database.sqlitePath = ResolveDataPath("levels_ranks.sqlite3");
	}

	m_database = database;
	m_db = plgdb::Database::Create(database);
	if (m_db == nullptr) {
		if (!m_core_config_error_logged) {
			LogError("levels_ranks: the database configuration is invalid - running without storage");
			m_core_config_error_logged = true;
		}
		return false;
	}

	m_core = std::make_unique<LrCore>(*m_db);
	m_core->SetSettings(m_settings);
	m_table_name = m_core->GetSettings().table_name;

	m_core->findPlayerName = [](int slot) { return g_lr_plugin.PlayerNameOf(slot); };
	m_core->onError = [](std::string_view message) { LogError(std::string(message)); };
	m_core->onLevelChangedPre = [](int slot, int& new_rank, int old_rank) {
		g_lr_plugin.BroadcastLevelChangedPre(slot, new_rank, old_rank);
	};
	m_core->onLevelChangedPost = [](int slot, int new_rank, int old_rank) {
		g_lr_plugin.BroadcastLevelChangedPost(slot, new_rank, old_rank);
	};
	m_core->onPlayerLoaded = [](int slot, const std::string& steamid) {
		g_lr_plugin.BroadcastPlayerLoaded(slot, steamid);
	};
	m_core->onResetPlayerStats = [](int slot, const std::string& steamid) {
		g_lr_plugin.BroadcastResetPlayerStats(slot, steamid);
	};
	m_core->onPlayerPosInTop = [](int slot, int exp_pos, int time_pos) {
		g_lr_plugin.BroadcastPlayerPosInTop(slot, exp_pos, time_pos);
	};
	m_core->onExpChangedPre = [](int slot, int& value) { g_lr_plugin.BroadcastExpChangedPre(slot, value); };
	m_core->onExpChangedPost = [](int slot, int value, int new_exp) {
		g_lr_plugin.BroadcastExpChangedPost(slot, value, new_exp);
	};
	m_core->onDatabaseCleanup = [](int type) { g_lr_plugin.BroadcastDatabaseCleanup(type); };
	m_core->onLevelMessage = [](int target, int speaker, int rank, bool up, bool personal) {
		const Settings& settings = g_lr_plugin.Cfg();
		const std::size_t index = rank > 0 ? static_cast<std::size_t>(rank - 1) : 0;
		const std::string& name = index < settings.rank_names.size()
									  ? g_lr_plugin.Phrase(settings.rank_names[index])
									  : std::string();
		if (personal) {
			g_lr_plugin.PrintToChat(target, FormatString(g_lr_plugin.Phrase(up ? "LevelUp" : "LevelDown").c_str(),
														name.c_str()));
			return;
		}
		const char* key = up ? "LevelUpAll" : "LevelDownAll";
		g_lr_plugin.PrintToChat(target, FormatString(g_lr_plugin.Phrase(key).c_str(),
													 g_lr_plugin.PlayerNameOf(speaker).c_str(), name.c_str()));
	};
	m_core->onExpMessage = [](int slot, const std::string& phrase, int new_exp, int requested) {
		const std::string value = (requested > 0 ? "+" : "") + std::to_string(requested);
		g_lr_plugin.PrintToChat(slot, FormatString(g_lr_plugin.Phrase(phrase).c_str(), new_exp, value.c_str()));
	};
	m_core->onCoreReady = {[] {
		if (LrCore* core = g_lr_plugin.Core(); core != nullptr) {
			core->CleanDatabase();
		}
		g_lr_plugin.BroadcastCoreReady();
	}};

	m_core->CreateSchemaAsync([this](bool ok) { OnCoreSchemaResult(ok); });
	return true;
}

void LrPlugin::OnCoreSchemaResult(bool ok) {
	if (m_core == nullptr) {
		return; // the core was torn down while the statements were queued
	}
	if (!ok) {
		// The callback runs from plgdb's Poll(); tearing the core down here would destroy the
		// object whose callback is still on the stack. OnPluginUpdate does it after the pump.
		m_core_schema_failed = true;
		return;
	}
	m_core->ConfigureDatabase();
}

// --- listeners ----------------------------------------------------------------------------------

void LrPlugin::RegisterListeners() {
	if (m_listeners_registered) {
		return;
	}
	m_listeners_registered = true;
	const bool auth_pair = __s2sdk_OnClientAuthenticated_Register != nullptr &&
						   __s2sdk_OnClientAuthenticated_Unregister != nullptr;
	const bool disconnect_pair = __s2sdk_OnClientDisconnect_Register != nullptr &&
								 __s2sdk_OnClientDisconnect_Unregister != nullptr;
	if (auth_pair) {
		try {
			s2sdk::OnClientAuthenticated_Register(&OnClientAuthenticatedTrampoline);
			m_auth_registered = true;
		} catch (...) {
			LogError("levels_ranks: could not register the s2sdk OnClientAuthenticated listener");
		}
	}
	if (disconnect_pair) {
		try {
			s2sdk::OnClientDisconnect_Register(&OnClientDisconnectTrampoline);
			m_disconnect_registered = true;
		} catch (...) {
			LogError("levels_ranks: could not register the s2sdk OnClientDisconnect listener");
		}
	}
	if (!auth_pair && !disconnect_pair) {
		LogError("levels_ranks: the s2sdk player-listener bindings are incomplete - the client "
				 "listeners are disabled");
	}
}

void LrPlugin::UnregisterListeners() {
	if (m_auth_registered && __s2sdk_OnClientAuthenticated_Unregister != nullptr) {
		try {
			s2sdk::OnClientAuthenticated_Unregister(&OnClientAuthenticatedTrampoline);
		} catch (...) {
		}
	}
	if (m_disconnect_registered && __s2sdk_OnClientDisconnect_Unregister != nullptr) {
		try {
			s2sdk::OnClientDisconnect_Unregister(&OnClientDisconnectTrampoline);
		} catch (...) {
		}
	}
	m_auth_registered = false;
	m_disconnect_registered = false;
	m_listeners_registered = false;
}

void LrPlugin::RegisterGameEvents() {
	if (__s2sdk_HookEvent == nullptr || __s2sdk_UnhookEvent == nullptr) {
		LogError("levels_ranks: the s2sdk event bindings are incomplete (HookEvent/UnhookEvent) - "
				 "the statistics events are disabled");
		return;
	}
	for (const char* name : kGameEvents) {
		try {
			if (s2sdk::HookEvent(plg::string(name), &EventTrampoline, s2sdk::HookMode::Pre) ==
				s2sdk::EventHookError::Okay) {
				m_hooked_events.emplace_back(name);
			} else {
				LogError(std::string("levels_ranks: could not hook the game event '") + name + "'");
			}
		} catch (...) {
			LogError(std::string("levels_ranks: could not hook the game event '") + name + "'");
		}
	}
}

void LrPlugin::UnregisterGameEvents() {
	if (__s2sdk_UnhookEvent == nullptr) {
		m_hooked_events.clear();
		return;
	}
	for (const plg::string& name : m_hooked_events) {
		try {
			s2sdk::UnhookEvent(name, &EventTrampoline, s2sdk::HookMode::Pre);
		} catch (...) {
		}
	}
	m_hooked_events.clear();
}

bool LrPlugin::IsWarmupPeriod() {
	// g_pGameRules->m_bWarmupPeriod() of the original. The field lives on the CCSGameRules OBJECT,
	// not on the CCSGameRulesProxy entity: GetGameRules() is the m_pGameRules object behind it.
	// The object pointer is not an entity, so the field is read at its schema offset.
	if (!IsMapReady() || __s2sdk_GetGameRules == nullptr) {
		return false;
	}
	const void* rules = nullptr;
	try {
		rules = s2sdk::GetGameRules();
	} catch (...) {
		return false;
	}
	if (rules == nullptr) {
		return false; // no map (the game rules exist only with one)
	}
	if (m_warmup_offset == -2) {
		m_warmup_offset = SchemaOffset("CCSGameRules", "m_bWarmupPeriod");
		if (m_warmup_offset < 0 && !m_warmup_field_missing_logged) {
			LogError("levels_ranks: the schema field CCSGameRules::m_bWarmupPeriod was not found - "
					 "lr_block_warmup cannot be applied");
			m_warmup_field_missing_logged = true;
		}
	}
	if (m_warmup_offset < 0) {
		return false;
	}
	return *reinterpret_cast<const uint8_t*>(static_cast<const char*>(rules) + m_warmup_offset) != 0;
}

// --- map lifecycle ------------------------------------------------------------------------------

void LrPlugin::OnMapStartHook() {
	m_since_tick = std::chrono::milliseconds(0);
	m_clear_allow_next_update = false;
	m_warmup_offset = -2;
	m_warmup_field_missing_logged = false;
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		m_admin_access.Revoke(slot);
		m_last_load_attempt[static_cast<std::size_t>(slot)] = 0;
		m_last_top_request[static_cast<std::size_t>(slot)] = {};
		m_last_rank_request[static_cast<std::size_t>(slot)] = 0;
		m_name_cache[static_cast<std::size_t>(slot)].clear();
	}
}

void LrPlugin::OnMapEndHook() {
	// PITFALLS B2: only state here. The menu layer ends the displays through the safe
	// CancelClientMenu(Disconnect) path itself (no client query from this plugin).
	CloseMenusOnMapEnd();
	m_since_tick = std::chrono::milliseconds(0);
	m_clear_allow_next_update = false;
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		m_admin_access.Revoke(slot);
		m_last_top_request[static_cast<std::size_t>(slot)] = {};
		m_last_rank_request[static_cast<std::size_t>(slot)] = 0;
	}
	if (m_core) {
		m_core->SetAllowStatistic(false);
	}
}

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult LrPlugin::OnPluginStart() {
	try {
		// A reload starts from a clean per-slot state: the previous instance may have been unloaded
		// mid-map, so its caches must not leak into this one.
		m_core_ready = false;
		m_core_schema_failed = false;
		m_core_config_error_logged = false;
		m_core_retry_timer = std::chrono::milliseconds(0);
		m_since_tick = std::chrono::milliseconds(0);
		m_clear_allow_next_update = false;
		for (int slot = 0; slot < kMaxSlots; ++slot) {
			m_admin_access.Revoke(slot);
			m_last_load_attempt[static_cast<std::size_t>(slot)] = 0;
			m_last_top_request[static_cast<std::size_t>(slot)] = {};
			m_last_rank_request[static_cast<std::size_t>(slot)] = 0;
			m_name_cache[static_cast<std::size_t>(slot)].clear();
		}
		SetMapLifecycleCallbacks(&MapStartLifecycle, &MapEndLifecycle);
		InitMapGate();
		StartMenus();
		LoadPhrases();
		LoadSettingsFile();

		// The database may be down at this moment (the daily reboot starts MySQL and CS2 together,
		// a reload can follow a DB restart). The commands, listeners and events do not touch the
		// engine here (PITFALLS B.1) and the core stays a no-op until it is ready, so they are
		// registered regardless; EnsureCore is retried from OnPluginUpdate until it succeeds.
		if (!EnsureCore()) {
			LogError("levels_ranks: the core is not ready - the plugin retries every 10 seconds");
		}

		RegisterCommands();
		RegisterListeners();
		RegisterGameEvents();
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: exception in OnPluginStart: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: unknown exception in OnPluginStart");
	}
	return plg::PluginResult{}; // never Failed: that stops every Plugify plugin
}

plg::PluginResult LrPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		// Open the gate for a plugin loaded into a running map; the menu queue is drained every
		// frame (also with the gate shut, to finish the safe teardown of the previous map).
		PollMapGate();
		ProcessMenuRequests();
		if (m_db != nullptr) {
			m_db->Poll();
		}
		if (m_core_schema_failed) {
			// The asynchronous CREATE TABLE failed: drop the half-built core outside the plgdb
			// callback that reported it and let the retry below start a fresh attempt.
			m_core_schema_failed = false;
			m_core.reset();
			m_db.reset();
			m_core_ready = false;
			m_core_retry_timer = std::chrono::milliseconds(0);
		}
		if (m_core == nullptr) {
			m_core_retry_timer += dt;
			if (m_core_retry_timer >= kCoreRetryInterval) {
				m_core_retry_timer = std::chrono::milliseconds(0);
				(void)EnsureCore();
			}
		}
		if (IsMapReady()) {
			if (m_clear_allow_next_update) {
				m_clear_allow_next_update = false;
				if (m_core) {
					m_core->SetAllowStatistic(false);
				}
			}
			m_since_tick += dt;
			if (m_since_tick >= std::chrono::seconds(1)) {
				m_since_tick = std::chrono::milliseconds(0);
				OnSecondTick();
			}
		} else {
			m_since_tick = std::chrono::milliseconds(0);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: exception in OnPluginUpdate: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: unknown exception in OnPluginUpdate");
	}
	return plg::PluginResult{};
}

plg::PluginResult LrPlugin::OnPluginEnd() {
	try {
		UnregisterGameEvents();
		UnregisterCommands();
		UnregisterListeners();
		// The menus go before the map gate: ShutdownMenus uses the live gate to close a displayed
		// backend safely on an unload-mid-map.
		ShutdownMenus();
		ShutdownMapGate();
		// The core goes before the database: its callbacks hold a lifetime token.
		m_core.reset();
		m_db.reset();
		m_core_ready = false;
		m_core_schema_failed = false;
		m_core_config_error_logged = false;
		m_core_retry_timer = std::chrono::milliseconds(0);
		m_listeners_registered = false;
		m_phrases.clear();
		m_local_cookies.clear();
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: exception in OnPluginEnd: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: unknown exception in OnPluginEnd");
	}
	return plg::PluginResult{};
}

// --- player helpers ---------------------------------------------------------------------------------

std::string LrPlugin::PlayerNameOf(int slot) const {
	if (!IsSlotInRange(slot)) {
		return {};
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	if (!IsMapReady()) {
		return m_name_cache[index];
	}
	std::string name = ClientName(slot);
	if (!name.empty()) {
		m_name_cache[index] = name;
		return name;
	}
	return m_name_cache[index];
}

std::string LrPlugin::SteamIdOf(int slot) const {
	if (!IsSlotInRange(slot)) {
		return {};
	}
	return ConvertSteamID(ClientAuthId(slot));
}

bool LrPlugin::CheckStatus(int slot) {
	// The CheckStatus() of the original, engine half first: a connected player with a pawn and a
	// steam id who is fully authenticated.
	if (slot < 0 || slot >= kMaxSlots || m_core == nullptr) {
		return false;
	}
	if (!IsMapReady()) {
		return false;
	}
	const bool engine_ok =
		IsClientConnected(slot) && ClientSteamId64(slot) > 0 && IsClientAuthenticated(slot);
	if (!engine_ok) {
		m_core->MarkUninitialized(slot); // the original cleared bInitialized as well
		return false;
	}
	if (!m_core->IsInitialized(slot)) {
		return false;
	}
	if (!HasPawn(slot)) {
		// A spectator/team-less player has no pawn. The original refused the credit then, but the
		// port's reconcile keeps the row so the counters since the last save are not lost.
		return false;
	}
	const std::string steamid = SteamIdOf(slot);
	return steamid.empty() || m_core->FindPlayer(slot)->steamid == steamid;
}

std::string LrPlugin::Cookie(int slot, std::string_view name) {
	const std::string key = std::string(kCookiePrefix) + std::string(name);
	const uint64_t steamid64 = ClientSteamId64(slot);
	if (steamid64 != 0 && __permissions_GetCookie != nullptr) {
		plg::any value;
		try {
			if (permissions::GetCookie(steamid64, plg::string(key), value) == permissions::Status::Success) {
				return AnyToString(value);
			}
		} catch (...) {
		}
	} else if (!m_cookie_warning_logged && __permissions_GetCookie == nullptr) {
		LogError("levels_ranks: the permissions plugin is missing - the cookies live in memory only "
				 "for this session");
		m_cookie_warning_logged = true;
	}
	const std::string steamid = SteamIdOf(slot);
	const auto player = m_local_cookies.find(steamid);
	if (player == m_local_cookies.end()) {
		return {};
	}
	const auto entry = player->second.find(key);
	return entry == player->second.end() ? std::string() : entry->second;
}

void LrPlugin::SetCookie(int slot, std::string_view name, std::string value) {
	const std::string key = std::string(kCookiePrefix) + std::string(name);
	const uint64_t steamid64 = ClientSteamId64(slot);
	if (steamid64 != 0 && __permissions_SetCookie != nullptr) {
		try {
			const permissions::Status status =
				permissions::SetCookie(static_cast<int64_t>(GetId()), steamid64, plg::string(key),
									   plg::any(plg::string(value)), /*dontBroadcast=*/false);
			if (status == permissions::Status::Success) {
				return;
			}
		} catch (...) {
		}
	}
	m_local_cookies[SteamIdOf(slot)][key] = std::move(value);
}

// --- client listeners -------------------------------------------------------------------------------

void LrPlugin::OnClientAuthenticated(int slot, uint64_t /*steamid*/) {
	if (!IsMapReady() || slot < 0 || slot >= kMaxSlots) {
		return;
	}
	LrCore* core = Core();
	if (core == nullptr || !IsCoreLoaded() || core->IsInitialized(slot)) {
		return;
	}
	if (!IsClientAuthenticated(slot) || ClientSteamId64(slot) == 0) {
		return; // the reconcile picks the slot up once the engine reports it as ready
	}
	const std::string steamid_text = SteamIdOf(slot);
	if (steamid_text.empty()) {
		return;
	}
	if (core->LoadInFlight(slot, steamid_text)) {
		return; // the row is on its way; a second ask would re-issue and reset the slot
	}
	NoteLoadAttempt(slot);
	core->LoadOrCreatePlayer(slot, steamid_text, PlayerNameOf(slot));
}

void LrPlugin::OnClientDisconnect(int slot) {
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	SetAdminAccess(slot, false);
	// PARITY: the original called SaveDataPlayer(slot, true), whose CheckStatus() also wanted a
	// live pawn - during a disconnect that is racy, so the port saves whenever the row is loaded.
	// The name comes from the cache when the map is already down (no client query is legal then).
	if (LrCore* core = Core(); core != nullptr && core->IsInitialized(slot)) {
		core->SavePlayer(slot, true);
	}
	m_name_cache[static_cast<std::size_t>(slot)].clear();
	m_last_load_attempt[static_cast<std::size_t>(slot)] = 0;
	m_last_top_request[static_cast<std::size_t>(slot)] = {};
	m_last_rank_request[static_cast<std::size_t>(slot)] = 0;
}

// --- listener fan-out ---------------------------------------------------------------------------------

void LrPlugin::BroadcastCoreReady() {
	m_core_ready = true;
	for (CoreReadyCallback callback : m_core_ready_listeners) {
		try {
			if (callback != nullptr) {
				callback();
			}
		} catch (const std::exception& ex) {
			LogError(std::string("levels_ranks: exception in an OnCoreIsReady listener: ") + ex.what());
		} catch (...) {
			LogError("levels_ranks: unknown exception in an OnCoreIsReady listener");
		}
	}
}

void LrPlugin::BroadcastLevelChangedPre(int slot, int& new_rank, int old_rank) {
	for (LevelChangedPreCallback callback : m_level_pre_listeners) {
		try {
			if (callback != nullptr) {
				callback(slot, new_rank, old_rank);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("levels_ranks: exception in an OnLevelChangedPre listener: ") + ex.what());
		} catch (...) {
			LogError("levels_ranks: unknown exception in an OnLevelChangedPre listener");
		}
	}
}

void LrPlugin::BroadcastLevelChangedPost(int slot, int new_rank, int old_rank) {
	for (LevelChangedPostCallback callback : m_level_post_listeners) {
		try {
			if (callback != nullptr) {
				callback(slot, new_rank, old_rank);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("levels_ranks: exception in an OnLevelChangedPost listener: ") + ex.what());
		} catch (...) {
			LogError("levels_ranks: unknown exception in an OnLevelChangedPost listener");
		}
	}
}

void LrPlugin::BroadcastPlayerKilledPre(int64_t event, int& exp_caused, int exp_victim, int exp_attacker) {
	for (PlayerKilledPreCallback callback : m_killed_pre_listeners) {
		try {
			if (callback != nullptr) {
				callback(event, exp_caused, exp_victim, exp_attacker);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("levels_ranks: exception in an OnPlayerKilledPre listener: ") + ex.what());
		} catch (...) {
			LogError("levels_ranks: unknown exception in an OnPlayerKilledPre listener");
		}
	}
}

void LrPlugin::BroadcastPlayerKilledPost(int64_t event, int exp_caused, int exp_victim, int exp_attacker) {
	for (PlayerKilledPostCallback callback : m_killed_post_listeners) {
		try {
			if (callback != nullptr) {
				callback(event, exp_caused, exp_victim, exp_attacker);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("levels_ranks: exception in an OnPlayerKilledPost listener: ") + ex.what());
		} catch (...) {
			LogError("levels_ranks: unknown exception in an OnPlayerKilledPost listener");
		}
	}
}

void LrPlugin::BroadcastPlayerLoaded(int slot, const std::string& steamid) {
	for (PlayerLoadedCallback callback : m_player_loaded_listeners) {
		try {
			if (callback != nullptr) {
				callback(slot, plg::string(steamid));
			}
		} catch (const std::exception& ex) {
			LogError(std::string("levels_ranks: exception in an OnPlayerLoaded listener: ") + ex.what());
		} catch (...) {
			LogError("levels_ranks: unknown exception in an OnPlayerLoaded listener");
		}
	}
}

void LrPlugin::BroadcastResetPlayerStats(int slot, const std::string& steamid) {
	for (ResetPlayerStatsCallback callback : m_reset_listeners) {
		try {
			if (callback != nullptr) {
				callback(slot, plg::string(steamid));
			}
		} catch (const std::exception& ex) {
			LogError(std::string("levels_ranks: exception in an OnResetPlayerStats listener: ") + ex.what());
		} catch (...) {
			LogError("levels_ranks: unknown exception in an OnResetPlayerStats listener");
		}
	}
}

void LrPlugin::BroadcastPlayerPosInTop(int slot, int exp_pos, int time_pos) {
	for (PlayerPosInTopCallback callback : m_pos_in_top_listeners) {
		try {
			if (callback != nullptr) {
				callback(slot, exp_pos, time_pos);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("levels_ranks: exception in an OnPlayerPosInTop listener: ") + ex.what());
		} catch (...) {
			LogError("levels_ranks: unknown exception in an OnPlayerPosInTop listener");
		}
	}
}

void LrPlugin::BroadcastExpChangedPre(int slot, int& give_exp) {
	for (ExpChangedPreCallback callback : m_exp_pre_listeners) {
		try {
			if (callback != nullptr) {
				callback(slot, give_exp);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("levels_ranks: exception in an OnExpChangedPre listener: ") + ex.what());
		} catch (...) {
			LogError("levels_ranks: unknown exception in an OnExpChangedPre listener");
		}
	}
}

void LrPlugin::BroadcastExpChangedPost(int slot, int give_exp, int new_exp) {
	for (ExpChangedPostCallback callback : m_exp_post_listeners) {
		try {
			if (callback != nullptr) {
				callback(slot, give_exp, new_exp);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("levels_ranks: exception in an OnExpChangedPost listener: ") + ex.what());
		} catch (...) {
			LogError("levels_ranks: unknown exception in an OnExpChangedPost listener");
		}
	}
}

void LrPlugin::BroadcastDatabaseCleanup(int type) {
	for (DatabaseCleanupCallback callback : m_cleanup_listeners) {
		try {
			if (callback != nullptr) {
				callback(type);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("levels_ranks: exception in an OnDatabaseCleanup listener: ") + ex.what());
		} catch (...) {
			LogError("levels_ranks: unknown exception in an OnDatabaseCleanup listener");
		}
	}
}

// --- statistics gate ------------------------------------------------------------------------------

void LrPlugin::CheckAllowStatistic(bool round_start) {
	// CheckAllowStatistic() of the original: at least lr_minplayers_count players with a team and
	// a pawn, and - with lr_block_warmup - not in the warmup period.
	if (!IsMapReady()) {
		return;
	}
	int players = 0;
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		if (!IsClientConnected(slot) || ClientSteamId64(slot) <= 0) {
			continue;
		}
		if (!HasPawn(slot) || !HasController(slot)) {
			continue;
		}
		if (ClientTeam(slot) > 1) {
			++players;
		}
	}

	const bool warning = players < m_settings.Value(LR_MinplayersCount);
	const bool warmup = m_settings.Value(LR_BlockWarmup) != 0 && IsWarmupPeriod();
	if (m_core) {
		m_core->SetAllowStatistic(!warning && !warmup);
	}

	if (m_settings.Value(LR_ShowSpawnMessage) == 0 || !round_start) {
		return;
	}
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		if (!IsClientConnected(slot) || ClientSteamId64(slot) <= 0) {
			continue;
		}
		if (warning) {
			PrintToChat(slot, FormatString(Phrase("RoundStartCheckCount").c_str(), players,
										   m_settings.Value(LR_MinplayersCount)));
		}
		PrintToChat(slot, Phrase("RoundStartMessageRanks"));
	}
}

void LrPlugin::OnSecondTick() {
	// The reliable load path: s2sdk's OnClientAuthenticated may not fire for real players (see
	// perm_connector), so connected players without a loaded row are picked up here. The same tick
	// keeps the statistics gate fresh when nothing else changes.
	ReconcileSlots();
	CheckAllowStatistic();
}

void LrPlugin::ReconcileSlots() {
	if (m_core == nullptr || !m_core_ready || !IsMapReady()) {
		return;
	}
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (!IsClientConnected(slot)) {
				m_admin_access.Revoke(slot);
				m_last_load_attempt[static_cast<std::size_t>(slot)] = 0;
				m_last_top_request[static_cast<std::size_t>(slot)] = {};
				m_last_rank_request[static_cast<std::size_t>(slot)] = 0;
				continue;
			}
			if (!IsClientAuthenticated(slot) || ClientSteamId64(slot) == 0) {
				continue;
			}
			const std::string steamid = SteamIdOf(slot);
			if (steamid.empty()) {
				continue;
			}
			if (const PlayerInfo* info = m_core->FindPlayer(slot);
				info != nullptr && info->initialized && info->steamid == steamid) {
				continue;
			}
			if (m_core->LoadInFlight(slot, steamid)) {
				continue;
			}
			const int64_t now = static_cast<int64_t>(std::time(nullptr));
			if (now - m_last_load_attempt[static_cast<std::size_t>(slot)] < kLoadRetrySeconds) {
				continue;
			}
			NoteLoadAttempt(slot);
			m_core->LoadOrCreatePlayer(slot, steamid, PlayerNameOf(slot));
		} catch (const std::exception& ex) {
			LogError("levels_ranks: reconcile of slot " + std::to_string(slot) + " failed: " + ex.what());
		} catch (...) {
			LogError("levels_ranks: reconcile of slot " + std::to_string(slot) + " failed with an unknown exception");
		}
	}
}

void LrPlugin::ResetPlayerStats(int slot) {
	if (m_core == nullptr || !CheckStatus(slot)) {
		return;
	}
	m_core->ResetPlayerStats(slot);
}

// --- the game events -------------------------------------------------------------------------------

namespace {

// GiveExpForStreakKills() of the original: the streak bonus of the kill streaks, then the kill
// counter is reset and - with lr_db_savedataplayer_mode - the row is saved.
void GiveExpForStreak(LrPlugin& plugin, int slot) {
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	LrCore* core = plugin.Core();
	if (core == nullptr) {
		return;
	}
	PlayerInfo* info = core->FindPlayer(slot);
	if (info == nullptr) {
		return;
	}
	if (info->kill_streak > 1) {
		const int index = LrCore::StreakBonusIndex(info->kill_streak);
		if (index >= 0 && index < kBonusCount) {
			core->GiveExp(slot, plugin.Cfg().bonus[static_cast<std::size_t>(index)],
						  kStreakPhrases[static_cast<std::size_t>(index)]);
		}
	}
	info = core->FindPlayer(slot);
	if (info != nullptr) {
		info->kill_streak = 0;
	}
	if (plugin.Cfg().Value(LR_DB_SaveDataPlayer_Mode) != 0) {
		core->SavePlayer(slot);
	}
}

bool EventBool(void* event, const char* key) {
	if (event == nullptr || key == nullptr || __s2sdk_GetEventBool == nullptr) {
		return false;
	}
	try {
		return s2sdk::GetEventBool(event, plg::string(key));
	} catch (...) {
		return false;
	}
}

int32_t EventInt(void* event, const char* key) {
	if (event == nullptr || key == nullptr || __s2sdk_GetEventInt == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetEventInt(event, plg::string(key));
	} catch (...) {
		return 0;
	}
}

} // namespace

void LrPlugin::OnGameEvent(const plg::string& name, void* event) {
	const std::string_view event_name(name.data(), name.size());
	if (event_name == "player_death") {
		OnPlayerDeath(event);
	} else if (event_name == "bomb_planted" || event_name == "bomb_defused" ||
			   event_name == "bomb_dropped" || event_name == "bomb_pickup") {
		OnBombEvent(std::string(event_name).c_str(), event);
	} else if (event_name == "round_start" || event_name == "round_end" || event_name == "round_mvp") {
		OnRoundEvent(std::string(event_name).c_str(), event);
	} else if (event_name == "hostage_killed" || event_name == "hostage_rescued") {
		OnHostageEvent(std::string(event_name).c_str(), event);
	} else if (event_name == "weapon_fire" || event_name == "player_hurt") {
		OnOtherEvent(std::string(event_name).c_str(), event);
	}
}

void LrPlugin::OnPlayerDeath(void* event) {
	if (m_core == nullptr || event == nullptr || !IsMapReady()) {
		return;
	}
	const bool map_ready = IsMapReady();
	// Player-typed fields: the 0..63 slot of the player, never the raw user id GetInt would answer
	// (see lr_event_fields.hpp).
	const int client = EventPlayerSlot(event, "userid", map_ready);
	const int attacker = EventPlayerSlot(event, "attacker", map_ready);
	const int assister = EventPlayerSlot(event, "assister", map_ready);

	// The original credited through NotifClient(), whose CheckStatus() required a pawn.
	if (assister >= 0 && assister < kMaxSlots && HasPawn(assister)) {
		if (m_core->GiveExp(assister, m_settings.Stats(LR_ExpGiveAssist), "AssisterKill").status) {
			if (PlayerInfo* info = m_core->FindPlayer(assister); info != nullptr) {
				info->stats[ST_ASSISTS]++;
				info->session[ST_ASSISTS]++;
			}
		}
	}

	if (attacker >= 0 && attacker < kMaxSlots && client >= 0 && client < kMaxSlots) {
		if (attacker == client) {
			if (!IsFakeClient(client)) {
				m_core->GiveExp(client, -m_settings.Stats(LR_ExpGiveSuicide), "Suicide");
			}
		} else {
			// The original required a controller and a pawn for both sides before it read the
			// teams (and returned from the whole handler when one was missing).
			if (!HasController(client) || !HasPawn(client) || !HasController(attacker) ||
				!HasPawn(attacker)) {
				return;
			}
			// PARITY: the original's team comparison counts two team-less pawns (both 0) as a
			// team kill as well.
			const bool same_team =
				!m_settings.Value(LR_AllAgainstAll) && ClientTeam(client) == ClientTeam(attacker);
			if (same_team) {
				m_core->GiveExp(attacker, -m_settings.Stats(LR_ExpGiveTeamKill), "TeamKill");
			} else {
				const PlayerInfo* victim_info = m_core->FindPlayer(client);
				const PlayerInfo* attacker_info = m_core->FindPlayer(attacker);
				if (victim_info == nullptr || attacker_info == nullptr) {
					return;
				}
				const int victim_exp = victim_info->stats[ST_EXP];
				const int attacker_exp = attacker_info->stats[ST_EXP];

				const bool fake_client = IsFakeClient(client) || !victim_info->initialized;
				const bool fake_attacker = IsFakeClient(attacker) || !attacker_info->initialized;

				int exp_attacker = 0;
				int exp_victim = 0;
				const int64_t event_id = static_cast<int64_t>(reinterpret_cast<intptr_t>(event));

				if (m_settings.type_statistics == 0) {
					exp_attacker = m_settings.Stats(LR_ExpKill + (fake_client ? 1 : 0));
					exp_victim = m_settings.Stats(LR_ExpDeath + (fake_attacker ? 1 : 0));
					BroadcastPlayerKilledPre(event_id, exp_attacker, victim_exp, attacker_exp);
				} else if (!fake_client && !fake_attacker) {
					if (m_settings.type_statistics == 1) {
						exp_attacker = LrCore::KillExpAttacker(1, attacker_exp, victim_exp);
						BroadcastPlayerKilledPre(event_id, exp_attacker, victim_exp, attacker_exp);
						if (exp_attacker < 1) {
							exp_attacker = 1;
						}
						exp_victim = LrCore::RatingVictimExp(exp_attacker, m_settings.Stats(LR_ExpKillCoefficient));
					} else {
						exp_attacker = LrCore::KillExpAttacker(2, attacker_exp, victim_exp);
						BroadcastPlayerKilledPre(event_id, exp_attacker, victim_exp, attacker_exp);
						exp_victim = exp_attacker = LrCore::SimpleModeExp(exp_attacker);
					}
				}

				const ExpResult attacker_result = m_core->GiveExp(attacker, exp_attacker, "Kill");
				const ExpResult victim_result = m_core->GiveExp(client, -exp_victim, "MyDeath");
				if (attacker_result.status || victim_result.status) {
					if (!fake_attacker) {
						if (EventBool(event, "headshot") &&
							m_core->GiveExp(attacker, m_settings.Stats(LR_ExpGiveHeadShot), "HeadShotKill").status) {
							if (PlayerInfo* info = m_core->FindPlayer(attacker); info != nullptr) {
								info->stats[ST_HEADSHOTS]++;
								info->session[ST_HEADSHOTS]++;
							}
						}
						if (PlayerInfo* info = m_core->FindPlayer(attacker); info != nullptr) {
							info->stats[ST_KILLS]++;
							info->session[ST_KILLS]++;
							info->kill_streak++;
						}
					}
					if (!fake_client) {
						if (PlayerInfo* info = m_core->FindPlayer(client); info != nullptr) {
							info->stats[ST_DEATHS]++;
							info->session[ST_DEATHS]++;
						}
					}
					const PlayerInfo* victim_after = m_core->FindPlayer(client);
					const PlayerInfo* attacker_after = m_core->FindPlayer(attacker);
					BroadcastPlayerKilledPost(event_id, exp_attacker,
											  victim_after != nullptr ? victim_after->stats[ST_EXP] : 0,
											  attacker_after != nullptr ? attacker_after->stats[ST_EXP] : 0);
				}
			}
		}
	}

	GiveExpForStreak(*this, client);
}

void LrPlugin::OnBombEvent(const char* name, void* event) {
	if (m_core == nullptr || event == nullptr || !IsMapReady()) {
		return;
	}
	// "userid" is player-typed: bomb_pickup's is a player_pawn field, the other three carry the
	// controller pair (lr_event_fields.hpp).
	const int slot = EventPlayerSlot(event, "userid", IsMapReady());
	if (slot < 0 || slot >= kMaxSlots || !HasPawn(slot)) {
		return;
	}
	const std::string_view event_name(name != nullptr ? name : "");

	if (event_name == "bomb_defused") {
		m_core->GiveExp(slot, m_settings.Stats(LR_ExpBombDefused), "BombDefused");
		return;
	}
	PlayerInfo* info = m_core->FindPlayer(slot);
	if (info == nullptr) {
		return;
	}
	if (event_name == "bomb_planted") {
		if (m_core->GiveExp(slot, m_settings.Stats(LR_ExpBombPlanted), "BombPlanted").status) {
			info->have_bomb = false;
		}
		return;
	}
	if (event_name == "bomb_dropped") {
		if (info->have_bomb &&
			m_core->GiveExp(slot, -m_settings.Stats(LR_ExpBombDropped), "BombDropped").status) {
			info->have_bomb = false;
		}
		return;
	}
	// bomb_pickup
	if (!info->have_bomb && m_core->GiveExp(slot, m_settings.Stats(LR_ExpBombPickup), "BombPickup").status) {
		info->have_bomb = true;
	}
}

void LrPlugin::OnRoundEvent(const char* name, void* event) {
	if (m_core == nullptr || event == nullptr || !IsMapReady()) {
		return;
	}
	const std::string_view event_name(name != nullptr ? name : "");

	if (event_name == "round_start") {
		CheckAllowStatistic(true);
		return;
	}

	if (event_name == "round_mvp") {
		const int slot = EventPlayerSlot(event, "userid", IsMapReady());
		if (slot >= 0 && slot < kMaxSlots && HasPawn(slot)) {
			m_core->GiveExp(slot, m_settings.Stats(LR_ExpRoundMVP), "RoundMVP");
		}
		return;
	}

	// round_end: "winner" is the numeric team/winner id, so GetEventInt stays.
	const int winner = EventInt(event, "winner");
	if (winner > 1) {
		for (int slot = 0; slot < kMaxSlots; ++slot) {
			if (!IsClientConnected(slot) || ClientSteamId64(slot) <= 0) {
				continue;
			}
			if (!HasPawn(slot)) {
				continue;
			}
			const int team = ClientTeam(slot);
			if (team > 1) {
				const bool lose = team != winner;
				const ExpResult result = lose ? m_core->GiveExp(slot, -m_settings.Stats(LR_ExpRoundLose), "RoundLose")
											  : m_core->GiveExp(slot, m_settings.Stats(LR_ExpRoundWin), "RoundWin");
				if (result.status) {
					if (PlayerInfo* info = m_core->FindPlayer(slot); info != nullptr) {
						info->stats[ST_ROUNDSWIN + (lose ? 1 : 0)]++;
						info->session[ST_ROUNDSWIN + (lose ? 1 : 0)]++;
					}
				}
			}
			if (ClientHealth(slot) > 0) {
				GiveExpForStreak(*this, slot);
			}
			if (m_settings.Value(LR_ShowUsualMessage) == 2) {
				const PlayerInfo* info = m_core->FindPlayer(slot);
				if (info != nullptr) {
					if (info->round_exp != 0) {
						PrintToChat(slot, FormatString(
											 Phrase(info->round_exp > 0 ? "RoundExpResultGive" : "RoundExpResultTake").c_str(),
											 info->round_exp));
					} else {
						PrintToChat(slot, Phrase("RoundExpResultNothing"));
					}
					PrintToChat(slot, FormatString(Phrase("RoundExpResultAll").c_str(), info->stats[ST_EXP]));
					if (PlayerInfo* mutable_info = m_core->FindPlayer(slot); mutable_info != nullptr) {
						mutable_info->round_exp = 0;
					}
				}
			}
		}
	}

	if (m_settings.Value(LR_GiveExpRoundEnd) == 0) {
		// The original cleared the flag on the next frame; one update later is the same frame
		// boundary.
		m_clear_allow_next_update = true;
	}
}

void LrPlugin::OnHostageEvent(const char* name, void* event) {
	if (m_core == nullptr || event == nullptr || !IsMapReady()) {
		return;
	}
	const int slot = EventPlayerSlot(event, "userid", IsMapReady());
	if (slot < 0 || slot >= kMaxSlots || !HasPawn(slot)) {
		return;
	}
	const std::string_view event_name(name != nullptr ? name : "");
	if (event_name == "hostage_killed") {
		m_core->GiveExp(slot, -m_settings.Stats(LR_ExpHostageKilled), "HostageKilled");
	} else {
		m_core->GiveExp(slot, m_settings.Stats(LR_ExpHostageRescued), "HostageRescued");
	}
}

void LrPlugin::OnOtherEvent(const char* name, void* event) {
	if (m_core == nullptr || event == nullptr || !m_core->CountPlayersAllowed() || !IsMapReady()) {
		return;
	}
	const std::string_view event_name(name != nullptr ? name : "");

	if (event_name == "weapon_fire") {
		const int slot = EventPlayerSlot(event, "userid", IsMapReady());
		if (slot >= 0 && slot < kMaxSlots && HasController(slot)) {
			if (PlayerInfo* info = m_core->FindPlayer(slot); info != nullptr && info->initialized) {
				info->stats[ST_SHOOTS]++;
				info->session[ST_SHOOTS]++;
			}
		}
		return;
	}

	// player_hurt
	const int slot = EventPlayerSlot(event, "userid", IsMapReady());
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	if (!HasController(slot)) {
		return;
	}
	const int attacker = EventPlayerSlot(event, "attacker", IsMapReady());
	if (attacker < 0 || attacker >= kMaxSlots || attacker == slot) {
		return;
	}
	if (PlayerInfo* info = m_core->FindPlayer(attacker); info != nullptr && info->initialized) {
		info->stats[ST_HITS]++;
		info->session[ST_HITS]++;
	}
}

} // namespace levels_ranks

// --- the exported methods (ILRApi of the original, 38 exports) -------------------------------------
// Every one of them catches its exceptions: they are called across the plugify boundary from other
// plugins, where a throw would unwind into the module (and from there into the engine).

using namespace levels_ranks;

namespace {

plg::vector<plg::string> ToPlgList(const std::vector<std::string>& values) {
	plg::vector<plg::string> result;
	result.reserve(values.size());
	for (const std::string& value : values) {
		result.emplace_back(value);
	}
	return result;
}

void AddCoreReady(CoreReadyCallback callback) {
	AddListener(g_lr_plugin.CoreReadyListeners(), callback);
}

void RemoveCoreReady(CoreReadyCallback callback) {
	RemoveListener(g_lr_plugin.CoreReadyListeners(), callback);
}

} // namespace

extern "C" PLUGIN_API bool IsCoreLoaded() {
	try {
		return g_lr_plugin.IsCoreLoaded();
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API int32_t GetSettingsValue(int32_t setting) {
	try {
		return g_lr_plugin.Cfg().Value(setting);
	} catch (...) {
		return 0;
	}
}

extern "C" PLUGIN_API int32_t GetSettingsStatsValue(int32_t setting) {
	try {
		return g_lr_plugin.Cfg().Stats(setting);
	} catch (...) {
		return 0;
	}
}

extern "C" PLUGIN_API int32_t GetCountPlayers() {
	try {
		return g_lr_plugin.Core() != nullptr ? g_lr_plugin.Core()->PlayerCount() : 0;
	} catch (...) {
		return 0;
	}
}

extern "C" PLUGIN_API plg::string GetTableName() {
	try {
		return plg::string(g_lr_plugin.TableName());
	} catch (...) {
		return plg::string("lvl_base");
	}
}

extern "C" PLUGIN_API plg::vector<plg::string> GetRankNames() {
	try {
		return ToPlgList(g_lr_plugin.Cfg().rank_names);
	} catch (...) {
		return {};
	}
}

extern "C" PLUGIN_API plg::vector<int32_t> GetRankExp() {
	try {
		const std::vector<int>& ranks = g_lr_plugin.Cfg().rank_exp;
		return plg::vector<int32_t>(ranks.begin(), ranks.end());
	} catch (...) {
		return {};
	}
}

extern "C" PLUGIN_API bool GetClientStatus(int32_t slot) {
	try {
		return g_lr_plugin.Core() != nullptr && g_lr_plugin.Core()->IsInitialized(slot);
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API bool CheckCountPlayers() {
	try {
		return g_lr_plugin.Core() != nullptr && g_lr_plugin.Core()->CountPlayersAllowed();
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API int32_t GetClientInfo(int32_t slot, int32_t statsType, bool session) {
	try {
		if (statsType < 0 || statsType >= kStatsCount) {
			return 0;
		}
		const PlayerInfo* info = g_lr_plugin.Core() != nullptr ? g_lr_plugin.Core()->FindPlayer(slot) : nullptr;
		if (info == nullptr) {
			return 0;
		}
		return session ? info->session[static_cast<std::size_t>(statsType)]
					   : info->stats[static_cast<std::size_t>(statsType)];
	} catch (...) {
		return 0;
	}
}

extern "C" PLUGIN_API void RoundWithoutValue() {
	try {
		if (g_lr_plugin.Core() != nullptr) {
			g_lr_plugin.Core()->SetAllowStatistic(false);
		}
	} catch (...) {
	}
}

extern "C" PLUGIN_API bool ChangeClientValue(int32_t slot, int32_t giveExp) {
	try {
		if (g_lr_plugin.Core() == nullptr || !g_lr_plugin.CheckStatus(slot)) {
			return false;
		}
		return g_lr_plugin.Core()->ChangeClientValue(slot, giveExp);
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API void ResetPlayerStats(int32_t slot) {
	try {
		g_lr_plugin.ResetPlayerStats(slot);
	} catch (...) {
	}
}

extern "C" PLUGIN_API plg::vector<plg::string> GetDatabaseConfig() {
	try {
		plg::vector<plg::string> result;
		result.reserve(6);
		const plgdb::Config& config = g_lr_plugin.DatabaseSettings();
		result.emplace_back(config.driver == plgdb::Driver::SQLite ? "sqlite" : "mysql");
		result.emplace_back(config.driver == plgdb::Driver::SQLite ? config.sqlitePath : config.host);
		result.emplace_back(std::to_string(config.port));
		result.emplace_back(config.driver == plgdb::Driver::SQLite ? "" : config.user);
		result.emplace_back(config.driver == plgdb::Driver::SQLite ? "" : config.password);
		result.emplace_back(config.database);
		return result;
	} catch (...) {
		return {};
	}
}

extern "C" PLUGIN_API void PrintToChat(int32_t slot, const plg::string& message) {
	try {
		g_lr_plugin.PrintToChat(slot, std::string_view(message));
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnCoreIsReady_Register(CoreReadyCallback callback) {
	try {
		AddCoreReady(callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnCoreIsReady_Unregister(CoreReadyCallback callback) {
	try {
		RemoveCoreReady(callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnLevelChangedPre_Register(LevelChangedPreCallback callback) {
	try {
		AddListener(g_lr_plugin.LevelChangedPreListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnLevelChangedPre_Unregister(LevelChangedPreCallback callback) {
	try {
		RemoveListener(g_lr_plugin.LevelChangedPreListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnLevelChangedPost_Register(LevelChangedPostCallback callback) {
	try {
		AddListener(g_lr_plugin.LevelChangedPostListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnLevelChangedPost_Unregister(LevelChangedPostCallback callback) {
	try {
		RemoveListener(g_lr_plugin.LevelChangedPostListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerKilledPre_Register(PlayerKilledPreCallback callback) {
	try {
		AddListener(g_lr_plugin.PlayerKilledPreListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerKilledPre_Unregister(PlayerKilledPreCallback callback) {
	try {
		RemoveListener(g_lr_plugin.PlayerKilledPreListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerKilledPost_Register(PlayerKilledPostCallback callback) {
	try {
		AddListener(g_lr_plugin.PlayerKilledPostListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerKilledPost_Unregister(PlayerKilledPostCallback callback) {
	try {
		RemoveListener(g_lr_plugin.PlayerKilledPostListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerLoaded_Register(PlayerLoadedCallback callback) {
	try {
		AddListener(g_lr_plugin.PlayerLoadedListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerLoaded_Unregister(PlayerLoadedCallback callback) {
	try {
		RemoveListener(g_lr_plugin.PlayerLoadedListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnResetPlayerStats_Register(ResetPlayerStatsCallback callback) {
	try {
		AddListener(g_lr_plugin.ResetPlayerStatsListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnResetPlayerStats_Unregister(ResetPlayerStatsCallback callback) {
	try {
		RemoveListener(g_lr_plugin.ResetPlayerStatsListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerPosInTop_Register(PlayerPosInTopCallback callback) {
	try {
		AddListener(g_lr_plugin.PlayerPosInTopListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerPosInTop_Unregister(PlayerPosInTopCallback callback) {
	try {
		RemoveListener(g_lr_plugin.PlayerPosInTopListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnExpChangedPre_Register(ExpChangedPreCallback callback) {
	try {
		AddListener(g_lr_plugin.ExpChangedPreListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnExpChangedPre_Unregister(ExpChangedPreCallback callback) {
	try {
		RemoveListener(g_lr_plugin.ExpChangedPreListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnExpChangedPost_Register(ExpChangedPostCallback callback) {
	try {
		AddListener(g_lr_plugin.ExpChangedPostListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnExpChangedPost_Unregister(ExpChangedPostCallback callback) {
	try {
		RemoveListener(g_lr_plugin.ExpChangedPostListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnDatabaseCleanup_Register(DatabaseCleanupCallback callback) {
	try {
		AddListener(g_lr_plugin.DatabaseCleanupListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnDatabaseCleanup_Unregister(DatabaseCleanupCallback callback) {
	try {
		RemoveListener(g_lr_plugin.DatabaseCleanupListeners(), callback);
	} catch (...) {
	}
}

PLUGIFY_PLUGIN(PLUGIN_API, &levels_ranks::g_lr_plugin)
