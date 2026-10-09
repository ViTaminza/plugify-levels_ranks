// levels_ranks — engine side of the plugin. This file ports levels_ranks.cpp of the Pisex
// cs2-lvl_ranks plugin onto lr_core (settings, ranks, statistics, SQL) and libs/pisexcompat
// (chat, menus, timers, events, players, KeyValues, phrases).
//
// The port keeps the structure of the original:
//   * LoadConfig()          -> LoadSettingsFile() (the pisexcompat KV parser fills the core's
//                              Settings through the KeyValuesView adapter below);
//   * AllPluginsLoaded()    -> EnsureCore() + RegisterListeners() + RegisterCommands();
//   * the IServerGameClients ClientPutInServer/ClientDisconnect hooks -> the s2sdk listeners plus
//                              a once-per-second reconcile (s2sdk's OnClientAuthenticated is not
//                              reliable, see perm_connector);
//   * the IUtilsApi event hooks -> pisex::g_pUtils->HookEvent() with the same handler bodies;
//   * the LRApi methods      -> the exported methods at the bottom of this file.
//
// Threading: everything runs on the game thread (the plugify pump, s2sdk callbacks, timers). The
// database is plgdb (one worker thread, results delivered from Poll()).

#include "lr_plugin.hpp"

#include "lr_keyvalues.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <utility>

#include <permissions/permissions.hpp>

namespace levels_ranks {

LrPlugin g_lr_plugin;

namespace {

// One non-empty plg::Log call; the host may run with logging disabled.
void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

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
// value that already is in STEAM_1 form (s2sdk's GetClientAuthId can hand that out) is passed
// through unchanged - the original's converter would have mangled it.
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

