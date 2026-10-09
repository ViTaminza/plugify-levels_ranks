// Tests for plugins/levels_ranks/include/lr_api.hpp - the consumer-side wrapper the eight Pisex
// LR modules are ported onto.
//
// The binding slots (__levels_ranks_*) are compiled in through $<TARGET_OBJECTS:lr_bindings> and
// stay null here; the test fills them with stubs, exactly what the plugify module does when it
// loads the levels_ranks plugin, and then exercises the wrapper: the null-slot safety, the
// plg <-> std conversions, the callback trampolines (captures, by-reference outputs, exception
// safety) and the PrintToChat varargs.

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include <plg/string.hpp>

#include <levels_ranks/levels_ranks.hpp>

#include "lr_api.hpp"

// A Plugify plugin defines this slot through PLUGIFY_PLUGIN; the consumer header only needs the
// log function, so the test provides it (null = logging stays off, the default in the modules).
namespace plg {
LogFn Log = nullptr;
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                            \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const long long a_ = static_cast<long long>(actual);                                       \
		const long long e_ = static_cast<long long>(expected);                                     \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: got %lld, expected %lld\n", __FILE__, __LINE__, a_, e_);      \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const std::string a_{actual};                                                              \
		const std::string e_{expected};                                                            \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: got \"%s\", expected \"%s\"\n", __FILE__, __LINE__,           \
						a_.c_str(), e_.c_str());                                                   \
		}                                                                                          \
	} while (false)

using levels_ranks::LrApi;

// --- stubs of the exported methods ---------------------------------------------------------

std::string g_table = "lvl_base";
plg::vector<plg::string> g_rank_names;
plg::vector<int32_t> g_rank_exp;
int32_t g_settings_value = 0;
int32_t g_stats_value = 0;
int32_t g_player_count = 0;
bool g_core_loaded = false;
bool g_allow = false;
int32_t g_client_info = 0;
std::vector<std::string> g_chat;             // the messages PrintToChat received
int32_t g_change_calls = 0;
bool g_change_result = true;
int32_t g_reset_calls = 0;
std::vector<std::string> g_db_config;

// The registered listeners.
levels_ranks::CoreReadyCallback g_core_ready_callback = nullptr;
levels_ranks::LevelChangedPreCallback g_level_pre_callback = nullptr;
levels_ranks::PlayerKilledPreCallback g_killed_pre_callback = nullptr;
levels_ranks::ExpChangedPreCallback g_exp_pre_callback = nullptr;
levels_ranks::PlayerLoadedCallback g_player_loaded_callback = nullptr;
int g_unregister_calls = 0;

bool StubIsCoreLoaded() {
	return g_core_loaded;
}

int32_t StubGetSettingsValue(int32_t setting) {
	return setting == levels_ranks::LR_MinplayersCount ? g_settings_value : -1;
}

int32_t StubGetSettingsStatsValue(int32_t setting) {
	return setting == levels_ranks::LR_ExpKill ? g_stats_value : -1;
}

int32_t StubGetCountPlayers() {
	return g_player_count;
}

plg::string StubGetTableName() {
	return plg::string(g_table);
}

plg::vector<plg::string> StubGetRankNames() {
	return g_rank_names;
}

plg::vector<int32_t> StubGetRankExp() {
	return g_rank_exp;
}

bool StubGetClientStatus(int32_t slot) {
	return slot == 3;
}

bool StubCheckCountPlayers() {
	return g_allow;
}

int32_t StubGetClientInfo(int32_t slot, int32_t stats_type, bool session) {
	g_client_info = slot * 1000 + stats_type * 10 + (session ? 1 : 0);
	return g_client_info;
}

void StubRoundWithoutValue() {
	g_allow = false;
}

bool StubChangeClientValue(int32_t, int32_t) {
	++g_change_calls;
	return g_change_result;
}

void StubResetPlayerStats(int32_t) {
	++g_reset_calls;
}

plg::vector<plg::string> StubGetDatabaseConfig() {
	plg::vector<plg::string> config;
	for (const std::string& value : g_db_config) {
		config.emplace_back(value);
	}
	return config;
}

