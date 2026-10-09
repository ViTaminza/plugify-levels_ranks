#pragma once
// Engine-independent core of the "levels_ranks" plugin: the settings model of settings.ini, the
// ranks/experience/statistics logic and the SQL of the original (one table, name from lr_table).
//
// No Plugify, s2sdk or engine headers here - only plgdb - so the whole module is unit-testable
// against SQLite and MySQL (plugins/levels_ranks/tests/lr_core_tests.cpp). plugin.cpp owns the
// engine half: the s2sdk event hooks, commands, menus, player/permission lookups and the printed
// messages; this core owns every counter, every query and every rule of
// ref/src/cs2-lvl_ranks/levels_ranks.cpp.
//
// Compatibility contract with the original Metamod plugin:
//   * the table layout, the statements and the computed values are those of levels_ranks.cpp;
//     the deviations below are the only ones and each is marked "PARITY:" in the code.
//     - every statement is parameterised (plgdb binds '?' placeholders). The original
//       interpolated strings and escaped the player name with IMySQLConnection::Escape; the
//       values that reach the database are the same.
//     - the MySQL-only charset statements (SET NAMES / SET CHARSET / ALTER TABLE ... CHARACTER
//       SET|MODIFY COLUMN) are run for the MySQL driver and skipped for the SQLite fallback,
//       which has neither.
//     - the settings array is 24 entries: the LR_SettingType enum of include/lvl_ranks.h has 24
//       enumerators while the original's `int g_Settings[22]` only had 22 and wrote past its end
//       for two of them (see kShowLevelUp below). The values a module reads are unchanged.
//     - a Rating_Extended kill by a player with 0 experience divided by zero in the original
//       (undefined behaviour); here the ratio is treated as 1.
//   * every statement is asynchronous (plgdb) and its effects are applied from the callback, which
//     plgdb only runs inside Poll() on the game thread. CreateSchema() (and the schema half of
//     ConfigureDatabase()) is the only blocking statement, called once at plugin start.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <plgdb/db.hpp>