	// "U:1:accountid" (or any 3-part form): the original splits on ':' and rebuilds STEAM_1.
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

void GiveExpForStreak(LrPlugin& plugin, int slot);

// --- the s2sdk listeners -------------------------------------------------------------------------
// The original hooked IServerGameClients::ClientPutInServer / ClientDisconnect; s2sdk reports the
// same points through these two listeners. OnClientAuthenticated may not fire for real players
// (see perm_connector), so ReconcileSlots() is the path that actually loads everyone.

void OnClientAuthenticatedListener(int32_t player_slot, uint64_t steamid) {
	(void)steamid;
	try {
		if (player_slot < 0 || player_slot >= kMaxSlots) {
			return;
		}
		LrCore* core = g_lr_plugin.Core();
		if (core == nullptr || core->IsInitialized(player_slot)) {
			return;
		}
		if (!pisex::g_pPlayers->IsAuthenticated(player_slot) || pisex::g_pPlayers->GetSteamID64(player_slot) == 0) {
			return; // the reconcile picks the slot up once the engine reports it as ready
		}
		const std::string steamid_text = g_lr_plugin.SteamIdOf(player_slot);
		if (steamid_text.empty()) {
			return;
		}
		g_lr_plugin.NoteLoadAttempt(player_slot);
		core->LoadOrCreatePlayer(player_slot, steamid_text, g_lr_plugin.PlayerNameOf(player_slot));
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: exception in OnClientAuthenticated: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: unknown exception in OnClientAuthenticated");
	}
}

void OnClientDisconnectListener(int32_t player_slot, s2sdk::NetworkDisconnectionReason reason) {
	(void)reason;
	try {
		if (player_slot < 0 || player_slot >= kMaxSlots) {
			return;
		}
		g_lr_plugin.SetAdminAccess(player_slot, false);
		// PARITY: the original called SaveDataPlayer(slot, true), whose CheckStatus() also wanted
		// a live pawn - during a disconnect that is racy, so the port saves whenever the row is
		// loaded (the counters are what has to reach the database) and always clears the slot.
		if (LrCore* core = g_lr_plugin.Core(); core != nullptr && core->IsInitialized(player_slot)) {
			core->SavePlayer(player_slot, true);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: exception in OnClientDisconnect: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: unknown exception in OnClientDisconnect");
	}
}

} // namespace

// --- phrases ------------------------------------------------------------------------------------

const std::string& LrPlugin::Phrase(std::string_view key) const {
	const auto it = m_phrases.find(key);
	if (it != m_phrases.end()) {
		return it->second;
	}
	// The original's map created an empty entry for an unknown key (printing just the prefix); the
	// key itself is friendlier and equally never crashing.
	static thread_local std::string missing;
	missing.assign(key);
	return missing;
}

void LrPlugin::PrintToChat(int slot, std::string_view text) const {
	// ClientPrint() of the original: the 'Prefix' phrase in front of every message.
	if (slot == -1) {
		pisex::PrintToConsole(-1, "%s %s", Phrase("Prefix").c_str(), std::string(text).c_str());
		return;
	}
	pisex::PrintToChat(slot, "%s %s", Phrase("Prefix").c_str(), std::string(text).c_str());
}

void LrPlugin::PrintToChatAll(std::string_view text) const {
	pisex::PrintToChatAll("%s %s", Phrase("Prefix").c_str(), std::string(text).c_str());
}

// --- configuration ------------------------------------------------------------------------------

bool LrPlugin::LoadSettingsFile() {
	// LoadConfig() of the original, key for key; the defaults live in lr_core's LoadSettings().
	pisex::KeyValues kv("LR_Settings");
	if (!kv.LoadFromFile("addons/configs/levels_ranks/settings.ini")) {
		LogError("levels_ranks: cannot read addons/configs/levels_ranks/settings.ini - keeping the "
				 "built-in defaults");
		return false;
	}

	std::string problem;
	KeyValuesView view(kv);
	Settings parsed = LoadSettings(view, &problem);
	if (!problem.empty()) {
		// The original unloaded the plugin here; a Failed OnPluginStart would stop *every*
		// Plugify plugin, so the port fails soft (see the porting rules) and keeps the defaults
		// of the missing block.
		LogError("levels_ranks: settings.ini: " + problem);
	}
	m_settings = std::move(parsed);
	m_table_name = m_settings.table_name;
	if (m_core) {
		m_core->SetSettings(m_settings);
		m_table_name = m_settings.table_name; // SetSettings may have rejected the name
	}
	return true;
}

bool LrPlugin::LoadPhrases() {
	// The original merges lr_core.phrases.txt and lr_core_ranks.phrases.txt into one map
	// (g_vecPhrases); an unknown key answers with the key, like admin_system::GetTranslation.
	pisex::PhraseMap loaded;
	if (!pisex::LoadPhrasesFile("addons/translations/lr_core.phrases.txt", loaded)) {
		LogError("levels_ranks: cannot read lr_core.phrases.txt - phrase keys are printed as-is");
	}
	pisex::PhraseMap ranks;
	if (!pisex::LoadPhrasesFile("addons/translations/lr_core_ranks.phrases.txt", ranks)) {
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
		// The ranks and the settings are re-read; the live player state keeps its counters, like
		// the original's LoadConfig() which also only replaced the globals.
		m_table_name = m_settings.table_name;
	}
}

// --- database -----------------------------------------------------------------------------------

bool LrPlugin::EnsureCore() {
	// The databases.cfg entry of the original (section "levels_ranks"), with "driver" and
	// "sqlite_path" as optional keys for a SQLite server.
	plgdb::Config database;
	database.driver = plgdb::Driver::MySQL;
	pisex::KeyValues kv("Databases");
	bool have_config = false;
	if (kv.LoadFromFile("addons/configs/databases.cfg")) {
		if (pisex::KeyValues* entry = kv.FindKey("levels_ranks", false)) {
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
				database.sqlitePath = pisex::ResolveDataPath(entry->GetString("sqlite_path", "levels_ranks.sqlite3"));
			}
		}
	}
	if (!have_config) {
		// Fail soft: the original unloaded the plugin; a local SQLite file keeps the server
		// running and is the only thing a test setup needs.
		LogError("levels_ranks: no 'levels_ranks' entry in addons/configs/databases.cfg - falling "
				 "back to a local SQLite database");
		database.driver = plgdb::Driver::SQLite;
		database.sqlitePath = pisex::ResolveDataPath("levels_ranks.sqlite3");
	}