void StubPrintToChat(int32_t slot, const plg::string& message) {
	g_chat.push_back(std::to_string(slot) + ":" + std::string(std::string_view(message)));
}

void StubRegisterCoreReady(levels_ranks::CoreReadyCallback callback) {
	g_core_ready_callback = callback;
}

void StubUnregisterCoreReady(levels_ranks::CoreReadyCallback) {
	++g_unregister_calls;
}

void StubRegisterLevelPre(levels_ranks::LevelChangedPreCallback callback) {
	g_level_pre_callback = callback;
}

void StubRegisterKilledPre(levels_ranks::PlayerKilledPreCallback callback) {
	g_killed_pre_callback = callback;
}

void StubRegisterExpPre(levels_ranks::ExpChangedPreCallback callback) {
	g_exp_pre_callback = callback;
}

void StubRegisterPlayerLoaded(levels_ranks::PlayerLoadedCallback callback) {
	g_player_loaded_callback = callback;
}

void InstallStubs() {
	__levels_ranks_IsCoreLoaded = &StubIsCoreLoaded;
	__levels_ranks_GetSettingsValue = &StubGetSettingsValue;
	__levels_ranks_GetSettingsStatsValue = &StubGetSettingsStatsValue;
	__levels_ranks_GetCountPlayers = &StubGetCountPlayers;
	__levels_ranks_GetTableName = &StubGetTableName;
	__levels_ranks_GetRankNames = &StubGetRankNames;
	__levels_ranks_GetRankExp = &StubGetRankExp;
	__levels_ranks_GetClientStatus = &StubGetClientStatus;
	__levels_ranks_CheckCountPlayers = &StubCheckCountPlayers;
	__levels_ranks_GetClientInfo = &StubGetClientInfo;
	__levels_ranks_RoundWithoutValue = &StubRoundWithoutValue;
	__levels_ranks_ChangeClientValue = &StubChangeClientValue;
	__levels_ranks_ResetPlayerStats = &StubResetPlayerStats;
	__levels_ranks_GetDatabaseConfig = &StubGetDatabaseConfig;
	__levels_ranks_PrintToChat = &StubPrintToChat;

	__levels_ranks_OnCoreIsReady_Register = &StubRegisterCoreReady;
	__levels_ranks_OnCoreIsReady_Unregister = &StubUnregisterCoreReady;
	__levels_ranks_OnLevelChangedPre_Register = &StubRegisterLevelPre;
	__levels_ranks_OnPlayerKilledPre_Register = &StubRegisterKilledPre;
	__levels_ranks_OnExpChangedPre_Register = &StubRegisterExpPre;
	__levels_ranks_OnPlayerLoaded_Register = &StubRegisterPlayerLoaded;
}

void TestUnavailable() {
	std::printf("-- unavailable slots\n");
	LrApi& api = LrApi::Get();
	CHECK(!api.Available());
	CHECK(!api.CoreIsLoaded());
	CHECK_EQ_INT(api.GetSettingsValue(levels_ranks::LR_MinplayersCount), 0);
	CHECK_EQ_INT(api.GetSettingsStatsValue(levels_ranks::LR_ExpKill), 0);
	CHECK_EQ_INT(api.GetCountPlayers(), 0);
	CHECK_EQ_STR(api.GetTableName(), "");
	CHECK(api.GetRankNames().empty());
	CHECK(api.GetRankExp().empty());
	CHECK(!api.GetClientStatus(3));
	CHECK(!api.CheckCountPlayers());
	CHECK_EQ_INT(api.GetClientInfo(1, levels_ranks::ST_KILLS), 0);
	CHECK(!api.ChangeClientValue(1, 100));
	api.ResetPlayerStats(1);
	api.RoundWithoutValue();
	api.PrintToChat(1, "%s", "hello"); // must not crash
	CHECK(api.GetDatabaseConfig().driver == "mysql");
	// Registering a listener without the plugin is a no-op, unregistering must not crash.
	api.HookOnCoreIsReady([] {});
	api.UnhookOnCoreIsReady();
	api.HookOnPlayerKilledPre([](pisex::IGameEvent*, int&, int, int) {});
	api.UnhookOnPlayerKilledPre();
}

