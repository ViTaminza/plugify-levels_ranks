#pragma once
// Engine side of the levels_ranks plugin: the port of the original's levels_ranks.cpp
// (event hooks, commands, menus, cookies, the exported ILRApi) on top of lr_core and the native
// s2sdk API.
//
// Everything here runs on the game thread: the plugify update pump (OnPluginUpdate), the s2sdk
// listeners, the console/chat callbacks and the menu queue. No locking is needed and the plugin
// never blocks on the database: the schema is created asynchronously and the whole core bring-up
// is retried until the database answers.
//
// Layout of the glue:
//   plugin.cpp    lifecycle, settings/phrases/database loading, s2sdk listeners, the game event
//                 handlers, the cookie store, the printed messages and the exported methods
//   commands.cpp  mm_lvl_give_admin, mm_lvl_reset and the console/chat commands of the original
//   menus.cpp     the LR menu tree (main menu, my stats, session stats, the tops, the rank list,
//                 the admin give/take flow) built on lr_menus
//
// The exported methods use the callback typedefs of the generated headers
// (<levels_ranks/levels_ranks.hpp>, produced by bin/plugify-gen) - the same types the consumer
// headers hand to modules, so the boundary is checked by the compiler. The generated *binding
// slots* (__levels_ranks_*) stay null in this plugin: it implements the API, it never calls it.

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <plgdb/db.hpp>

#include <levels_ranks/levels_ranks.hpp>

#include "lr_core.hpp"
#include "lr_gate.hpp"

namespace levels_ranks {

// The name of the plugin in the plugify layout: configs, phrases and the cookie prefix use it.
inline constexpr std::string_view kPluginName = "levels_ranks";
inline constexpr std::string_view kCookiePrefix = "LR."; // ICookiesApi spelling of the original
inline constexpr auto kReconcileInterval = std::chrono::seconds(1);
// How often a slot whose row is not loaded yet is asked again (the original loaded once per
// connect; the reconcile may need a second try, but a broken database must not be hammered). Only
// applies to a load that actually failed or has been silent for the core's kLoadTimeoutSeconds: a
// load that is still in flight is left alone (LrCore::LoadInFlight).
inline constexpr int64_t kLoadRetrySeconds = 5;
// How often a failed core bring-up (database down at plugin start) is retried. The schema creation
// is asynchronous, so a retry never blocks the game thread.
inline constexpr auto kCoreRetryInterval = std::chrono::seconds(10);
// Per-slot cooldown of the top lists and of the rank report: one full scan of the statistics table
// per player per interval, however often the chat command is bound to a key.
inline constexpr int64_t kCommandCooldownSeconds = 5;

class LrPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// --- the core and its configuration -----------------------------------------------------
	LrCore* Core() { return m_core.get(); }
	const Settings& Cfg() const { return m_settings; }
	bool IsCoreLoaded() const { return m_core_ready; }
	// The table name cache: the exported GetTableName() hands out a pointer that has to stay
	// valid, exactly like the original's g_sTableName.
	const char* TableName() const { return m_table_name.c_str(); }
	// A phrase of lr_core.phrases.txt + lr_core_ranks.phrases.txt (the original's g_vecPhrases);
	// an unknown key is answered with the key itself.
	std::string Phrase(std::string_view key) const;
	// "Prefix <text>" for the chat, the ClientPrint() of the original.
	void PrintToChat(int slot, std::string_view text) const;
	// The same without the 'Prefix' phrase (the original's raw pisex::PrintToChat call sites).
	void PrintToChatRaw(int slot, std::string_view text) const;
	void PrintToChatAll(std::string_view text) const;
	// A server-console line: colour tags stripped, through plg::Log (the original's META_CONPRINT).
	void PrintToConsole(std::string_view text) const;

	// --- players ----------------------------------------------------------------------------
	// CheckStatus() of the original: a real, authenticated player whose row is loaded. A slot that
	// fails the engine checks loses bInitialized (the side effect of the original).
	bool CheckStatus(int slot);
	// engine->GetPlayerNetworkIDString() + ConvertSteamID() of the original.
	std::string SteamIdOf(int slot) const;
	std::string PlayerNameOf(int slot) const;
	// The player's own cookie ("LR.<name>"): the official permissions cookies when the plugin is
	// installed, an in-memory session store otherwise (the original fell back to
	// addons/data/lr_data.ini).
	std::string Cookie(int slot, std::string_view name);
	void SetCookie(int slot, std::string_view name, std::string value);