	m_database = database;
	m_db = plgdb::Database::Create(database);
	if (m_db == nullptr) {
		LogError("levels_ranks: the database configuration is invalid - running without storage");
		return false;
	}

	m_core = std::make_unique<LrCore>(*m_db);
	m_core->SetSettings(m_settings);
	m_table_name = m_settings.table_name;

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
			g_lr_plugin.PrintToChat(target, up ? pisex::FormatString(g_lr_plugin.Phrase("LevelUp").c_str(), name.c_str())
											   : pisex::FormatString(g_lr_plugin.Phrase("LevelDown").c_str(), name.c_str()));
			return;
		}
		const char* key = up ? "LevelUpAll" : "LevelDownAll";
		g_lr_plugin.PrintToChat(target, pisex::FormatString(g_lr_plugin.Phrase(key).c_str(),
														   g_lr_plugin.PlayerNameOf(speaker).c_str(), name.c_str()));
	};
	m_core->onExpMessage = [](int slot, const std::string& phrase, int new_exp, int requested) {
		const std::string value = (requested > 0 ? "+" : "") + std::to_string(requested);
		g_lr_plugin.PrintToChat(
			slot, pisex::FormatString(g_lr_plugin.Phrase(phrase).c_str(), new_exp, value.c_str()));
	};
	m_core->onCoreReady = {[] {
		g_lr_plugin.BroadcastCoreReady();
	}};

	try {
		m_core->CreateSchema();
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: ") + ex.what());
		return false;
	}

	m_core->ConfigureDatabase();
	return true;
}

// --- listeners ----------------------------------------------------------------------------------

void LrPlugin::RegisterListeners() {
	if (m_listeners_registered) {
		return;
	}
	// Each optional binding is checked on its own (a missing slot is a null function pointer).
	if (__s2sdk_OnClientAuthenticated_Register) {
		s2sdk::OnClientAuthenticated_Register(&OnClientAuthenticatedListener);
	}
	if (__s2sdk_OnClientDisconnect_Post_Register) {
		s2sdk::OnClientDisconnect_Post_Register(&OnClientDisconnectListener);
	}
	m_listeners_registered = true;
}

void LrPlugin::UnregisterListeners() {
	if (!m_listeners_registered) {
		return;
	}
	if (__s2sdk_OnClientAuthenticated_Unregister) {
		s2sdk::OnClientAuthenticated_Unregister(&OnClientAuthenticatedListener);
	}
	if (__s2sdk_OnClientDisconnect_Post_Unregister) {
		s2sdk::OnClientDisconnect_Post_Unregister(&OnClientDisconnectListener);
	}
	m_listeners_registered = false;
}