void TestGetters() {
	std::printf("-- getters\n");
	InstallStubs();
	LrApi& api = LrApi::Get();
	CHECK(api.Available());

	g_core_loaded = true;
	CHECK(api.CoreIsLoaded());
	g_settings_value = 7;
	CHECK_EQ_INT(api.GetSettingsValue(levels_ranks::LR_MinplayersCount), 7);
	CHECK_EQ_INT(api.GetSettingsValue(levels_ranks::LR_ShowUsualMessage), -1); // the stub's sentinel
	g_stats_value = 5;
	CHECK_EQ_INT(api.GetSettingsStatsValue(levels_ranks::LR_ExpKill), 5);
	g_player_count = 42;
	CHECK_EQ_INT(api.GetCountPlayers(), 42);

	// The table name pointer stays valid across calls (the original handed out g_sTableName).
	const char* table = api.GetTableName();
	CHECK_EQ_STR(table, "lvl_base");
	CHECK_EQ_INT(api.GetClientInfo(2, levels_ranks::ST_KILLS, true), 2 * 1000 + 2 * 10 + 1);
	CHECK(api.GetClientInfo(2, levels_ranks::ST_KILLS, false) == 2 * 1000 + 2 * 10);
	CHECK(api.GetClientStatus(3));
	CHECK(!api.GetClientStatus(4));

	g_allow = true;
	CHECK(api.CheckCountPlayers());
	api.RoundWithoutValue();
	CHECK(!api.CheckCountPlayers());

	// Vectors are converted both ways.
	g_rank_names = {plg::string("rank_1"), plg::string("rank_2")};
	g_rank_exp = {0, 500};
	const std::vector<std::string> names = api.GetRankNames();
	CHECK_EQ_INT(names.size(), 2);
	CHECK_EQ_STR(names.size() > 1 ? names[1] : std::string("<missing>"), "rank_2");
	const std::vector<int> exp = api.GetRankExp();
	CHECK_EQ_INT(exp.size(), 2);
	CHECK_EQ_INT(exp.size() > 1 ? exp[1] : -1, 500);

	g_change_result = true;
	CHECK(api.ChangeClientValue(1, 50));
	CHECK_EQ_INT(g_change_calls, 1);
	g_change_result = false;
	CHECK(!api.ChangeClientValue(1, 50));
	api.ResetPlayerStats(2);
	CHECK_EQ_INT(g_reset_calls, 1);
}

void TestDatabaseConfig() {
	std::printf("-- database config\n");
	LrApi& api = LrApi::Get();
	g_db_config = {"mysql", "10.0.0.5", "3307", "user", "secret", "lrdb"};
	const levels_ranks::DatabaseConfig mysql = api.GetDatabaseConfig();
	CHECK_EQ_STR(mysql.driver, "mysql");
	CHECK_EQ_STR(mysql.host, "10.0.0.5");
	CHECK_EQ_INT(mysql.port, 3307);
	CHECK_EQ_STR(mysql.user, "user");
	CHECK_EQ_STR(mysql.password, "secret");
	CHECK_EQ_STR(mysql.database, "lrdb");
	CHECK(!mysql.IsSqlite());

	g_db_config = {"sqlite", "/tmp/lr.sqlite3", "0", "", "", "levels_ranks"};
	const levels_ranks::DatabaseConfig sqlite = api.GetDatabaseConfig();
	CHECK(sqlite.IsSqlite());
	CHECK_EQ_STR(sqlite.host, "/tmp/lr.sqlite3");
	CHECK_EQ_INT(sqlite.port, 3306); // the invalid "0" falls back to the default

	const plgdb::Config plgdb_sqlite = levels_ranks::MakeDatabaseConfig(sqlite, "/tmp/fallback.sqlite3");
	CHECK(plgdb_sqlite.driver == plgdb::Driver::SQLite);
	CHECK_EQ_STR(plgdb_sqlite.sqlitePath, "/tmp/lr.sqlite3");
	const plgdb::Config plgdb_mysql = levels_ranks::MakeDatabaseConfig(mysql, "/tmp/fallback.sqlite3");
	CHECK(plgdb_mysql.driver == plgdb::Driver::MySQL);
	CHECK_EQ_STR(plgdb_mysql.host, "10.0.0.5");
	CHECK_EQ_INT(plgdb_mysql.port, 3307);
}

