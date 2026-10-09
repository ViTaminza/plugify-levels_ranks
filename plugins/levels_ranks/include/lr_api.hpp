#pragma once
// Consumer-side wrapper for the levels_ranks plugin API.
//
// The generated headers (levels_ranks/levels_ranks.hpp and friends, produced by bin/plugify-gen
// from levels_ranks.pplugin) expose the raw boundary: plain C function pointers, plg::string /
// plg::vector values and no captures. This header restores the shape the Pisex LR modules were
// written against:
//
//   * std::function callbacks with captures (the ILRApi typedefs), dispatched from static
//     trampolines - one listener per plugin and event, registering again replaces it, exactly
//     like the PluginId-keyed maps of the original;
//   * std::string / std::vector<int> / std::vector<std::string> instead of plg types;
//   * the LR_*/ST_* constants of include/lvl_ranks.h;
//   * the varargs of PrintToChat (Plugify cannot forward them, so they are formatted here);
//   * the IGameEvent* of the two kill hooks: Plugify moves the s2sdk event handle as an int64 and
//     this header rebuilds the pisex::IGameEvent the modules read (needs pisexcompat, which every
//     LR module links anyway).
//
// Porting a module is then mostly mechanical: keep `g_pLRCore` and make it point at
// levels_ranks::LrApi::Get(), drop the SourceMM::PluginId argument of the Hook* calls, everything
// else compiles as it did before.
//
//   #include <lr_api.hpp>                       // -I plugins/levels_ranks/include
//   #include <levels_ranks/levels_ranks.hpp>    // -I .../include/external/plugify/include
//
//   levels_ranks::LrApi* g_pLRCore = &levels_ranks::LrApi::Get();
//
//   g_pLRCore->HookOnCoreIsReady([]{ g_pLRCore->HookOnPlayerLoaded(OnPlayerLoaded); });
//   if (g_pLRCore->CheckCountPlayers()) { g_pLRCore->ChangeClientValue(iSlot, 10); }
//
// Lifetime notes:
//   * GetTableName() returns a pointer into the cache of this plugin: valid until the same getter
//     is called again, exactly like the original's g_sTableName (which never changed).
//   * The event pointer handed to the kill hooks is only valid inside the callback - it is the
//     s2sdk event of the frame, the same guarantee the original's IGameEvent* had.
//   * Every trampoline swallows exceptions (the boundary must not unwind into the plugin) and logs
//     them through plg::Log.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <levels_ranks/levels_ranks.hpp>

#if defined(__has_include)
#	if __has_include(<pisex/events.hpp>)
#		include <pisex/events.hpp>
#		define LR_API_HAS_PISEX_EVENTS 1
#	endif
#	if __has_include(<plgdb/db.hpp>)
#		include <plgdb/db.hpp>
#		define LR_API_HAS_PLGDB 1
#	endif
#endif

#if !LR_API_HAS_PISEX_EVENTS
// Without pisexcompat the kill hooks still exist, but no event is handed to them (the modules
// always link it, so this is only a compile-time fallback for unusual consumers).
namespace pisex {
class IGameEvent;
}
#endif