namespace levels_ranks {

// --- constants -------------------------------------------------------------------------------
inline constexpr int kMaxSlots = 64;      // the original's g_iPlayerInfo[64]
inline constexpr int kStatsCount = 13;    // LR_StatsType / ST_* array width
inline constexpr int kSettingCount = 24;  // LR_SettingType enumerators of include/lvl_ranks.h
inline constexpr int kStatsSettingCount = 18; // LR_SettingStatsType enumerators
inline constexpr int kBonusCount = 10;    // g_iBonus[10] (lr_bonus_1 .. lr_bonus_10)

// The LR_SettingType enumerators (include/lvl_ranks.h, the values a module passes to
// GetSettingsValue). Kept as constants instead of an enum class so the indexing of the settings
// array stays readable.
inline constexpr int LR_FlagAdminmenu = 0;
inline constexpr int LR_MinplayersCount = 1;
inline constexpr int LR_ShowResetMyStats = 2;
inline constexpr int LR_ResetMyStatsCooldown = 3;
inline constexpr int LR_ShowUsualMessage = 4;
inline constexpr int LR_ShowSpawnMessage = 5;
inline constexpr int LR_ShowAllLevelUpMessage = 6;
inline constexpr int LR_ShowAllLevelDownMessage = 7;
inline constexpr int LR_ShowRankMessage = 8;
inline constexpr int LR_GiveExpRoundEnd = 9;
inline constexpr int LR_ShowRankList = 10;
inline constexpr int LR_BlockWarmup = 11;
inline constexpr int LR_AllAgainstAll = 12;
inline constexpr int LR_CleanDB_Days = 13;
inline constexpr int LR_CleanDB_BanClient = 14;
inline constexpr int LR_DB_SaveDataPlayer_Mode = 15;
inline constexpr int LR_DB_Allow_UTF8MB4 = 16;
inline constexpr int LR_DB_Charset_Type = 17;
inline constexpr int LR_TopCount = 18;
inline constexpr int LR_StartPoints = 19;
inline constexpr int LR_TypeStatistics = 20;
inline constexpr int LR_OnlineID = 21;
inline constexpr int LR_ShowLevelUpMessage = 22;
inline constexpr int LR_ShowLevelDownMessage = 23;

// PARITY: the core's *own* macro block of levels_ranks.h names the two "show the level up/down
// message" keys 6 and 7 - the same slots the interface enum calls LR_ShowAllLevelUpMessage /
// LR_ShowAllLevelDownMessage. The original therefore stored lr_show_levelup_message and
// lr_show_all_levelup_message in one slot (the later read won) and used that single slot both for
// the personal message and for the broadcast; the interface's LR_ShowLevelUpMessage (22) and
// LR_ShowLevelDownMessage (23) were never written and always read 0. The port keeps that.
inline constexpr int kShowLevelUp = LR_ShowAllLevelUpMessage;     // 6
inline constexpr int kShowLevelDown = LR_ShowAllLevelDownMessage; // 7

// LR_SettingStatsType enumerators.
inline constexpr int LR_ExpKill = 0;
inline constexpr int LR_ExpKillIsBot = 1;
inline constexpr int LR_ExpDeath = 2;
inline constexpr int LR_ExpDeathIsBot = 3;
inline constexpr int LR_ExpKillCoefficient = 4;
inline constexpr int LR_ExpGiveHeadShot = 5;
inline constexpr int LR_ExpGiveAssist = 6;
inline constexpr int LR_ExpGiveSuicide = 7;
inline constexpr int LR_ExpGiveTeamKill = 8;
inline constexpr int LR_ExpRoundWin = 9;
inline constexpr int LR_ExpRoundLose = 10;
inline constexpr int LR_ExpRoundMVP = 11;
inline constexpr int LR_ExpBombPlanted = 12;
inline constexpr int LR_ExpBombDefused = 13;
inline constexpr int LR_ExpBombDropped = 14;
inline constexpr int LR_ExpBombPickup = 15;
inline constexpr int LR_ExpHostageKilled = 16;
inline constexpr int LR_ExpHostageRescued = 17;

// LR_StatsType enumerators (the indexes of the per-player arrays).
inline constexpr int ST_EXP = 0;
inline constexpr int ST_RANK = 1;
inline constexpr int ST_KILLS = 2;
inline constexpr int ST_DEATHS = 3;
inline constexpr int ST_SHOOTS = 4;
inline constexpr int ST_HITS = 5;
inline constexpr int ST_HEADSHOTS = 6;
inline constexpr int ST_ASSISTS = 7;
inline constexpr int ST_ROUNDSWIN = 8;
inline constexpr int ST_ROUNDSLOSE = 9;
inline constexpr int ST_PLAYTIME = 10;
inline constexpr int ST_PLACEINTOP = 11;
inline constexpr int ST_PLACEINTOPTIME = 12;

// LR_CleanupType enumerators (the argument of the OnDatabaseCleanup callbacks).
inline constexpr int LR_AllData = 0;
inline constexpr int LR_ExpData = 1;
inline constexpr int LR_StatsData = 2;

// The experience the "Rating" statistics modes never fall below (the iExpBuffer of the original).
inline constexpr int kRatingFloor = 400;
// Start experience of the two rating modes; the "Funded" mode starts at lr_start_points.
inline constexpr int kRatingStartExp = 1000;

// --- settings ---------------------------------------------------------------------------------
// The model of settings.ini. The defaults are the ones of LoadConfig() in levels_ranks.cpp; the
// file is parsed by the plugin (pisexcompat KeyValues) and handed over through LoadSettings().
struct Settings {
	std::string table_name = "lvl_base";  // lr_table
	std::string plugin_title;             // lr_plugin_title (no default in the original: empty)
	int type_statistics = 0;              // lr_type_statistics, mirrored into values[] as well
	// g_Settings[LR_SettingType]; out-of-range indexes answer 0.
	std::array<int, kSettingCount> values{};
	// g_SettingsStats[LR_SettingStatsType].
	std::array<int, kStatsSettingCount> stats{};
	// g_iBonus (lr_bonus_1 .. lr_bonus_10).
	std::array<int, kBonusCount> bonus{};
	// g_hRankNames / g_hRankExp, in configuration order.
	std::vector<std::string> rank_names;
	std::vector<int> rank_exp;

	// The value of an LR_SettingType index (0 outside the array, like the original's macros that
	// indexed past their end are avoided here).
	int Value(int setting) const {
		return setting >= 0 && setting < kSettingCount ? values[static_cast<std::size_t>(setting)] : 0;
	}
	// The value of an LR_SettingStatsType index.
	int Stats(int setting) const {
		return setting >= 0 && setting < kStatsSettingCount ? stats[static_cast<std::size_t>(setting)] : 0;
	}