	// g_bAdminAccess[slot] of the original, but bound to the player instead of the slot: the grant
	// is valid only while the same SteamID64 and controller handle/generation still occupy the slot.
	// A re-used slot or a replaced controller loses the admin menu automatically.
	bool HasAdminAccess(int slot) const { return m_admin_access.Has(slot); }
	// Returns true only when the grant was actually bound to the slot's verified owner.
	bool SetAdminAccess(int slot, bool access) {
		if (access) {
			return m_admin_access.Grant(slot);
		}
		m_admin_access.Revoke(slot);
		return false;
	}

	// --- the module listeners (the ILRApi hook maps of the original) ---------------------------
	std::vector<CoreReadyCallback>& CoreReadyListeners() { return m_core_ready_listeners; }
	std::vector<LevelChangedPreCallback>& LevelChangedPreListeners() { return m_level_pre_listeners; }
	std::vector<LevelChangedPostCallback>& LevelChangedPostListeners() { return m_level_post_listeners; }
	std::vector<PlayerKilledPreCallback>& PlayerKilledPreListeners() { return m_killed_pre_listeners; }
	std::vector<PlayerKilledPostCallback>& PlayerKilledPostListeners() { return m_killed_post_listeners; }
	std::vector<PlayerLoadedCallback>& PlayerLoadedListeners() { return m_player_loaded_listeners; }
	std::vector<ResetPlayerStatsCallback>& ResetPlayerStatsListeners() { return m_reset_listeners; }
	std::vector<PlayerPosInTopCallback>& PlayerPosInTopListeners() { return m_pos_in_top_listeners; }
	std::vector<ExpChangedPreCallback>& ExpChangedPreListeners() { return m_exp_pre_listeners; }
	std::vector<ExpChangedPostCallback>& ExpChangedPostListeners() { return m_exp_post_listeners; }
	std::vector<DatabaseCleanupCallback>& DatabaseCleanupListeners() { return m_cleanup_listeners; }

	void BroadcastCoreReady();
	void BroadcastLevelChangedPre(int slot, int& new_rank, int old_rank);
	void BroadcastLevelChangedPost(int slot, int new_rank, int old_rank);
	void BroadcastPlayerKilledPre(int64_t event, int& exp_caused, int exp_victim, int exp_attacker);
	void BroadcastPlayerKilledPost(int64_t event, int exp_caused, int exp_victim, int exp_attacker);
	void BroadcastPlayerLoaded(int slot, const std::string& steamid);
	void BroadcastResetPlayerStats(int slot, const std::string& steamid);
	void BroadcastPlayerPosInTop(int slot, int exp_pos, int time_pos);
	void BroadcastExpChangedPre(int slot, int& give_exp);
	void BroadcastExpChangedPost(int slot, int give_exp, int new_exp);
	void BroadcastDatabaseCleanup(int type);

	// --- the ported logic ------------------------------------------------------------------------
	// LRApi::ResetPlayerStats() and LRApi::ChangeClientValue() (they check the status first).
	void ResetPlayerStats(int slot);
	// A full round of the statistics gate: the original's CheckAllowStatistic() plus the
	// "round start" messages when |round_start| is set.
	void CheckAllowStatistic(bool round_start = false);
	// The per-second work: the reconcile of the connected players (the reliable path, because
	// s2sdk's OnClientAuthenticated may not fire) and the statistics gate.
	void OnSecondTick();
	void ReconcileSlots();
	// Marks a started row load (the reconcile rate limits its retries with it).
	void NoteLoadAttempt(int slot) {
		if (slot >= 0 && slot < kMaxSlots) {
			m_last_load_attempt[static_cast<std::size_t>(slot)] = static_cast<int64_t>(std::time(nullptr));
		}
	}

	// --- the s2sdk client listeners ---------------------------------------------------------------
	void OnClientAuthenticated(int slot, uint64_t steamid);
	void OnClientDisconnect(int slot);

	// --- the game event handlers (s2sdk pre hooks) ------------------------------------------------
	void OnGameEvent(const plg::string& name, void* event);
	void OnPlayerDeath(void* event);
	void OnBombEvent(const char* name, void* event);
	void OnRoundEvent(const char* name, void* event);
	void OnHostageEvent(const char* name, void* event);
	void OnOtherEvent(const char* name, void* event);

	// --- map lifecycle (called by lr_gate after the gate state changed) ---------------------------
	void OnMapStartHook();
	void OnMapEndHook();

	// --- the menus (menus.cpp) ---------------------------------------------------------------------
	void ShowMainMenu(int slot);
	void ShowMyStats(int slot);
	void ShowSessionStats(int slot);
	void ShowStatsReset(int slot);
	void ShowTopsMenu(int slot);
	void ShowTopPlayers(int slot, bool by_playtime);
	void ShowRanksMenu(int slot);
	void ShowAdminMenu(int slot);
	void ShowGiveTakeMenu(int slot);
	void ShowGiveTakeValues(int slot, int target);