namespace levels_ranks {

// --- the LR_SettingType enumerators (include/lvl_ranks.h) --------------------------------------
inline constexpr int32_t LR_FlagAdminmenu = 0;
inline constexpr int32_t LR_MinplayersCount = 1;
inline constexpr int32_t LR_ShowResetMyStats = 2;
inline constexpr int32_t LR_ResetMyStatsCooldown = 3;
inline constexpr int32_t LR_ShowUsualMessage = 4;
inline constexpr int32_t LR_ShowSpawnMessage = 5;
inline constexpr int32_t LR_ShowAllLevelUpMessage = 6;
inline constexpr int32_t LR_ShowAllLevelDownMessage = 7;
inline constexpr int32_t LR_ShowRankMessage = 8;
inline constexpr int32_t LR_GiveExpRoundEnd = 9;
inline constexpr int32_t LR_ShowRankList = 10;
inline constexpr int32_t LR_BlockWarmup = 11;
inline constexpr int32_t LR_AllAgainstAll = 12;
inline constexpr int32_t LR_CleanDB_Days = 13;
inline constexpr int32_t LR_CleanDB_BanClient = 14;
inline constexpr int32_t LR_DB_SaveDataPlayer_Mode = 15;
inline constexpr int32_t LR_DB_Allow_UTF8MB4 = 16;
inline constexpr int32_t LR_DB_Charset_Type = 17;
inline constexpr int32_t LR_TopCount = 18;
inline constexpr int32_t LR_StartPoints = 19;
inline constexpr int32_t LR_TypeStatistics = 20;
inline constexpr int32_t LR_OnlineID = 21;
inline constexpr int32_t LR_ShowLevelUpMessage = 22;
inline constexpr int32_t LR_ShowLevelDownMessage = 23;

// --- the LR_SettingStatsType enumerators -------------------------------------------------------
inline constexpr int32_t LR_ExpKill = 0;
inline constexpr int32_t LR_ExpKillIsBot = 1;
inline constexpr int32_t LR_ExpDeath = 2;
inline constexpr int32_t LR_ExpDeathIsBot = 3;
inline constexpr int32_t LR_ExpKillCoefficient = 4;
inline constexpr int32_t LR_ExpGiveHeadShot = 5;
inline constexpr int32_t LR_ExpGiveAssist = 6;
inline constexpr int32_t LR_ExpGiveSuicide = 7;
inline constexpr int32_t LR_ExpGiveTeamKill = 8;
inline constexpr int32_t LR_ExpRoundWin = 9;
inline constexpr int32_t LR_ExpRoundLose = 10;
inline constexpr int32_t LR_ExpRoundMVP = 11;
inline constexpr int32_t LR_ExpBombPlanted = 12;
inline constexpr int32_t LR_ExpBombDefused = 13;
inline constexpr int32_t LR_ExpBombDropped = 14;
inline constexpr int32_t LR_ExpBombPickup = 15;
inline constexpr int32_t LR_ExpHostageKilled = 16;
inline constexpr int32_t LR_ExpHostageRescued = 17;

// --- the LR_StatsType enumerators --------------------------------------------------------------
inline constexpr int32_t ST_EXP = 0;
inline constexpr int32_t ST_RANK = 1;
inline constexpr int32_t ST_KILLS = 2;
inline constexpr int32_t ST_DEATHS = 3;
inline constexpr int32_t ST_SHOOTS = 4;
inline constexpr int32_t ST_HITS = 5;
inline constexpr int32_t ST_HEADSHOTS = 6;
inline constexpr int32_t ST_ASSISTS = 7;
inline constexpr int32_t ST_ROUNDSWIN = 8;
inline constexpr int32_t ST_ROUNDSLOSE = 9;
inline constexpr int32_t ST_PLAYTIME = 10;
inline constexpr int32_t ST_PLACEINTOP = 11;
inline constexpr int32_t ST_PLACEINTOPTIME = 12;

// --- the LR_CleanupType enumerators (the argument of OnDatabaseCleanup) ------------------------
inline constexpr int32_t LR_AllData = 0;
inline constexpr int32_t LR_ExpData = 1;
inline constexpr int32_t LR_StatsData = 2;

// --- callback types (the ILRApi typedefs, captured std::function) -------------------------------
using OnCoreIsReady = std::function<void()>;
using OnLevelChangedPre = std::function<void(int iSlot, int& iNewLevel, int iOldLevel)>;
using OnLevelChangedPost = std::function<void(int iSlot, int iNewLevel, int iOldLevel)>;
using OnPlayerKilledPre = std::function<void(pisex::IGameEvent* hEvent, int& iExpCaused, int iExpVictim, int iExpAttacker)>;
using OnPlayerKilledPost = std::function<void(pisex::IGameEvent* hEvent, int iExpCaused, int iExpVictim, int iExpAttacker)>;
using OnPlayerLoaded = std::function<void(int iSlot, const char* SteamID)>;
using OnResetPlayerStats = std::function<void(int iSlot, const char* SteamID)>;
using OnPlayerPosInTop = std::function<void(int iSlot, int iExpPos, int iTimePos)>;
using OnExpChangedPre = std::function<void(int iSlot, int& iGiveExp)>;
using OnExpChangedPost = std::function<void(int iSlot, int iGiveExp, int iNewExpCount)>;
using OnDatabaseCleanup = std::function<void(int iType)>;

// --- database settings --------------------------------------------------------------------------
// The values the original handed out through GetDatabases() (a live IMySQLConnection*), minus the
// connection itself: a Plugify plugin cannot share another plugin's connection object, so every
// module opens its own plgdb connection with these settings. The table name is GetTableName().
struct DatabaseConfig {
	std::string driver = "mysql";   // "mysql" or "sqlite"
	std::string host = "127.0.0.1"; // sqlite: path of the database file
	uint16_t port = 3306;
	std::string user;
	std::string password;
	std::string database;