void TestChat() {
	std::printf("-- chat\n");
	LrApi& api = LrApi::Get();
	g_chat.clear();
	api.PrintToChat(4, "%s (%i)", "Bob", 12);
	CHECK_EQ_INT(g_chat.size(), 1);
	CHECK_EQ_STR(g_chat.empty() ? std::string("<empty>") : g_chat.front(), "4:Bob (12)");
	// A long message is truncated like the original's 512 byte ClientPrint buffer.
	const std::string long_text(400, 'x');
	api.PrintToChat(-1, "%s%s", long_text.c_str(), long_text.c_str());
	CHECK_EQ_INT(g_chat.size(), 2);
	const std::string last = g_chat.empty() ? std::string("<empty>") : g_chat.back();
	CHECK(last.size() < long_text.size() * 2 + 5);
	CHECK_EQ_STR(last.substr(0, 5), "-1:xx");
}

void TestTrampolines() {
	std::printf("-- trampolines\n");
	LrApi& api = LrApi::Get();

	// OnCoreIsReady: the captures live in the wrapper, the plugin only holds the pointer.
	int fired = 0;
	api.HookOnCoreIsReady([&fired] { ++fired; });
	CHECK(g_core_ready_callback != nullptr);
	g_core_ready_callback();
	g_core_ready_callback();
	CHECK_EQ_INT(fired, 2);
	// A throwing callback must not escape into the plugin.
	api.HookOnCoreIsReady([] { throw std::runtime_error("boom"); });
	g_core_ready_callback(); // must not propagate
	CHECK_EQ_INT(fired, 2);
	api.UnhookOnCoreIsReady();
	CHECK_EQ_INT(g_unregister_calls, 1);

	// OnLevelChangedPre: the by-reference new level is written back.
	api.HookOnLevelChangedPre([](int slot, int& new_level, int old_level) {
		CHECK_EQ_INT(slot, 5);
		CHECK_EQ_INT(old_level, 2);
		new_level = 9;
	});
	int32_t level = 3;
	g_level_pre_callback(5, level, 2);
	CHECK_EQ_INT(level, 9);

	// OnPlayerKilledPre: the event handle is rebuilt as a pisex::IGameEvent* and the exp is
	// written back.
	int64_t seen_event = -1;
	api.HookOnPlayerKilledPre([&seen_event](pisex::IGameEvent* event, int& exp_caused, int exp_victim, int exp_attacker) {
		seen_event = event != nullptr ? static_cast<int64_t>(reinterpret_cast<intptr_t>(event->Native())) : -1;
		CHECK_EQ_INT(exp_victim, 100);
		CHECK_EQ_INT(exp_attacker, 200);
		exp_caused *= 3;
	});
	int32_t caused = 10;
	g_killed_pre_callback(0x1234, caused, 100, 200);
	CHECK_EQ_INT(caused, 30);
	CHECK_EQ_INT(seen_event, 0x1234);

	// OnExpChangedPre.
	api.HookOnExpChangedPre([](int slot, int& give_exp) {
		CHECK_EQ_INT(slot, 1);
		give_exp = 0;
	});
	int32_t give = 25;
	g_exp_pre_callback(1, give);
	CHECK_EQ_INT(give, 0);

	// OnPlayerLoaded: the steam id is handed over as a const char*.
	std::string loaded_id;
	int loaded_slot = -1;
	api.HookOnPlayerLoaded([&](int slot, const char* steamid) {
		loaded_slot = slot;
		loaded_id = steamid;
	});
	g_player_loaded_callback(7, plg::string("STEAM_1:0:123"));
	CHECK_EQ_INT(loaded_slot, 7);
	CHECK_EQ_STR(loaded_id, "STEAM_1:0:123");
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("levels_ranks api tests\n");

	TestUnavailable();
	TestGetters();
	TestDatabaseConfig();
	TestChat();
	TestTrampolines();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