	// The table name is interpolated into every statement (plgdb cannot bind identifiers), so it
	// must be a plain identifier. Rejected names are replaced by the default.
	bool HasValidTableName() const;
};

// --- the configuration source -----------------------------------------------------------------
// What LoadSettings() needs of a KV1 tree, so the core does not depend on the pisexcompat parser:
// the plugin implements it over pisex::KeyValues, the tests over a literal tree. |section| is the
// name of a KV1 block; "" is the root node ("LR_Settings" of the original), whose *direct* child
// lookup is what made the original read lr_resetmystats_cooldown from the wrong node (see
// LoadSettings).
class ConfigView {
public:
	virtual ~ConfigView() = default;

	// True when the block exists.
	virtual bool HasSection(std::string_view section) const = 0;
	virtual std::string GetString(std::string_view section, std::string_view key, std::string_view def) const = 0;
	virtual float GetFloat(std::string_view section, std::string_view key, float def) const = 0;
	// GetInt of the original, including its "missing key -> def" behaviour. A key that only exists
	// in a deeper block is missing (KV1 GetInt(key) looks at direct children only).
	virtual int GetInt(std::string_view section, std::string_view key, int def) const = 0;
	// The "key value" pairs of a block in file order (the Ranks block: phrase key -> experience).
	virtual std::vector<std::pair<std::string, int>> GetIntValues(std::string_view section) const = 0;
};

// LoadConfig() of the original: every value and every default of levels_ranks.cpp, including the
// lr_killcoeff clamp to [0.5, 1.5] and the "lr_show_levelup_message wins over
// lr_show_all_levelup_message" aliasing. |error| (optional) receives the first structural problem
// (a missing block) - the original unloaded the plugin then, the port only logs it (a Failed
// OnPluginStart would stop every Plugify plugin).
Settings LoadSettings(const ConfigView& view, std::string* error = nullptr);

// --- player state -----------------------------------------------------------------------------
// g_iPlayerInfo[slot] of the original.
struct PlayerInfo {
	bool have_bomb = false;      // bHaveBomb
	bool initialized = false;    // bInitialized
	std::string steamid;         // szAuth (STEAM_1:x:y)
	std::array<int, kStatsCount> stats{};
	std::array<int, kStatsCount> session{};
	int round_exp = 0;           // iRoundExp
	int kill_streak = 0;         // iKillStreak
};

// --- outcomes ----------------------------------------------------------------------------------
// What CheckRank() produced, so the plugin can print and fan out.
struct RankChange {
	bool changed = false;
	int slot = 0;
	int new_rank = 0;
	int old_rank = 0;
	bool up = false;
	bool saved = false; // SaveDataPlayer() was triggered (lr_db_savedataplayer_mode)
};

// What NotifClient() (GiveExp) produced.
struct ExpResult {
	bool status = false;      // CheckStatus + the statistic gate passed
	bool applied = false;     // the change was applied (value != 0)
	int requested = 0;        // the value after the OnExpChangedPre hooks
	int applied_value = 0;    // the value actually credited (iExpBuffer of the original)
	int new_exp = 0;          // the player's new experience total
	bool saved = false;       // SaveDataPlayer() was triggered
};

// One row of the top lists.
struct TopRow {
	std::string name;
	std::string steamid;
	double value = 0.0; // experience, or playtime in hours when the list is the play-time one
};

// --- the core -----------------------------------------------------------------------------------
class LrCore {
public:
	explicit LrCore(plgdb::Database& db);
	~LrCore();

	LrCore(const LrCore&) = delete;
	LrCore& operator=(const LrCore&) = delete;

	// --- configuration -------------------------------------------------------------------------
	void SetSettings(Settings settings);
	const Settings& GetSettings() const { return m_settings; }
	std::string Table() const; // `table_name`

	// --- clock ---------------------------------------------------------------------------------
	// Overridable clock so tests do not depend on the wall clock.
	void SetClock(std::function<int64_t()> clock);
	int64_t Now() const;