	bool IsSqlite() const noexcept { return driver == "sqlite"; }
};

namespace detail {

inline std::string ToStd(const plg::string& value) {
	return std::string(std::string_view(value));
}

inline void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

// True when the plugify module filled the binding slots, i.e. when levels_ranks is loaded.
inline bool Available() {
	return __levels_ranks_IsCoreLoaded != nullptr;
}

// One listener per event, exactly like the PluginId-keyed maps of the original: registering again
// replaces the previous one.
struct State {
	OnCoreIsReady coreReady;
	OnLevelChangedPre levelChangedPre;
	OnLevelChangedPost levelChangedPost;
	OnPlayerKilledPre playerKilledPre;
	OnPlayerKilledPost playerKilledPost;
	OnPlayerLoaded playerLoaded;
	OnResetPlayerStats resetPlayerStats;
	OnPlayerPosInTop playerPosInTop;
	OnExpChangedPre expChangedPre;
	OnExpChangedPost expChangedPost;
	OnDatabaseCleanup databaseCleanup;

	// Stable addresses: node based container, so the returned c_str() survives later getters.
	std::map<std::string, std::string, std::less<>> strings;
};

inline State& S() {
	static State state;
	return state;
}

// --- trampolines --------------------------------------------------------------------------------
// Plain function pointers (they cross the boundary), captures live in State. No exception may
// escape into the levels_ranks plugin.

inline void CoreReadyTrampoline() {
	try {
		if (const auto& callback = S().coreReady) {
			callback();
		}
	} catch (...) {
		LogError("levels_ranks: OnCoreIsReady callback threw");
	}
}

inline void LevelChangedPreTrampoline(int32_t slot, int32_t& new_level, int32_t old_level) {
	try {
		if (const auto& callback = S().levelChangedPre) {
			int level = new_level; // the module sees an int&
			callback(slot, level, old_level);
			new_level = level;
		}
	} catch (...) {
		LogError("levels_ranks: OnLevelChangedPre callback threw");
	}
}

inline void LevelChangedPostTrampoline(int32_t slot, int32_t new_level, int32_t old_level) {
	try {
		if (const auto& callback = S().levelChangedPost) {
			callback(slot, new_level, old_level);
		}
	} catch (...) {
		LogError("levels_ranks: OnLevelChangedPost callback threw");
	}
}

inline void PlayerKilledPreTrampoline(int64_t event, int32_t& exp_caused, int32_t exp_victim, int32_t exp_attacker) {
	try {
		if (const auto& callback = S().playerKilledPre) {
			int caused = exp_caused;
#if LR_API_HAS_PISEX_EVENTS
			pisex::IGameEvent handle(reinterpret_cast<void*>(static_cast<intptr_t>(event)));
			callback(&handle, caused, exp_victim, exp_attacker);
#else
			(void)event;
			callback(nullptr, caused, exp_victim, exp_attacker);
#endif
			exp_caused = caused;
		}
	} catch (...) {
		LogError("levels_ranks: OnPlayerKilledPre callback threw");
	}
}

inline void PlayerKilledPostTrampoline(int64_t event, int32_t exp_caused, int32_t exp_victim, int32_t exp_attacker) {
	try {
		if (const auto& callback = S().playerKilledPost) {
#if LR_API_HAS_PISEX_EVENTS
			pisex::IGameEvent handle(reinterpret_cast<void*>(static_cast<intptr_t>(event)));
			callback(&handle, exp_caused, exp_victim, exp_attacker);
#else
			(void)event;
			callback(nullptr, exp_caused, exp_victim, exp_attacker);
#endif
		}
	} catch (...) {
		LogError("levels_ranks: OnPlayerKilledPost callback threw");
	}
}

inline void PlayerLoadedTrampoline(int32_t slot, const plg::string& steamid) {
	try {
		// The pointer is valid for the call, like the original's szAuth.c_str().
		if (const auto& callback = S().playerLoaded) {
			callback(slot, steamid.c_str());
		}
	} catch (...) {
		LogError("levels_ranks: OnPlayerLoaded callback threw");
	}
}

inline void ResetPlayerStatsTrampoline(int32_t slot, const plg::string& steamid) {
	try {
		if (const auto& callback = S().resetPlayerStats) {
			callback(slot, steamid.c_str());
		}
	} catch (...) {
		LogError("levels_ranks: OnResetPlayerStats callback threw");
	}
}

inline void PlayerPosInTopTrampoline(int32_t slot, int32_t exp_pos, int32_t time_pos) {
	try {
		if (const auto& callback = S().playerPosInTop) {
			callback(slot, exp_pos, time_pos);
		}
	} catch (...) {
		LogError("levels_ranks: OnPlayerPosInTop callback threw");
	}
}

inline void ExpChangedPreTrampoline(int32_t slot, int32_t& give_exp) {
	try {
		if (const auto& callback = S().expChangedPre) {
			int value = give_exp;
			callback(slot, value);
			give_exp = value;
		}
	} catch (...) {
		LogError("levels_ranks: OnExpChangedPre callback threw");
	}
}

inline void ExpChangedPostTrampoline(int32_t slot, int32_t give_exp, int32_t new_exp_count) {
	try {
		if (const auto& callback = S().expChangedPost) {
			callback(slot, give_exp, new_exp_count);
		}
	} catch (...) {
		LogError("levels_ranks: OnExpChangedPost callback threw");
	}
}

inline void DatabaseCleanupTrampoline(int32_t type) {
	try {
		if (const auto& callback = S().databaseCleanup) {
			callback(type);
		}
	} catch (...) {
		LogError("levels_ranks: OnDatabaseCleanup callback threw");
	}
}

// --- plg/std conversions ------------------------------------------------------------------------

inline std::vector<std::string> ToStdList(const plg::vector<plg::string>& values) {
	std::vector<std::string> result;
	result.reserve(values.size());
	for (const plg::string& value : values) {
		result.emplace_back(ToStd(value));
	}
	return result;
}

inline std::vector<int> ToIntList(const plg::vector<int32_t>& values) {
	return std::vector<int>(values.begin(), values.end());
}

} // namespace detail

// --- the API -------------------------------------------------------------------------------------
class LrApi {
public:
	static LrApi& Get() {
		static LrApi instance;
		return instance;
	}