void LrPlugin::RegisterGameEvents() {
	// The twelve HookEvent calls of the original's AllPluginsLoaded, same names and same pre mode
	// (pisexcompat installs the s2sdk pre hook; the handlers below are dispatched by name).
	pisex::g_pUtils->HookEvent("player_death", [](const char* /*name*/, pisex::IGameEvent* event, bool /*dont*/) {
		try {
			g_lr_plugin.OnPlayerDeath(event != nullptr ? event->Native() : nullptr);
		} catch (...) {
			LogError("levels_ranks: unknown exception in the player_death hook");
		}
	});

	const char* const bomb_events[] = {"bomb_planted", "bomb_defused", "bomb_dropped", "bomb_pickup"};
	for (const char* name : bomb_events) {
		pisex::g_pUtils->HookEvent(name, [](const char* event_name, pisex::IGameEvent* event, bool) {
			try {
				g_lr_plugin.OnBombEvent(event_name, event != nullptr ? event->Native() : nullptr);
			} catch (...) {
				LogError("levels_ranks: unknown exception in a bomb event hook");
			}
		});
	}

	const char* const round_events[] = {"round_start", "round_end", "round_mvp"};
	for (const char* name : round_events) {
		pisex::g_pUtils->HookEvent(name, [](const char* event_name, pisex::IGameEvent* event, bool) {
			try {
				g_lr_plugin.OnRoundEvent(event_name, event != nullptr ? event->Native() : nullptr);
			} catch (...) {
				LogError("levels_ranks: unknown exception in a round event hook");
			}
		});
	}

	const char* const hostage_events[] = {"hostage_killed", "hostage_rescued"};
	for (const char* name : hostage_events) {
		pisex::g_pUtils->HookEvent(name, [](const char* event_name, pisex::IGameEvent* event, bool) {
			try {
				g_lr_plugin.OnHostageEvent(event_name, event != nullptr ? event->Native() : nullptr);
			} catch (...) {
				LogError("levels_ranks: unknown exception in a hostage event hook");
			}
		});
	}

	const char* const other_events[] = {"weapon_fire", "player_hurt"};
	for (const char* name : other_events) {
		pisex::g_pUtils->HookEvent(name, [](const char* event_name, pisex::IGameEvent* event, bool) {
			try {
				g_lr_plugin.OnOtherEvent(event_name, event != nullptr ? event->Native() : nullptr);
			} catch (...) {
				LogError("levels_ranks: unknown exception in a weapon/hurt event hook");
			}
		});
	}
}