	// --- database ------------------------------------------------------------------------------
	// The table of the original (CREATE TABLE IF NOT EXISTS) plus, for MySQL, the ALTER/CHARSET
	// statements of the connect callback. Blocking (QuerySync): startup only, like the admin core.
	// Throws std::runtime_error when the CREATE TABLE fails; the MySQL-only statements that
	// legitimately fail on a schema created by an older version (ADD COLUMN `online`) are
	// tolerated.
	void CreateSchema();
	// The asynchronous tail of the original's connect callback: SELECT COUNT(steam) -> the player
	// count, UPDATE online = 0 WHERE online = lr_online_id, then the onCoreReady hook (the
	// SendCoreIsReady of the original).
	void ConfigureDatabase();
	// OnCleanDB(): forgets the last connection of players gone for lr_cleandb_days days.
	void CleanDatabase();

	// --- player counting -----------------------------------------------------------------------
	// g_bAllowStatistic: set by the plugin (CheckAllowStatistic needs the engine) and by
	// RoundWithoutValue().
	void SetAllowStatistic(bool allow) { m_allow_statistic = allow; }
	bool CountPlayersAllowed() const { return m_allow_statistic; }
	// g_iDBCountPlayers: the row count read at startup, incremented for every inserted player.
	int PlayerCount() const { return m_player_count; }

	// --- player state --------------------------------------------------------------------------
	const PlayerInfo* FindPlayer(int slot) const;
	PlayerInfo* FindPlayer(int slot);
	bool IsInitialized(int slot) const;
	// ResetPlayerData() of the original: clears both arrays, starts a new kill streak and sets
	// iStats[ST_PLAYTIME] to "now" (so the accumulated play time starts at 0) and the experience
	// to the start value of the active lr_type_statistics mode.
	void ResetPlayerData(int slot);
	// The side effect of CheckStatus(): a slot that is not a usable player loses bInitialized.
	void MarkUninitialized(int slot);

	// --- rank ----------------------------------------------------------------------------------
	// CheckRank() of the original: computes the rank from the player's experience, fires the pre
	// hook (which may rewrite the new rank), stores it, session-deltas the rank counter and, when
	// |active|, saves the row (lr_db_savedataplayer_mode), fires the post hook and reports the
	// level-up/level-down messages through onLevelMessage.
	RankChange CheckRank(int slot, bool active = true);

	// --- experience ----------------------------------------------------------------------------
	// NotifClient() of the original: the statistic gate, the OnExpChangedPre hook, the clamp at
	// 400 for the rating modes, the round/session counters, CheckRank() and the OnExpChangedPost
	// hook. |allow| is the "bAllow" argument of the internal callers (the admin give/take menu),
	// |phrase| the message key the caller wants printed (Kill, MyDeath, RoundWin, ...).
	ExpResult GiveExp(int slot, int value, const char* phrase, bool allow = false);
	// LRApi::ChangeClientValue(): the same math without the pre-gate and without a message.
	bool ChangeClientValue(int slot, int give_exp);
	// ResetPlayerStats() of the original: ResetPlayerData + CheckRank(false) + the
	// OnResetPlayerStats hook.
	void ResetPlayerStats(int slot);

	// --- database paths ------------------------------------------------------------------------
	// OnClientPutInServer(): SELECT the row of |steamid| (13 columns, the two COUNT sub-queries
	// included) or INSERT a fresh one, then the online/lastconnect UPDATE and the OnPlayerLoaded
	// hook. |name| is only used for the INSERT.
	void LoadOrCreatePlayer(int slot, std::string steamid, std::string name);
	// SaveDataPlayer(): REPLACE the row of the slot (name from findPlayerName) and, when not
	// disconnecting, refresh the place in the top (onPlayerPosInTop). A disconnect also clears the
	// slot and sets online = 0.
	void SavePlayer(int slot, bool disconnect = false);
	// The mm_lvl_reset command's statements: TRUNCATE / the experience reset / the statistics
	// reset (lr_cleandb_banclient is not used, exactly like the original) plus the
	// OnDatabaseCleanup hook and the completion callback the plugin logs with. |type| is
	// LR_CleanupType.
	void CleanupData(int type, std::function<void(bool ok)> done = {});
	// The two top lists of the menu (SELECT name, steam, value|playtime/3600 ... ORDER BY ...
	// LIMIT lr_top_count).
	void LoadTopPlayers(bool by_playtime, std::function<void(std::vector<TopRow>)> callback);
	// The names of the players the cleanup command reloads (the bInitialized slots).
	std::vector<int> InitializedSlots() const;