	// False when the levels_ranks plugin is not loaded: every getter then answers with its
	// neutral value instead of dereferencing a null slot.
	bool Available() const { return detail::Available(); }

	// --- core -------------------------------------------------------------------------------
	bool CoreIsLoaded() {
		return detail::Available() && levels_ranks::IsCoreLoaded();
	}
	int GetSettingsValue(int setting) {
		return detail::Available() ? levels_ranks::GetSettingsValue(setting) : 0;
	}
	int GetSettingsStatsValue(int setting) {
		return detail::Available() ? levels_ranks::GetSettingsStatsValue(setting) : 0;
	}
	int GetCountPlayers() {
		return detail::Available() ? levels_ranks::GetCountPlayers() : 0;
	}
	// The original returned g_sTableName, a pointer that lived for the plugin's lifetime: the
	// value is cached here, so the pointer stays valid until the table name changes (it never
	// does after the configuration was read).
	const char* GetTableName() {
		detail::State& state = detail::S();
		if (detail::Available()) {
			state.strings["table"] = detail::ToStd(levels_ranks::GetTableName());
		}
		const auto it = state.strings.find("table");
		return it == state.strings.end() ? "" : it->second.c_str();
	}
	std::vector<std::string> GetRankNames() {
		if (!detail::Available()) {
			return {};
		}
		return detail::ToStdList(levels_ranks::GetRankNames());
	}
	std::vector<int> GetRankExp() {
		if (!detail::Available()) {
			return {};
		}
		return detail::ToIntList(levels_ranks::GetRankExp());
	}
	bool GetClientStatus(int slot) {
		return detail::Available() && levels_ranks::GetClientStatus(slot);
	}
	bool CheckCountPlayers() {
		return detail::Available() && levels_ranks::CheckCountPlayers();
	}
	int GetClientInfo(int slot, int statsType, bool session = false) {
		return detail::Available() ? levels_ranks::GetClientInfo(slot, statsType, session) : 0;
	}
	void RoundWithoutValue() {
		if (detail::Available()) {
			levels_ranks::RoundWithoutValue();
		}
	}
	bool ChangeClientValue(int slot, int giveExp) {
		return detail::Available() && levels_ranks::ChangeClientValue(slot, giveExp);
	}
	void ResetPlayerStats(int slot) {
		if (detail::Available()) {
			levels_ranks::ResetPlayerStats(slot);
		}
	}