bool LrPlugin::IsWarmupPeriod() {
	// g_pGameRules->m_bWarmupPeriod() of the original.
	//
	// The field lives on the CCSGameRules OBJECT, not on the CCSGameRulesProxy entity: s2sdk's
	// GetGameRulesProxy() is the proxy entity while GetGameRules() is the m_pGameRules object
	// behind it (s2sdk core/plugin.cpp: g_pGameRules = g_pGameRulesProxy->m_pGameRules). Reading
	// it through the entity-index helpers would apply a CCSGameRules offset to the proxy's
	// address - a wrong (and potentially out-of-bounds) read. The object pointer is not an
	// entity, so the field is read at its schema offset of that pointer, exactly like the
	// original's member access. The offset is resolved once; it is looked up only after
	// GetGameRules() answered (a map is up, so the schema system is too), and a miss is kept as
	// "missing" so a renamed field is logged once instead of re-queried (and re-warned) per call.
	if (__s2sdk_GetGameRules == nullptr) {
		return false;
	}
	const void* rules = s2sdk::GetGameRules();
	if (rules == nullptr) {
		return false; // no map (the game rules exist only with one)
	}
	if (m_warmup_offset == -2) {
		m_warmup_offset = pisex::SchemaOffset("CCSGameRules", "m_bWarmupPeriod");
		if (m_warmup_offset < 0 && !m_warmup_field_missing_logged) {
			// A game update renamed the field: lr_block_warmup then cannot be honoured. Log once
			// so the live server shows it instead of silently rating warmup rounds.
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

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult LrPlugin::OnPluginStart() {
	try {
		pisex::Init(std::string(kPluginName));
		LoadPhrases();
		LoadSettingsFile();

		if (!EnsureCore()) {
			LogError("levels_ranks: the core is not ready - the plugin runs as a no-op");
			return plg::PluginResult{};
		}

		RegisterListeners();
		RegisterCommands();
		RegisterGameEvents();

		// The one-second tick of the reconcile and of the statistics gate. Created from the
		// startup callback (a live map, like admin_system's timer); until then OnPluginUpdate
		// pumps it.
		pisex::RegisterStartupCallback([] {
			g_lr_plugin.StartSecondTimer();
			if (g_lr_plugin.Core() != nullptr) {
				g_lr_plugin.Core()->CleanDatabase();
			}
		});
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: exception in OnPluginStart: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: unknown exception in OnPluginStart");
	}
	return plg::PluginResult{};
}

plg::PluginResult LrPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		// pisexcompat first: deferred startup callbacks, timer reschedules and the menu requests
		// that came out of engine callbacks (which run under s2sdk's locks).
		pisex::Poll();
		if (m_db != nullptr) {
			m_db->Poll();
		}
		if (m_second_timer == nullptr) {
			m_since_tick += dt;
			if (m_since_tick >= std::chrono::seconds(1)) {
				m_since_tick = std::chrono::milliseconds(0);
				OnSecondTick();
			}
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
		UnregisterListeners();
		if (m_second_timer != nullptr) {
			pisex::g_pUtils->RemoveTimer(m_second_timer);
			m_second_timer = nullptr;
		}
		pisex::Shutdown();
		// The core goes before the database: its callbacks hold a lifetime token.
		m_core.reset();
		m_db.reset();
		m_core_ready = false;
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: exception in OnPluginEnd: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: unknown exception in OnPluginEnd");
	}
	return plg::PluginResult{};
}

// --- player helpers ---------------------------------------------------------------------------------

std::string LrPlugin::PlayerNameOf(int slot) const {
	const char* name = pisex::g_pPlayers->GetPlayerName(slot);
	return name != nullptr ? std::string(name) : std::string();
}

std::string LrPlugin::SteamIdOf(int slot) const {
	return ConvertSteamID(pisex::g_pPlayers->GetSteamID(slot));
}

bool LrPlugin::CheckStatus(int slot) {
	// The CheckStatus() of the original, engine half first: a connected player with a pawn and a
	// steam id who is fully authenticated.
	if (slot < 0 || slot >= kMaxSlots || m_core == nullptr) {
		return false;
	}
	const bool engine_ok = pisex::g_pPlayers->IsConnected(slot) && pisex::GetPawnEntity(slot) != 0 &&
						   pisex::g_pPlayers->GetSteamID64(slot) > 0 && pisex::g_pPlayers->IsAuthenticated(slot);
	if (!engine_ok) {
		m_core->MarkUninitialized(slot); // (g_iPlayerInfo[iSlot].bInitialized = false)
		return false;
	}
	return m_core->IsInitialized(slot);
}

std::string LrPlugin::Cookie(int slot, std::string_view name) {
	const std::string key = std::string(kCookiePrefix) + std::string(name);
	const uint64_t steamid64 = pisex::g_pPlayers->GetSteamID64(slot);
	if (steamid64 != 0 && __permissions_GetCookie != nullptr) {
		plg::any value;
		if (permissions::GetCookie(steamid64, plg::string(key), value) == permissions::Status::Success) {
			return AnyToString(value);
		}
		return {};
	}
	if (!m_cookie_warning_logged && __permissions_GetCookie == nullptr) {
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
	const uint64_t steamid64 = pisex::g_pPlayers->GetSteamID64(slot);
	if (steamid64 != 0 && __permissions_SetCookie != nullptr) {
		permissions::SetCookie(static_cast<int64_t>(GetId()), steamid64, plg::string(key),
							   plg::any(plg::string(value)), /*dontBroadcast=*/false);
		return;
	}
	m_local_cookies[SteamIdOf(slot)][key] = std::move(value);
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
	int players = 0;
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		if (!pisex::g_pPlayers->IsConnected(slot) || pisex::g_pPlayers->GetSteamID64(slot) <= 0) {
			continue;
		}
		if (pisex::GetPawnEntity(slot) == 0 || pisex::GetControllerEntity(slot) == 0) {
			continue;
		}
		if (pisex::GetPlayerTeam(slot) > 1) {
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
		if (!pisex::g_pPlayers->IsConnected(slot) || pisex::g_pPlayers->GetSteamID64(slot) <= 0) {
			continue;
		}
		if (warning) {
			PrintToChat(slot, pisex::FormatString(Phrase("RoundStartCheckCount").c_str(), players,
												 m_settings.Value(LR_MinplayersCount)));
		}
		PrintToChat(slot, Phrase("RoundStartMessageRanks"));
	}
}

void LrPlugin::StartSecondTimer() {
	if (m_second_timer != nullptr) {
		return;
	}
	try {
		m_second_timer = pisex::g_pUtils->CreateTimer(1.0f, [] {
			try {
				g_lr_plugin.OnSecondTick();
			} catch (...) {
				LogError("levels_ranks: unknown exception in the one-second tick");
			}
			return 1.0f;
		});
	} catch (const std::exception& ex) {
		LogError(std::string("levels_ranks: cannot start the one-second timer: ") + ex.what());
	} catch (...) {
		LogError("levels_ranks: cannot start the one-second timer");
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
	if (m_core == nullptr) {
		return;
	}
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (!pisex::g_pPlayers->IsConnected(slot)) {
				if (!m_steamids[slot].empty()) {
					m_steamids[slot].clear();
					m_admin_access[slot] = false;
				}
				m_last_load_attempt[slot] = 0;
				continue;
			}
			if (!pisex::g_pPlayers->IsAuthenticated(slot) || pisex::g_pPlayers->GetSteamID64(slot) == 0) {
				continue;
			}
			if (m_core->IsInitialized(slot)) {
				continue;
			}
			const int64_t now = static_cast<int64_t>(std::time(nullptr));
			if (now - m_last_load_attempt[slot] < kLoadRetrySeconds) {
				continue;
			}
			const std::string steamid = SteamIdOf(slot);
			if (steamid.empty()) {
				continue;
			}
			NoteLoadAttempt(slot);
			m_steamids[slot] = steamid;
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

} // namespace

void LrPlugin::OnPlayerDeath(void* event) {
	if (m_core == nullptr || event == nullptr) {
		return;
	}
	pisex::IGameEvent handle(event);
	const int client = handle.GetInt("userid");
	const int attacker = handle.GetInt("attacker");
	const int assister = handle.GetInt("assister");

	if (assister >= 0 && assister < kMaxSlots) {
		if (m_core->GiveExp(assister, m_settings.Stats(LR_ExpGiveAssist), "AssisterKill").status) {
			if (PlayerInfo* info = m_core->FindPlayer(assister); info != nullptr) {
				info->stats[ST_ASSISTS]++;
				info->session[ST_ASSISTS]++;
			}
		}
	}

	if (attacker >= 0 && attacker < kMaxSlots && client >= 0 && client < kMaxSlots) {
		if (attacker == client) {
			if (!pisex::g_pPlayers->IsFakeClient(client)) {
				m_core->GiveExp(client, -m_settings.Stats(LR_ExpGiveSuicide), "Suicide");
			}
		} else {
			// The original required a controller and a pawn for both sides before it read the
			// teams (and returned from the whole handler when one was missing).
			if (pisex::GetControllerEntity(client) == 0 ||
				pisex::GetPawnEntity(client) == 0 ||
				pisex::GetControllerEntity(attacker) == 0 ||
				pisex::GetPawnEntity(attacker) == 0) {
				return;
			}
			// PARITY: `pVictimPawn->GetTeam() == pAttackerPawn->GetTeam()` counts two
			// team-less pawns (both 0) as a team kill as well.
			const bool same_team = !m_settings.Value(LR_AllAgainstAll) &&
								   pisex::GetPlayerTeam(client) == pisex::GetPlayerTeam(attacker);
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

				const bool fake_client = pisex::g_pPlayers->IsFakeClient(client) || !victim_info->initialized;
				const bool fake_attacker = pisex::g_pPlayers->IsFakeClient(attacker) || !attacker_info->initialized;

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
				// PARITY: the original summed the two booleans (`+`) to enter this block.
				if (attacker_result.status || victim_result.status) {
					if (!fake_attacker) {
						if (handle.GetBool("headshot") &&
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
	if (m_core == nullptr || event == nullptr) {
		return;
	}
	pisex::IGameEvent handle(event);
	const int slot = handle.GetInt("userid");
	if (slot < 0 || slot >= kMaxSlots) {
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
		// PARITY: the original tests the *return* of NotifClient (CheckStatus + the statistic
		// gate), not whether an amount was applied.
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
	if (m_core == nullptr || event == nullptr) {
		return;
	}
	pisex::IGameEvent handle(event);
	const std::string_view event_name(name != nullptr ? name : "");

	if (event_name == "round_start") {
		CheckAllowStatistic(true);
		return;
	}

	if (event_name == "round_mvp") {
		const int slot = handle.GetInt("userid");
		if (slot >= 0 && slot < kMaxSlots) {
			m_core->GiveExp(slot, m_settings.Stats(LR_ExpRoundMVP), "RoundMVP");
		}
		return;
	}

	// round_end
	const int winner = handle.GetInt("winner");
	if (winner > 1) {
		for (int slot = 0; slot < kMaxSlots; ++slot) {
			if (!pisex::g_pPlayers->IsConnected(slot) || pisex::g_pPlayers->GetSteamID64(slot) <= 0) {
				continue;
			}
			if (pisex::GetPawnEntity(slot) == 0) {
				continue;
			}
			const int team = pisex::GetPlayerTeam(slot);
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
			if (pisex::GetPlayerHealth(slot) > 0) {
				GiveExpForStreak(*this, slot);
			}
			if (m_settings.Value(LR_ShowUsualMessage) == 2) {
				const PlayerInfo* info = m_core->FindPlayer(slot);
				if (info != nullptr) {
					if (info->round_exp != 0) {
						PrintToChat(slot, pisex::FormatString(
											 Phrase(info->round_exp > 0 ? "RoundExpResultGive" : "RoundExpResultTake").c_str(),
											 info->round_exp));
					} else {
						PrintToChat(slot, Phrase("RoundExpResultNothing"));
					}
					PrintToChat(slot, pisex::FormatString(Phrase("RoundExpResultAll").c_str(), info->stats[ST_EXP]));
					if (PlayerInfo* mutable_info = m_core->FindPlayer(slot); mutable_info != nullptr) {
						mutable_info->round_exp = 0;
					}
				}
			}
		}
	}

	if (m_settings.Value(LR_GiveExpRoundEnd) == 0) {
		// The original cleared the flag on the next frame; one update later is the same frame
		// boundary (pisexcompat drains NextFrame from Poll()).
		pisex::g_pUtils->NextFrame([] {
			if (g_lr_plugin.Core() != nullptr) {
				g_lr_plugin.Core()->SetAllowStatistic(false);
			}
		});
	}
}

void LrPlugin::OnHostageEvent(const char* name, void* event) {
	if (m_core == nullptr || event == nullptr) {
		return;
	}
	pisex::IGameEvent handle(event);
	const int slot = handle.GetInt("userid");
	if (slot < 0 || slot >= kMaxSlots) {
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
	if (m_core == nullptr || event == nullptr || !m_core->CountPlayersAllowed()) {
		return;
	}
	pisex::IGameEvent handle(event);
	const std::string_view event_name(name != nullptr ? name : "");

	if (event_name == "weapon_fire") {
		const int slot = handle.GetInt("userid");
		if (slot >= 0 && slot < kMaxSlots && pisex::GetControllerEntity(slot) != 0) {
			// PARITY: the original required the controller of "userid" to exist and incremented
			// the counters only for a loaded slot.
			if (PlayerInfo* info = m_core->FindPlayer(slot); info != nullptr && info->initialized) {
				info->stats[ST_SHOOTS]++;
				info->session[ST_SHOOTS]++;
			}
		}
		return;
	}

	// player_hurt
	const int slot = handle.GetInt("userid");
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	if (pisex::GetControllerEntity(slot) == 0) {
		return;
	}
	const int attacker = handle.GetInt("attacker");
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
		// SQLite hands out the file path in the host slot; there is no user/password then.
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
