#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from levels_ranks.pplugin (group: core)

namespace levels_ranks {

  using _IsCoreLoaded = bool (*)();
}
extern "C" PLUGIN_API levels_ranks::_IsCoreLoaded __levels_ranks_IsCoreLoaded;
namespace levels_ranks {
  /**
   * @brief The bActive flag of LRApi: true once the database is connected, the table exists and the configuration has been read, i.e. after the OnCoreIsReady callbacks fired. Modules that loaded late poll this instead of waiting.
   * @return bool: Whether the core finished loading.
   */
  inline bool IsCoreLoaded() {
    return __levels_ranks_IsCoreLoaded();
  }

  using _GetSettingsValue = int32_t (*)(int32_t);
}
extern "C" PLUGIN_API levels_ranks::_GetSettingsValue __levels_ranks_GetSettingsValue;
namespace levels_ranks {
  /**
   * @brief Value of the MainSettings block of settings.ini, indexed by the LR_SettingType enumerators (lvl_ranks.h). The ported constants live in lr_api.hpp (levels_ranks::LR_MinplayersCount = 1, LR_ShowUsualMessage = 4, LR_DB_SaveDataPlayer_Mode = 15, LR_TypeStatistics = 20, ...). Out-of-range indexes answer 0.
   * @param setting (int32): LR_SettingType enumerator.
   * @return int32: The setting's value (0 for an unknown index).
   */
  inline int32_t GetSettingsValue(int32_t setting) {
    return __levels_ranks_GetSettingsValue(setting);
  }

  using _GetSettingsStatsValue = int32_t (*)(int32_t);
}
extern "C" PLUGIN_API levels_ranks::_GetSettingsStatsValue __levels_ranks_GetSettingsStatsValue;
namespace levels_ranks {
  /**
   * @brief Value of the statistics block of settings.ini (Funded_System / Rating_Extended / Rating_Simple, whichever lr_type_statistics selected), indexed by the LR_SettingStatsType enumerators. LR_ExpKillCoefficient (4) holds the Rating_Extended coefficient * 100.
   * @param setting (int32): LR_SettingStatsType enumerator.
   * @return int32: The statistics setting's value (0 for an unknown index).
   */
  inline int32_t GetSettingsStatsValue(int32_t setting) {
    return __levels_ranks_GetSettingsStatsValue(setting);
  }

  using _GetCountPlayers = int32_t (*)();
}
extern "C" PLUGIN_API levels_ranks::_GetCountPlayers __levels_ranks_GetCountPlayers;
namespace levels_ranks {
  /**
   * @brief g_iDBCountPlayers of the original: the number of rows of the LR table, read once when the core connected and incremented for every player inserted afterwards. The LR menu shows it as the denominator of the player's place in the top.
   * @return int32: Rows counted in the statistics table.
   */
  inline int32_t GetCountPlayers() {
    return __levels_ranks_GetCountPlayers();
  }

  using _GetTableName = plg::string (*)();
}
extern "C" PLUGIN_API levels_ranks::_GetTableName __levels_ranks_GetTableName;
namespace levels_ranks {
  /**
   * @brief The statistics table name (lr_table of settings.ini, default lvl_base). The Pisex modules build their own tables from it (<name>_hits, <name>_weapons, <name>_maps, <name>_geoip, <name>_unusualkills), so a ported module must use this value for its queries as well.
   * @return string: Table name without backticks.
   */
  inline plg::string GetTableName() {
    return __levels_ranks_GetTableName();
  }

  using _GetRankNames = plg::vector<plg::string> (*)();
}
extern "C" PLUGIN_API levels_ranks::_GetRankNames __levels_ranks_GetRankNames;
namespace levels_ranks {
  /**
   * @brief The keys of the Ranks block of settings.ini in file order (rank_1, rank_2, ...). Each key is also a phrase key of lr_core_ranks.phrases.txt: rank number n (1-based) is displayed as the phrase of entry n-1.
   * @return string[]: Rank phrase keys, in configuration order.
   */
  inline plg::vector<plg::string> GetRankNames() {
    return __levels_ranks_GetRankNames();
  }

  using _GetRankExp = plg::vector<int32_t> (*)();
}
extern "C" PLUGIN_API levels_ranks::_GetRankExp __levels_ranks_GetRankExp;
namespace levels_ranks {
  /**
   * @brief The values of the Ranks block of settings.ini in file order - the experience needed for each rank, index 0 being the first rank (normally 0). Entry n-1 belongs to the phrase key of GetRankNames()[n-1].
   * @return int32[]: Experience thresholds, in configuration order.
   */
  inline plg::vector<int32_t> GetRankExp() {
    return __levels_ranks_GetRankExp();
  }

  using _GetClientStatus = bool (*)(int32_t);
}
extern "C" PLUGIN_API levels_ranks::_GetClientStatus __levels_ranks_GetClientStatus;
namespace levels_ranks {
  /**
   * @brief g_iPlayerInfo[slot].bInitialized of the original - true once the statistics row of a connected player was loaded (OnPlayerLoaded fired) and false again on a disconnect or when the player became unusable.
   * @param slot (int32): Player slot, 0..63.
   * @return bool: Whether the slot's statistics are loaded.
   */
  inline bool GetClientStatus(int32_t slot) {
    return __levels_ranks_GetClientStatus(slot);
  }