	// --- chat --------------------------------------------------------------------------------
	// The varargs of the original (ClientPrint with a printf format); formatting happens here,
	// the message is printed by the core with the 'Prefix' phrase in front.
	void PrintToChat(int slot, const char* format, ...) __attribute__((format(printf, 3, 4))) {
		if (!detail::Available()) {
			return;
		}
		char buffer[512]; // the original's ClientPrint buffer
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		levels_ranks::PrintToChat(slot, plg::string(buffer));
	}

	// --- database ------------------------------------------------------------------------------
	DatabaseConfig GetDatabaseConfig() {
		DatabaseConfig config;
		if (!detail::Available()) {
			return config;
		}
		const plg::vector<plg::string> values = levels_ranks::GetDatabaseConfig();
		if (values.size() >= 6) {
			config.driver = detail::ToStd(values[0]);
			config.host = detail::ToStd(values[1]);
			const int port = std::atoi(detail::ToStd(values[2]).c_str());
			config.port = port > 0 && port <= 65535 ? static_cast<uint16_t>(port) : 3306;
			config.user = detail::ToStd(values[3]);
			config.password = detail::ToStd(values[4]);
			config.database = detail::ToStd(values[5]);
		}
		return config;
	}

	// --- hooks -----------------------------------------------------------------------------------
	// The PluginId argument of the original is gone: one listener per event and plugin.
	void HookOnCoreIsReady(const OnCoreIsReady& fn) {
		detail::S().coreReady = fn;
		Register(__levels_ranks_OnCoreIsReady_Register, detail::CoreReadyTrampoline, fn != nullptr);
	}
	void UnhookOnCoreIsReady() {
		detail::S().coreReady = nullptr;
		Unregister(__levels_ranks_OnCoreIsReady_Unregister, detail::CoreReadyTrampoline);
	}

	void HookOnLevelChangedPre(const OnLevelChangedPre& fn) {
		detail::S().levelChangedPre = fn;
		Register(__levels_ranks_OnLevelChangedPre_Register, detail::LevelChangedPreTrampoline, fn != nullptr);
	}
	void UnhookOnLevelChangedPre() {
		detail::S().levelChangedPre = nullptr;
		Unregister(__levels_ranks_OnLevelChangedPre_Unregister, detail::LevelChangedPreTrampoline);
	}

	void HookOnLevelChangedPost(const OnLevelChangedPost& fn) {
		detail::S().levelChangedPost = fn;
		Register(__levels_ranks_OnLevelChangedPost_Register, detail::LevelChangedPostTrampoline, fn != nullptr);
	}
	void UnhookOnLevelChangedPost() {
		detail::S().levelChangedPost = nullptr;
		Unregister(__levels_ranks_OnLevelChangedPost_Unregister, detail::LevelChangedPostTrampoline);
	}

	void HookOnPlayerKilledPre(const OnPlayerKilledPre& fn) {
		detail::S().playerKilledPre = fn;
		Register(__levels_ranks_OnPlayerKilledPre_Register, detail::PlayerKilledPreTrampoline, fn != nullptr);
	}
	void UnhookOnPlayerKilledPre() {
		detail::S().playerKilledPre = nullptr;
		Unregister(__levels_ranks_OnPlayerKilledPre_Unregister, detail::PlayerKilledPreTrampoline);
	}

	void HookOnPlayerKilledPost(const OnPlayerKilledPost& fn) {
		detail::S().playerKilledPost = fn;
		Register(__levels_ranks_OnPlayerKilledPost_Register, detail::PlayerKilledPostTrampoline, fn != nullptr);
	}
	void UnhookOnPlayerKilledPost() {
		detail::S().playerKilledPost = nullptr;
		Unregister(__levels_ranks_OnPlayerKilledPost_Unregister, detail::PlayerKilledPostTrampoline);
	}

	void HookOnPlayerLoaded(const OnPlayerLoaded& fn) {
		detail::S().playerLoaded = fn;
		Register(__levels_ranks_OnPlayerLoaded_Register, detail::PlayerLoadedTrampoline, fn != nullptr);
	}
	void UnhookOnPlayerLoaded() {
		detail::S().playerLoaded = nullptr;
		Unregister(__levels_ranks_OnPlayerLoaded_Unregister, detail::PlayerLoadedTrampoline);
	}