	// The connection settings the plugin opened (the GetDatabaseConfig export).
	const plgdb::Config& DatabaseSettings() const { return m_database; }

	// --- commands (commands.cpp) ---------------------------------------------------------------------
	void RegisterCommands();
	void UnregisterCommands();
	// mm_lvl_reset: the cleanup statement plus the reload of every loaded player.
	void RunDatabaseCleanup(int type);
	// The mm_rank / !rank report (RankPlayer, once per player with lr_show_rankmessage).
	void ShowRankReport(int slot);
	void ReloadConfig();

private:
	bool LoadPhrases();
	bool LoadSettingsFile();
	// Creates the database/core and starts the asynchronous schema creation. True when an attempt
	// was started; the core is usable only after OnCoreSchemaResult(true) -> ConfigureDatabase ->
	// the core-ready broadcast.
	bool EnsureCore();
	void OnCoreSchemaResult(bool ok);

	void RegisterListeners();
	void UnregisterListeners();
	void RegisterGameEvents();
	void UnregisterGameEvents();

	// m_bWarmupPeriod of the game rules (the schema field the original read through CCSGameRules).
	bool IsWarmupPeriod();

	Settings m_settings;
	std::string m_table_name = "lvl_base";
	std::map<std::string, std::string, std::less<>> m_phrases;

	std::unique_ptr<plgdb::Database> m_db;
	std::unique_ptr<LrCore> m_core;
	plgdb::Config m_database;
	bool m_core_ready = false;
	bool m_listeners_registered = false;
	bool m_auth_registered = false;
	bool m_disconnect_registered = false;
	// A failed asynchronous CREATE TABLE is torn down from OnPluginUpdate (not from the plgdb
	// callback that reported it), and EnsureCore is tried again every kCoreRetryInterval.
	bool m_core_schema_failed = false;
	bool m_core_config_error_logged = false;
	std::chrono::milliseconds m_core_retry_timer{0};

	// The player cookies of the fallback store (no permissions plugin): steamid -> name -> value.
	std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> m_local_cookies;
	bool m_cookie_warning_logged = false;

	std::vector<CoreReadyCallback> m_core_ready_listeners;
	std::vector<LevelChangedPreCallback> m_level_pre_listeners;
	std::vector<LevelChangedPostCallback> m_level_post_listeners;
	std::vector<PlayerKilledPreCallback> m_killed_pre_listeners;
	std::vector<PlayerKilledPostCallback> m_killed_post_listeners;
	std::vector<PlayerLoadedCallback> m_player_loaded_listeners;
	std::vector<ResetPlayerStatsCallback> m_reset_listeners;
	std::vector<PlayerPosInTopCallback> m_pos_in_top_listeners;
	std::vector<ExpChangedPreCallback> m_exp_pre_listeners;
	std::vector<ExpChangedPostCallback> m_exp_post_listeners;
	std::vector<DatabaseCleanupCallback> m_cleanup_listeners;

	// The names of the events actually hooked (UnhookEvent runs only for these).
	std::vector<plg::string> m_hooked_events;

	// The per-slot bookkeeping of the glue (the original kept it in globals). The admin grant is
	// owner-bound (SteamID64 + controller handle); the steam id and the counters of a slot live in
	// the core. The load attempts are rate limited so a broken database is not queried every
	// second. The name cache keeps the last real name for a save that arrives after OnMapEnd (no
	// client query is allowed there).
	AdminAccess m_admin_access;
	std::array<int64_t, kMaxSlots> m_last_load_attempt{};
	mutable std::array<std::string, kMaxSlots> m_name_cache;
	// The last time the slot asked for a top list (per list: experience / play time) and for the
	// rank report (the command cooldown).
	std::array<std::array<int64_t, 2>, kMaxSlots> m_last_top_request{};
	std::array<int64_t, kMaxSlots> m_last_rank_request{};
	// CCSGameRules::m_bWarmupPeriod offset: -2 = not resolved yet, -1 = unknown (logged once).
	int m_warmup_offset = -2;
	bool m_warmup_field_missing_logged = false;

	std::chrono::milliseconds m_since_tick{0};
	// round_end with lr_giveexp_roundend = 0 clears the statistic gate on the next frame.
	bool m_clear_allow_next_update = false;
};

// The one instance (the exports reach it through g_lr_plugin, the listeners through this name).
extern LrPlugin g_lr_plugin;

} // namespace levels_ranks