  using _CheckCountPlayers = bool (*)();
}
extern "C" PLUGIN_API levels_ranks::_CheckCountPlayers __levels_ranks_CheckCountPlayers;
namespace levels_ranks {
  /**
   * @brief LRApi::CheckCountPlayers(), i.e. g_bAllowStatistic: true while the statistics are being counted for the running round (enough players and, with lr_block_warmup, not in warmup). Every module checks this before awarding experience. Use RoundWithoutValue() to switch it off for the rest of the round.
   * @return bool: Whether statistics are being collected right now.
   */
  inline bool CheckCountPlayers() {
    return __levels_ranks_CheckCountPlayers();
  }

  using _GetClientInfo = int32_t (*)(int32_t, int32_t, bool);
}
extern "C" PLUGIN_API levels_ranks::_GetClientInfo __levels_ranks_GetClientInfo;
namespace levels_ranks {
  /**
   * @brief One statistics value of a slot, indexed by the LR_StatsType enumerators (ST_EXP = 0, ST_RANK = 1, ST_KILLS = 2, ST_DEATHS = 3, ST_SHOOTS = 4, ST_HITS = 5, ST_HEADSHOTS = 6, ST_ASSISTS = 7, ST_ROUNDSWIN = 8, ST_ROUNDSLOSE = 9, ST_PLAYTIME = 10, ST_PLACEINTOP = 11, ST_PLACEINTOPTIME = 12). With session = true the per-session array is read instead of the total one; a slot outside 0..63 answers 0 and an unknown index makes the call fail.
   * @param slot (int32): Player slot, 0..63.
   * @param statsType (int32): LR_StatsType enumerator, 0..12.
   * @param session (bool): Read the per-session counter instead of the total one.
   * @return int32: The requested counter (0 for an invalid slot or index).
   */
  inline int32_t GetClientInfo(int32_t slot, int32_t statsType, bool session) {
    return __levels_ranks_GetClientInfo(slot, statsType, session);
  }

  using _RoundWithoutValue = void (*)();
}
extern "C" PLUGIN_API levels_ranks::_RoundWithoutValue __levels_ranks_RoundWithoutValue;
namespace levels_ranks {
  /**
   * @brief LRApi::RoundWithoutValue(): clears g_bAllowStatistic so no experience is counted for the rest of the round (a module calls this when a round should not be rated, e.g. a warmup-like round).
   */
  inline void RoundWithoutValue() {
    return __levels_ranks_RoundWithoutValue();
  }

  using _ChangeClientValue = bool (*)(int32_t, int32_t);
}
extern "C" PLUGIN_API levels_ranks::_ChangeClientValue __levels_ranks_ChangeClientValue;
namespace levels_ranks {
  /**
   * @brief LRApi::ChangeClientValue(): adds giveExp to the player's experience (clamping at 400 while lr_type_statistics is on), updates the session counter and re-checks the rank - the exact body of the original, including its quirk that a negative result clamps the *clamped* value into iGiveExp. Returns false when the slot is not a loaded, authenticated player (CheckStatus) or the change did not happen.
   * @param slot (int32): Player slot, 0..63.
   * @param giveExp (int32): Experience to add (may be negative).
   * @return bool: true when the change was applied to a loaded player.
   */
  inline bool ChangeClientValue(int32_t slot, int32_t giveExp) {
    return __levels_ranks_ChangeClientValue(slot, giveExp);
  }

  using _ResetPlayerStats = void (*)(int32_t);
}
extern "C" PLUGIN_API levels_ranks::_ResetPlayerStats __levels_ranks_ResetPlayerStats;
namespace levels_ranks {
  /**
   * @brief LRApi::ResetPlayerStats(): resets the in-memory statistics of a loaded player to the start values, re-checks the rank without messages and fires the OnResetPlayerStats callbacks. The row itself is only rewritten on the next save (exactly like the original).
   * @param slot (int32): Player slot, 0..63.
   */
  inline void ResetPlayerStats(int32_t slot) {
    return __levels_ranks_ResetPlayerStats(slot);
  }

  using _GetDatabaseConfig = plg::vector<plg::string> (*)();
}
extern "C" PLUGIN_API levels_ranks::_GetDatabaseConfig __levels_ranks_GetDatabaseConfig;
namespace levels_ranks {
  /**
   * @brief Replacement for ILRApi::GetDatabases(): a Plugify plugin cannot share another plugin's connection object (the worker thread and the callback queue belong to the owner), so the connection settings are exported instead and every module opens its own plgdb connection. Returns exactly 6 strings: 0 driver ('mysql' or 'sqlite'), 1 host (for SQLite the file path), 2 port, 3 user, 4 password, 5 database. The table name comes from GetTableName(). Use levels_ranks::LrApi::GetDatabaseConfig() in lr_api.hpp for the named struct and MakeDatabaseConfig().
   * @return string[]: driver, host, port, user, password, database.
   */
  inline plg::vector<plg::string> GetDatabaseConfig() {
    return __levels_ranks_GetDatabaseConfig();
  }

  using _PrintToChat = void (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API levels_ranks::_PrintToChat __levels_ranks_PrintToChat;
namespace levels_ranks {
  /**
   * @brief The chat printer of LRApi::PrintToChat() minus the varargs: the message is formatted by the caller and this call prepends the 'Prefix' phrase of lr_core.phrases.txt and prints to the slot (slot -1 goes to the server console). lr_api.hpp keeps the printf-style signature of the original so ported call sites do not change.
   * @param slot (int32): Player slot, -1 for the server console.
   * @param message (string): Already formatted message.
   */
  inline void PrintToChat(int32_t slot, const plg::string& message) {
    return __levels_ranks_PrintToChat(slot, message);
  }

} // namespace levels_ranks