	void HookOnResetPlayerStats(const OnResetPlayerStats& fn) {
		detail::S().resetPlayerStats = fn;
		Register(__levels_ranks_OnResetPlayerStats_Register, detail::ResetPlayerStatsTrampoline, fn != nullptr);
	}
	void UnhookOnResetPlayerStats() {
		detail::S().resetPlayerStats = nullptr;
		Unregister(__levels_ranks_OnResetPlayerStats_Unregister, detail::ResetPlayerStatsTrampoline);
	}

	void HookOnPlayerPosInTop(const OnPlayerPosInTop& fn) {
		detail::S().playerPosInTop = fn;
		Register(__levels_ranks_OnPlayerPosInTop_Register, detail::PlayerPosInTopTrampoline, fn != nullptr);
	}
	void UnhookOnPlayerPosInTop() {
		detail::S().playerPosInTop = nullptr;
		Unregister(__levels_ranks_OnPlayerPosInTop_Unregister, detail::PlayerPosInTopTrampoline);
	}

	void HookOnExpChangedPre(const OnExpChangedPre& fn) {
		detail::S().expChangedPre = fn;
		Register(__levels_ranks_OnExpChangedPre_Register, detail::ExpChangedPreTrampoline, fn != nullptr);
	}
	void UnhookOnExpChangedPre() {
		detail::S().expChangedPre = nullptr;
		Unregister(__levels_ranks_OnExpChangedPre_Unregister, detail::ExpChangedPreTrampoline);
	}

	void HookOnExpChangedPost(const OnExpChangedPost& fn) {
		detail::S().expChangedPost = fn;
		Register(__levels_ranks_OnExpChangedPost_Register, detail::ExpChangedPostTrampoline, fn != nullptr);
	}
	void UnhookOnExpChangedPost() {
		detail::S().expChangedPost = nullptr;
		Unregister(__levels_ranks_OnExpChangedPost_Unregister, detail::ExpChangedPostTrampoline);
	}

	void HookOnDatabaseCleanup(const OnDatabaseCleanup& fn) {
		detail::S().databaseCleanup = fn;
		Register(__levels_ranks_OnDatabaseCleanup_Register, detail::DatabaseCleanupTrampoline, fn != nullptr);
	}
	void UnhookOnDatabaseCleanup() {
		detail::S().databaseCleanup = nullptr;
		Unregister(__levels_ranks_OnDatabaseCleanup_Unregister, detail::DatabaseCleanupTrampoline);
	}

private:
	LrApi() = default; // the singleton is constructed inside Get()

	// Every registration slot is optional (the plugin may be missing); a null slot is a no-op.
	template <typename Slot, typename Trampoline>
	static void Register(Slot& slot, Trampoline trampoline, bool wanted) {
		if (!wanted || slot == nullptr) {
			return;
		}
		try {
			slot(trampoline);
		} catch (...) {
			detail::LogError("levels_ranks: registering a listener failed");
		}
	}

	template <typename Slot, typename Trampoline>
	static void Unregister(Slot& slot, Trampoline trampoline) {
		if (slot == nullptr) {
			return; // the plugin is not loaded: nothing was registered either
		}
		try {
			slot(trampoline);
		} catch (...) {
			detail::LogError("levels_ranks: removing a listener failed");
		}
	}
};

} // namespace levels_ranks

#if defined(LR_API_HAS_PLGDB)
#include <plgdb/db.hpp>
namespace levels_ranks {

// Opens a plgdb connection for a module that runs its own queries (the LR modules keep the
// <table>_hits / _weapons / _maps / _geoip / _unusualkills tables next to the core's one).
// |sqlite_file| is used when the core itself fell back to SQLite (no MySQL in databases.cfg).
inline plgdb::Config MakeDatabaseConfig(const DatabaseConfig& config, std::string_view sqlite_file) {
	plgdb::Config result;
	if (config.IsSqlite()) {
		result.driver = plgdb::Driver::SQLite;
		result.sqlitePath = config.host.empty() ? std::string(sqlite_file) : config.host;
		return result;
	}
	result.driver = plgdb::Driver::MySQL;
	result.host = config.host;
	result.port = config.port;
	result.user = config.user;
	result.password = config.password;
	result.database = config.database;
	return result;
}

} // namespace levels_ranks
#endif