	// --- engine boundary -----------------------------------------------------------------------
	// The player's name for the saves (GetClientConVarValue(slot, "name") in the original).
	std::function<std::string(int slot)> findPlayerName;
	// Hooks. Install them before the first statement finishes; every one of them is optional.
	std::function<void()> onCoreReady;
	std::function<void(int slot, const std::string& steamid)> onPlayerLoaded;
	std::function<void(int slot, const std::string& steamid)> onResetPlayerStats;
	std::function<void(int slot, int& new_rank, int old_rank)> onLevelChangedPre;
	std::function<void(int slot, int new_rank, int old_rank)> onLevelChangedPost;
	std::function<void(int slot, int exp_pos, int time_pos)> onPlayerPosInTop;
	std::function<void(int slot, int& value)> onExpChangedPre;
	std::function<void(int slot, int value, int new_exp)> onExpChangedPost;
	std::function<void(int type)> onDatabaseCleanup;
	// A level-up/level-down message: |target| receives it, |speaker| changed the rank, |rank| is
	// the 1-based rank number (the phrase key is rank_names[rank - 1]). |personal| is the player's
	// own message (truthy when the core decided to print it); the broadcast case is reported once
	// per recipient and only when the settings ask for it.
	std::function<void(int target, int speaker, int rank, bool up, bool personal)> onLevelMessage;
	// The "usual message" of NotifClient: phrase key + the new total + the requested value.
	std::function<void(int slot, const std::string& phrase, int new_exp, int requested)> onExpMessage;
	// SQL failures and unusable results.
	std::function<void(std::string_view)> onError;

	// --- pure logic (exposed for the plugin and the tests) --------------------------------------
	// The rank of an experience value: the `while(--iRank && g_hRankExp[iRank-1] > iExp)` loop of
	// CheckRank, 1-based, 0 when the experience is below the first rank's threshold.
	static int RankForExp(int exp, const std::vector<int>& rank_exp);
	// The raw attacker gain of a kill for the two rating modes (type 0 reads lr_kill /
	// lr_kill_is_bot from the settings instead, the caller does that):
	//   1 - RoundToNearest(victim_exp / attacker_exp * 5.0). The original applies its
	//       "if (iExpAttacker < 1) iExpAttacker = 1;" *after* the OnPlayerKilledPre hook, so the
	//       minimum is applied by the caller as well. An attacker experience of 0 (not reachable
	//       through the normal paths, which clamp at 400/1000) threw a division by zero in the
	//       original; here the ratio is treated as 1 (PARITY).
	//   2 - victim_exp - attacker_exp, before the `SimpleModeExp` mapping.
	static int KillExpAttacker(int type, int attacker_exp, int victim_exp);
	// The Extended mode's victim loss: RoundToNearest(attacker_gain * coefficient_percent / 100),
	// minimum 1. |coefficient_percent| is LR_ExpKillCoefficient, i.e. lr_killcoeff * 100.
	static int RatingVictimExp(int attacker_gain, int coefficient_percent);
	// The Simple mode's mapping of the raw difference, applied to both sides of a kill (the
	// original's `iExpVictim = iExpAttacker = x < 3 ? 2 : (x / 100) + 2`).
	static int SimpleModeExp(int raw);
	// The index into g_iBonus / the streak phrases for a kill streak (the -= 2 and the 9 cap of
	// GiveExpForStreakKills); the caller only uses it when the streak is > 1.
	static int StreakBonusIndex(int kill_streak);
	// RoundToNearest()/RoundToCeil() of the original.
	static int RoundToNearest(double value);
	static int RoundToCeil(double value);

private:
	void Query(std::string sql, plgdb::Params params, plgdb::Callback callback);
	void ReportError(std::string_view message) const;
	bool SlotInRange(int slot) const;
	bool Status(int slot) const; // the database half of CheckStatus: bInitialized
	void FireCoreReady();

	plgdb::Database& m_db;
	Settings m_settings;
	std::function<int64_t()> m_clock;
	std::array<PlayerInfo, kMaxSlots> m_players;
	bool m_allow_statistic = false;
	int m_player_count = 0;
	// Kept alive by the core; callbacks plgdb runs from Poll() hold a weak reference so a result
	// landing after the core is destroyed is dropped instead of touching freed state.
	std::shared_ptr<void> m_life;
};

} // namespace levels_ranks
