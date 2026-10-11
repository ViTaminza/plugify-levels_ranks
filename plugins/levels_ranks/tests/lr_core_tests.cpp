// Tests for plugins/levels_ranks/src/lr_core.hpp (engine-independent core of the levels_ranks
// plugin: the settings model of settings.ini, the rank/experience/statistics logic and the SQL of
// the original cs2-lvl_ranks).
//
// Runs against SQLite ":memory:" always, and against MySQL when MYSQL_HOST is set (test.sh
// provides MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE).
//
// Every database run uses fresh STEAM_ ids and deletes its rows at the end, so repeated runs
// against the shared MySQL database do not interfere. The fixture drives the core exactly like
// the plugin does: statements are queued from the game thread and their callbacks run from Poll().

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

#include <plgdb/db.hpp>

#include <plg/plugin.hpp>
#include <pisex/keyvalues.hpp>

#include "lr_core.hpp"
#include "lr_keyvalues.hpp"

// The plg globals a Plugify plugin defines through PLUGIFY_PLUGIN; pisexcompat (the KV1 parser
// used here) references a few of them, so the test provides them (null = the fallbacks apply).
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

#ifndef LR_TEST_DATA_DIR
#define LR_TEST_DATA_DIR "."
#endif

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),             \
						g_section.data(), __FILE__, __LINE__, #expr);                              \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const long long a_ = static_cast<long long>(actual);                                       \
		const long long e_ = static_cast<long long>(expected);                                     \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                            \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,           \
						__LINE__, a_, e_);                                                         \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const std::string a_{actual};                                                              \
		const std::string e_{expected};                                                            \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,           \
						__LINE__, a_.c_str(), e_.c_str());                                         \
		}                                                                                          \
	} while (false)

#define CHECK_QUERY_OK(result)                                                                     \
	do {                                                                                           \
		++g_checks;                                                                                \
		const plgdb::Result& r_ = (result);                                                        \
		if (!r_.Ok()) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: query failed: %s\n",                                   \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,           \
						__LINE__, r_.Error().c_str());                                             \
		}                                                                                          \
	} while (false)

using levels_ranks::LrCore;
using levels_ranks::PlayerInfo;
using levels_ranks::Settings;

// A failed CHECK must not turn the next line into an out-of-range crash.
std::string Front(const std::vector<std::string>& values) {
	return values.empty() ? std::string("<empty>") : values.front();
}

std::string Back(const std::vector<std::string>& values) {
	return values.empty() ? std::string("<empty>") : values.back();
}

// --- helpers ---------------------------------------------------------------------------

bool Wait(plgdb::Database& db, const std::function<bool()>& done, int timeoutMs = 10000) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;) {
		db.Poll();
		if (done()) {
			return true;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			db.Poll();
			return done();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}

// Lets every queued statement run.
void Settle(plgdb::Database& db, int rounds = 3) {
	for (int i = 0; i < rounds; ++i) {
		Wait(db, [] { return false; }, 30);
	}
}

plgdb::Config SqliteConfig() {
	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = ":memory:";
	return config;
}

bool MySqlConfig(plgdb::Config& config, std::string& error) {
	const char* host = std::getenv("MYSQL_HOST");
	if (!host || !*host) {
		return false;
	}
	config.driver = plgdb::Driver::MySQL;
	config.host = host;
	config.user = std::getenv("MYSQL_USER") ? std::getenv("MYSQL_USER") : "";
	config.password = std::getenv("MYSQL_PASSWORD") ? std::getenv("MYSQL_PASSWORD") : "";
	config.database = std::getenv("MYSQL_DATABASE") ? std::getenv("MYSQL_DATABASE") : "";
	const char* port = std::getenv("MYSQL_PORT");
	if (port && *port) {
		try {
			const int value = std::stoi(port);
			if (value < 1 || value > 65535) {
				error = "MYSQL_PORT out of range";
				return false;
			}
			config.port = static_cast<uint16_t>(value);
		} catch (const std::exception&) {
			error = "MYSQL_PORT is not a number";
			return false;
		}
	}
	return true;
}

// A literal KV1 tree, so the settings parser can be tested without a file.
struct FakeView final : levels_ranks::ConfigView {
	std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> sections;
	std::vector<std::pair<std::string, int>> ranks;

	bool HasSection(std::string_view section) const override {
		return section.empty() || sections.count(std::string(section)) != 0;
	}
	std::string GetString(std::string_view section, std::string_view key, std::string_view def) const override {
		const auto block = sections.find(std::string(section));
		if (block == sections.end()) {
			return std::string(def);
		}
		const auto entry = block->second.find(std::string(key));
		return entry == block->second.end() ? std::string(def) : entry->second;
	}
	float GetFloat(std::string_view section, std::string_view key, float def) const override {
		const std::string text = GetString(section, key, "");
		return text.empty() ? def : std::strtof(text.c_str(), nullptr);
	}
	int GetInt(std::string_view section, std::string_view key, int def) const override {
		const std::string text = GetString(section, key, "");
		return text.empty() ? def : std::atoi(text.c_str());
	}
	std::vector<std::pair<std::string, int>> GetIntValues(std::string_view section) const override {
		(void)section;
		return ranks;
	}
};

// --- settings ---------------------------------------------------------------------------

void TestSettingsDefaults() {
	Section("settings defaults");
	FakeView view;
	std::string problem;
	const Settings settings = levels_ranks::LoadSettings(view, &problem);

	CHECK_EQ_STR(settings.table_name, "lvl_base");
	CHECK_EQ_STR(settings.plugin_title, "");
	CHECK_EQ_INT(settings.type_statistics, 0);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_MinplayersCount), 4);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_ShowUsualMessage), 1);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_ShowSpawnMessage), 1);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_GiveExpRoundEnd), 1);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_BlockWarmup), 1);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_CleanDB_Days), 30);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_OnlineID), 1);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_DB_SaveDataPlayer_Mode), 1);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_DB_Allow_UTF8MB4), 1);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_StartPoints), 0);
	// The cooldown is read off the root (where the key of the shipped file is not)
	// -> the default is what the original used.
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_ResetMyStatsCooldown), 86400);
	// The stats block of the "Funded" mode has no defaults for the four kill values in the
	// original (GetInt without a default) but they were never applied without a section.
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpGiveHeadShot), 1);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpGiveAssist), 1);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpRoundWin), 2);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpRoundLose), 2);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpRoundMVP), 1);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpHostageRescued), 2);
	CHECK(settings.rank_names.empty());
	CHECK(settings.HasValidTableName());
	// Missing blocks are reported (the original unloaded the plugin then).
	CHECK(!problem.empty());
}

void TestSettingsAliasing() {
	Section("settings aliasing (slots 6/7)");
	FakeView view;
	view.sections["MainSettings"]["lr_show_all_levelup_message"] = "0";
	view.sections["MainSettings"]["lr_show_all_leveldown_message"] = "0";
	view.sections["MainSettings"]["lr_show_levelup_message"] = "1";
	view.sections["MainSettings"]["lr_show_leveldown_message"] = "1";
	const Settings settings = levels_ranks::LoadSettings(view, nullptr);

	// PARITY: the core's macro block stores lr_show_levelup_message in the slot the interface
	// calls LR_ShowAllLevelUpMessage (6) - the later read wins.
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_ShowAllLevelUpMessage), 1);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_ShowAllLevelDownMessage), 1);
	// ... and the interface's own LR_ShowLevelUpMessage (22) is never written.
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_ShowLevelUpMessage), 0);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_ShowLevelDownMessage), 0);
}

void TestSettingsCoefficientClamp() {
	Section("lr_killcoeff clamp");
	FakeView low;
	low.sections["MainSettings"]["lr_type_statistics"] = "1";
	low.sections["Rating_Extended"]["lr_killcoeff"] = "0.10";
	CHECK_EQ_INT(levels_ranks::LoadSettings(low, nullptr).Stats(levels_ranks::LR_ExpKillCoefficient), 50);

	FakeView high;
	high.sections["MainSettings"]["lr_type_statistics"] = "1";
	high.sections["Rating_Extended"]["lr_killcoeff"] = "1.90";
	CHECK_EQ_INT(levels_ranks::LoadSettings(high, nullptr).Stats(levels_ranks::LR_ExpKillCoefficient), 150);

	FakeView inside;
	inside.sections["MainSettings"]["lr_type_statistics"] = "1";
	inside.sections["Rating_Extended"]["lr_killcoeff"] = "1.04";
	CHECK_EQ_INT(levels_ranks::LoadSettings(inside, nullptr).Stats(levels_ranks::LR_ExpKillCoefficient), 104);
}

void TestShippedSettingsFile() {
	Section("the shipped settings.ini");
	const std::string path = std::string(LR_TEST_DATA_DIR) + "/settings.ini";
	pisex::KeyValues kv("LR_Settings");
	if (!kv.LoadFromFile(path)) {
		std::printf("FATAL: cannot read %s\n", path.c_str());
		++g_failures;
		return;
	}
	std::string problem;
	levels_ranks::KeyValuesView view(kv);
	const Settings settings = levels_ranks::LoadSettings(view, &problem);
	CHECK(problem.empty());

	CHECK_EQ_STR(settings.table_name, "lvl_base");
	CHECK_EQ_STR(settings.plugin_title, "Levels Ranks");
	CHECK_EQ_INT(settings.type_statistics, 0);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_MinplayersCount), 4);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_TopCount), 5);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_CleanDB_Days), 30);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_ShowResetMyStats), 1);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_ShowRankList), 1);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_ShowRankMessage), 1);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_ShowAllLevelUpMessage), 1);
	CHECK_EQ_INT(settings.Value(levels_ranks::LR_DB_Charset_Type), 0);
	// Funded_System
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpKill), 5);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpKillIsBot), 0);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpDeath), 4);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpDeathIsBot), 0);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpGiveSuicide), 5);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpGiveTeamKill), 20);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpRoundWin), 3);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpBombDefused), 4);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpHostageRescued), 3);
	CHECK_EQ_INT(settings.Stats(levels_ranks::LR_ExpKillCoefficient), 0); // not read in this mode
	// Special_Bonuses: lr_bonus_1..10 (the eleventh of the file is never read)
	CHECK_EQ_INT(settings.bonus[0], 2);
	CHECK_EQ_INT(settings.bonus[9], 11);
	CHECK_EQ_INT(settings.bonus.size(), 10);
	// Ranks
	CHECK_EQ_INT(settings.rank_exp.size(), 18);
	CHECK_EQ_STR(Front(settings.rank_names), "rank_1");
	CHECK_EQ_INT(settings.rank_exp.empty() ? -1 : settings.rank_exp.front(), 0);
	CHECK_EQ_STR(Back(settings.rank_names), "rank_18");
	CHECK_EQ_INT(settings.rank_exp.empty() ? -1 : settings.rank_exp.back(), 100000);
	CHECK_EQ_INT(settings.rank_exp[1], 500);
}

void TestPermissionsByTableName() {
	Section("table name validation");
	Settings settings;
	settings.table_name = "lvl_base";
	CHECK(settings.HasValidTableName());
	settings.table_name = "bad name";
	CHECK(!settings.HasValidTableName());
	settings.table_name = std::string(33, 'a');
	CHECK(!settings.HasValidTableName());
	settings.table_name = "";
	CHECK(!settings.HasValidTableName());
	settings.table_name = "LR_Base1";
	CHECK(settings.HasValidTableName());

	plgdb::Config config = SqliteConfig();
	auto db = plgdb::Database::Create(config);
	LrCore core(*db);
	Settings custom;
	custom.table_name = "bad-name";
	core.SetSettings(custom);
	CHECK_EQ_STR(core.GetSettings().table_name, "lvl_base"); // rejected, default kept
	CHECK_EQ_STR(core.Table(), "`lvl_base`");
}

// --- pure logic -------------------------------------------------------------------------

void TestRankMath() {
	Section("rank math");
	const std::vector<int> ranks = {0, 500, 1250, 2500, 4250, 6000};
	CHECK_EQ_INT(LrCore::RankForExp(0, ranks), 1);
	CHECK_EQ_INT(LrCore::RankForExp(499, ranks), 1);
	CHECK_EQ_INT(LrCore::RankForExp(500, ranks), 2);
	CHECK_EQ_INT(LrCore::RankForExp(1249, ranks), 2);
	CHECK_EQ_INT(LrCore::RankForExp(1250, ranks), 3);
	CHECK_EQ_INT(LrCore::RankForExp(6000, ranks), 6);
	CHECK_EQ_INT(LrCore::RankForExp(1000000, ranks), 6);
	// PARITY: an experience below the first threshold walks the loop down to rank 0.
	CHECK_EQ_INT(LrCore::RankForExp(-1, ranks), 0);
	CHECK_EQ_INT(LrCore::RankForExp(0, {}), 0);
}

void TestKillMath() {
	Section("kill experience");
	// Rating_Extended: RoundToNearest(victim / attacker * 5).
	CHECK_EQ_INT(LrCore::KillExpAttacker(1, 1000, 1500), 8);   // 1.5 * 5 = 7.5 -> 8
	CHECK_EQ_INT(LrCore::KillExpAttacker(1, 1000, 1000), 5);
	CHECK_EQ_INT(LrCore::KillExpAttacker(1, 400, 100000), 1250);
	// PARITY: the original divided by zero here; the port treats it as a ratio of 1.
	CHECK_EQ_INT(LrCore::KillExpAttacker(1, 0, 1000), 5);
	// Rating_Simple: the raw difference, mapped by SimpleModeExp.
	CHECK_EQ_INT(LrCore::KillExpAttacker(2, 1000, 1500), 500);
	CHECK_EQ_INT(LrCore::SimpleModeExp(500), 7);
	CHECK_EQ_INT(LrCore::SimpleModeExp(2), 2);
	CHECK_EQ_INT(LrCore::SimpleModeExp(3), 2);
	CHECK_EQ_INT(LrCore::SimpleModeExp(101), 3);
	CHECK_EQ_INT(LrCore::SimpleModeExp(-50), 2);
	// The Extended mode's victim loss.
	CHECK_EQ_INT(LrCore::RatingVictimExp(8, 100), 8);
	CHECK_EQ_INT(LrCore::RatingVictimExp(8, 104), 8); // 8.32 -> 8
	CHECK_EQ_INT(LrCore::RatingVictimExp(10, 150), 15);
	CHECK_EQ_INT(LrCore::RatingVictimExp(1, 50), 1);  // 0.5 rounds to 1, the minimum holds
	CHECK_EQ_INT(LrCore::RatingVictimExp(1, 1), 1);
	// The streak bonus index: min(streak - 2, 9).
	CHECK_EQ_INT(LrCore::StreakBonusIndex(2), 0);
	CHECK_EQ_INT(LrCore::StreakBonusIndex(3), 1);
	CHECK_EQ_INT(LrCore::StreakBonusIndex(11), 9);
	CHECK_EQ_INT(LrCore::StreakBonusIndex(50), 9);
	// The rounding helpers.
	CHECK_EQ_INT(LrCore::RoundToNearest(7.5), 8);
	CHECK_EQ_INT(LrCore::RoundToCeil(100.0 / 3 * 2), 67);
}

// --- the database ------------------------------------------------------------------------

// The settings of the shipped settings.ini (through the parser), plus the 18 ranks, so a test
// that does not care about the configuration still runs with the values of a real server.
Settings BaseSettings() {
	Settings settings = levels_ranks::LoadSettings(FakeView{}, nullptr);
	static const int kRanks[] = {0,     500,   1250,  2500,  4250,  6000,   8500,   11500, 14900,
								 19000, 23000, 27000, 31500, 36000, 41000, 50000, 70000, 100000};
	for (std::size_t i = 0; i < std::size(kRanks); ++i) {
		settings.rank_names.push_back("rank_" + std::to_string(i + 1));
		settings.rank_exp.push_back(kRanks[i]);
	}
	return settings;
}

levels_ranks::TopRow RowAt(const std::vector<levels_ranks::TopRow>& rows, std::size_t index) {
	return index < rows.size() ? rows[index] : levels_ranks::TopRow{};
}

struct Fixture {
	explicit Fixture(uint64_t base) : run_base(base) {}

	std::unique_ptr<plgdb::Database> db;
	std::unique_ptr<LrCore> core;
	std::vector<std::string> errors;
	std::vector<std::string> loaded;      // "<slot>:<steamid>" of onPlayerLoaded
	std::vector<std::string> resets;      // "<slot>:<steamid>" of onResetPlayerStats
	std::vector<std::string> positions;   // "<slot>:<exp>:<time>"
	std::vector<std::string> exp_pre;     // "<slot>:<value>"
	std::vector<std::string> exp_post;    // "<slot>:<value>:<newExp>"
	std::vector<std::string> level_messages;
	std::vector<std::string> exp_messages;
	std::vector<int> cleanup;
	int core_ready = 0;
	int64_t now = 1'700'000'000;
	uint64_t run_base;
	int counter = 0;
	std::vector<std::string> tracked;

	std::string NewSteamId() {
		const std::string value = "STEAM_1:1:" + std::to_string(run_base + static_cast<uint64_t>(++counter));
		tracked.push_back(value);
		return value;
	}

	bool Init(const plgdb::Config& config, Settings settings = BaseSettings()) {
		db = plgdb::Database::Create(config);
		if (!db) {
			std::printf("FATAL: Database::Create returned null\n");
			return false;
		}
		settings.table_name = "lrt_" + std::to_string(run_base % 1000000);
		core = std::make_unique<LrCore>(*db);
		core->SetSettings(settings);
		core->SetClock([this] { return now; });
		core->findPlayerName = [](int slot) { return "Player" + std::to_string(slot); };
		core->onError = [this](std::string_view message) { errors.emplace_back(message); };
		core->onCoreReady = [this] { ++core_ready; };
		core->onPlayerLoaded = [this](int slot, const std::string& steamid) {
			loaded.push_back(std::to_string(slot) + ":" + steamid);
		};
		core->onResetPlayerStats = [this](int slot, const std::string& steamid) {
			resets.push_back(std::to_string(slot) + ":" + steamid);
		};
		core->onPlayerPosInTop = [this](int slot, int exp_pos, int time_pos) {
			positions.push_back(std::to_string(slot) + ":" + std::to_string(exp_pos) + ":" + std::to_string(time_pos));
		};
		core->onExpChangedPre = [this](int slot, int& value) {
			exp_pre.push_back(std::to_string(slot) + ":" + std::to_string(value));
			value = pre_rewrite ? pre_rewrite(slot, value) : value;
		};
		core->onExpChangedPost = [this](int slot, int value, int new_exp) {
			exp_post.push_back(std::to_string(slot) + ":" + std::to_string(value) + ":" + std::to_string(new_exp));
		};
		core->onLevelMessage = [this](int target, int speaker, int rank, bool up, bool personal) {
			level_messages.push_back(std::to_string(target) + ":" + std::to_string(speaker) + ":" +
									 std::to_string(rank) + ":" + (up ? "up" : "down") + ":" +
									 (personal ? "own" : "all"));
		};
		core->onExpMessage = [this](int slot, const std::string& phrase, int new_exp, int requested) {
			exp_messages.push_back(std::to_string(slot) + ":" + phrase + ":" + std::to_string(new_exp) + ":" +
								   std::to_string(requested));
		};
		core->onDatabaseCleanup = [this](int type) { cleanup.push_back(type); };
		core->CreateSchema();
		return true;
	}

	std::function<int(int, int)> pre_rewrite;

	~Fixture() { Destroy(); }

	void Destroy() {
		if (db == nullptr) {
			return;
		}
		core.reset();
		if (!tracked.empty()) {
			std::string list;
			for (const std::string& steamid : tracked) {
				if (!list.empty()) {
					list += ",";
				}
				list += "'" + steamid + "'";
			}
			(void)db->QuerySync("DELETE FROM `lrt_" + std::to_string(run_base % 1000000) + "` WHERE `steam` IN (" + list + ");",
								{});
		}
		db.reset();
	}

	plgdb::Result Query(const std::string& sql) const {
		return db->QuerySync(sql, {});
	}

	plgdb::Result Row(const std::string& steamid) const {
		return Query("SELECT `value`, `rank`, `kills`, `deaths`, `headshots`, `assists`, `round_win`, `playtime`, "
					 "`lastconnect`, `online` FROM `lrt_" +
					 std::to_string(run_base % 1000000) + "` WHERE `steam` = '" + steamid + "';");
	}
};

void RunDatabaseSuite(const plgdb::Config& config, const char* label) {
	std::printf("== %s\n", label);
	const uint64_t base = [] {
		std::mt19937_64 rng(static_cast<uint64_t>(::getpid()) ^ static_cast<uint64_t>(std::time(nullptr)));
		return 1'000'000ULL + rng() % 1'000'000'000ULL;
	}();

	// --- load a fresh player, then save it ------------------------------------------------
	{
		Section("fresh player and save");
		Fixture fx(base);
		if (!fx.Init(config)) {
			return;
		}
		CHECK_EQ_INT(fx.core_ready, 0);
		fx.core->ConfigureDatabase();
		CHECK(Wait(*fx.db, [&] { return fx.core_ready == 1; }));
		CHECK_EQ_INT(fx.core->PlayerCount(), 0);

		const std::string steamid = fx.NewSteamId();
		fx.core->LoadOrCreatePlayer(3, steamid, "Fresh");
		CHECK(Wait(*fx.db, [&] { return !fx.loaded.empty(); }));
		CHECK_EQ_STR(Front(fx.loaded), "3:" + steamid);
		CHECK_EQ_INT(fx.core->PlayerCount(), 1);
		const PlayerInfo* info = fx.core->FindPlayer(3);
		CHECK(info != nullptr && info->initialized);
		CHECK_EQ_INT(info->stats[levels_ranks::ST_EXP], 0); // lr_start_points
		CHECK_EQ_INT(info->stats[levels_ranks::ST_RANK], 1); // rank_1 (threshold 0)
		CHECK_EQ_INT(info->stats[levels_ranks::ST_PLAYTIME], static_cast<int>(fx.now));

		// The row was inserted with the start values.
		const plgdb::Result row = fx.Row(steamid);
		CHECK_QUERY_OK(row);
		CHECK_EQ_INT(row.RowCount(), 1);
		CHECK_EQ_INT(row.GetInt(0, 0), 0);

		// 60 seconds of play, some counters, then a save.
		fx.now += 60;
		PlayerInfo* mutable_info = fx.core->FindPlayer(3);
		mutable_info->stats[levels_ranks::ST_KILLS] = 4;
		mutable_info->stats[levels_ranks::ST_EXP] = 1234;
		mutable_info->stats[levels_ranks::ST_RANK] = 2;
		fx.core->SavePlayer(3);
		CHECK(Wait(*fx.db, [&] { return !fx.positions.empty(); }));
		const plgdb::Result saved = fx.Row(steamid);
		CHECK_QUERY_OK(saved);
		CHECK_EQ_INT(saved.GetInt(0, 0), 1234); // value
		CHECK_EQ_INT(saved.GetInt(0, 1), 2);    // rank
		CHECK_EQ_INT(saved.GetInt(0, 2), 4);    // kills
		CHECK_EQ_INT(saved.GetInt(0, 7), 60);   // playtime (accumulated)
		CHECK_EQ_INT(saved.GetInt(0, 8), fx.now);
		CHECK_EQ_INT(saved.GetInt(0, 9), 1);    // online = lr_online_id
		CHECK_EQ_STR(Front(fx.positions), "3:1:1");
		CHECK_EQ_INT(fx.core->FindPlayer(3)->stats[levels_ranks::ST_PLACEINTOP], 1);
		CHECK_EQ_STR(fx.core->FindPlayer(3)->steamid, steamid);
	}

	// --- a second connect reads the saved row ---------------------------------------------
	{
		Section("reconnect");
		Fixture fx(base + 1);
		if (!fx.Init(config)) {
			return;
		}
		const std::string steamid = fx.NewSteamId();
		fx.core->LoadOrCreatePlayer(5, steamid, "A");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 1; }));
		fx.now += 30;
		fx.core->FindPlayer(5)->stats[levels_ranks::ST_EXP] = 900;
		fx.core->FindPlayer(5)->stats[levels_ranks::ST_KILLS] = 7;
		fx.core->SavePlayer(5);
		Settle(*fx.db);

		// A disconnect clears the slot and the online flag.
		fx.core->SavePlayer(5, true);
		Settle(*fx.db);
		CHECK(!fx.core->FindPlayer(5)->initialized);
		CHECK_EQ_INT(fx.Row(steamid).GetInt(0, 9), 0);

		// The newcomer of the slot loads the stored values again.
		fx.core->LoadOrCreatePlayer(5, steamid, "A");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 2; }));
		const PlayerInfo* info = fx.core->FindPlayer(5);
		CHECK_EQ_INT(info->stats[levels_ranks::ST_EXP], 900);
		CHECK_EQ_INT(info->stats[levels_ranks::ST_KILLS], 7);
		// The accumulated play time is preserved across the reconnect.
		CHECK_EQ_INT(fx.Row(steamid).GetInt(0, 7), 30);
	}

	// --- GiveExp, the statistic gate and the message hooks ---------------------------------
	{
		Section("give exp");
		Fixture fx(base + 2);
		if (!fx.Init(config)) {
			return;
		}
		const std::string steamid = fx.NewSteamId();
		fx.core->LoadOrCreatePlayer(7, steamid, "B");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 1; }));

		// The gate is closed: a value is refused (status false).
		CHECK_EQ_INT(fx.core->GiveExp(7, 10, "Kill").status, 0);
		fx.core->SetAllowStatistic(true);
		const levels_ranks::ExpResult result = fx.core->GiveExp(7, 10, "Kill");
		CHECK_EQ_INT(result.status, 1);
		CHECK_EQ_INT(result.applied_value, 10);
		CHECK_EQ_INT(result.new_exp, 10);
		CHECK_EQ_INT(fx.core->FindPlayer(7)->session[levels_ranks::ST_EXP], 10);
		// The "usual message" fired with the phrase and the requested value.
		CHECK_EQ_INT(fx.exp_messages.size(), 1);
		CHECK_EQ_STR(Front(fx.exp_messages), "7:Kill:10:10");

		// The pre hook may rewrite the value.
		fx.pre_rewrite = [](int, int value) { return value / 2; };
		fx.core->GiveExp(7, 10, "Kill");
		CHECK_EQ_INT(fx.core->FindPlayer(7)->stats[levels_ranks::ST_EXP], 15);
		CHECK_EQ_STR(Back(fx.exp_pre), "7:10");
		CHECK_EQ_STR(Back(fx.exp_post), "7:5:15");
		fx.pre_rewrite = nullptr;

		// A value rewritten to 0 skips the change (but still reports the status).
		fx.pre_rewrite = [](int, int) { return 0; };
		const levels_ranks::ExpResult zero = fx.core->GiveExp(7, 10, "Kill");
		CHECK_EQ_INT(zero.status, 1);
		CHECK_EQ_INT(zero.applied, 0);
		CHECK_EQ_INT(fx.core->FindPlayer(7)->stats[levels_ranks::ST_EXP], 15);
		fx.pre_rewrite = nullptr;

		// lr_show_usual_message = 0 silences the message.
		Settings quiet = fx.core->GetSettings();
		quiet.values[levels_ranks::LR_ShowUsualMessage] = 0;
		fx.core->SetSettings(quiet);
		const std::size_t before = fx.exp_messages.size();
		fx.core->GiveExp(7, 10, "Kill");
		CHECK_EQ_INT(fx.exp_messages.size(), before);
	}

	// --- the rating clamp of ChangeClientValue ---------------------------------------------
	{
		Section("change client value clamp");
		Fixture fx(base + 3);
		Settings rating = BaseSettings();
		rating.type_statistics = 1;
		rating.stats[levels_ranks::LR_ExpKillCoefficient] = 104;
		if (!fx.Init(config, rating)) {
			return;
		}
		const std::string steamid = fx.NewSteamId();
		fx.core->LoadOrCreatePlayer(9, steamid, "C");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 1; }));
		CHECK_EQ_INT(fx.core->FindPlayer(9)->stats[levels_ranks::ST_EXP], 1000);

		// -5000 clamps at the 400 floor; the round/session counters get the clamped delta
		// (the PARITY quirk of ChangeClientValue).
		CHECK(fx.core->ChangeClientValue(9, -5000));
		CHECK_EQ_INT(fx.core->FindPlayer(9)->stats[levels_ranks::ST_EXP], 400);
		CHECK_EQ_INT(fx.core->FindPlayer(9)->round_exp, -600);
		CHECK_EQ_INT(fx.core->FindPlayer(9)->session[levels_ranks::ST_EXP], -600);
		// The unloaded slot is refused.
		CHECK(!fx.core->ChangeClientValue(11, 100));
	}

	// --- rank changes, the pre hook and the level messages -----------------------------------
	{
		Section("rank changes");
		Fixture fx(base + 4);
		Settings settings = BaseSettings();
		settings.rank_names = {"rank_1", "rank_2", "rank_3"};
		settings.rank_exp = {0, 100, 300};
		settings.values[levels_ranks::LR_ShowUsualMessage] = 0; // keep the exp messages out of the way
		if (!fx.Init(config, settings)) {
			return;
		}
		const std::string steamid = fx.NewSteamId();
		fx.core->LoadOrCreatePlayer(1, steamid, "D");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 1; }));
		CHECK_EQ_INT(fx.core->FindPlayer(1)->stats[levels_ranks::ST_RANK], 1);
		fx.core->SetAllowStatistic(true);

		// The pre hook may replace the new rank: the vetoed promotion keeps rank 1 and the
		// session delta of the rank is 0.
		int hook_calls = 0;
		fx.core->onLevelChangedPre = [&hook_calls](int, int& new_rank, int old_rank) {
			++hook_calls;
			if (new_rank > old_rank) {
				new_rank = old_rank;
			}
		};
		CHECK_EQ_INT(fx.core->GiveExp(1, 150, "Kill").status, 1);
		CHECK_EQ_INT(hook_calls, 1);
		CHECK_EQ_INT(fx.core->FindPlayer(1)->stats[levels_ranks::ST_RANK], 1);
		CHECK_EQ_INT(fx.core->FindPlayer(1)->session[levels_ranks::ST_RANK], 0);
		// The level messages are driven by the slots 6/7 (lr_show_levelup_message's defaults: 0),
		// so a change without them prints nothing.
		CHECK(fx.level_messages.empty());
		fx.core->onLevelChangedPre = nullptr;

		// With the personal message switched on (slot 6 = what lr_show_levelup_message fills) the
		// same experience promotes: 300 -> rank 3, one personal message.
		Settings talking = fx.core->GetSettings();
		talking.values[levels_ranks::LR_ShowAllLevelUpMessage] = 1;
		fx.core->SetSettings(talking);
		CHECK_EQ_INT(fx.core->GiveExp(1, 150, "Kill").status, 1);
		CHECK_EQ_INT(fx.core->FindPlayer(1)->stats[levels_ranks::ST_RANK], 3);
		CHECK_EQ_INT(fx.core->FindPlayer(1)->session[levels_ranks::ST_RANK], 2);
		CHECK_EQ_INT(fx.level_messages.size(), 1);
		CHECK_EQ_STR(Front(fx.level_messages), "1:1:3:up:own");
		// lr_db_savedataplayer_mode saved the row - the stored rank follows.
		Settle(*fx.db);
		CHECK_EQ_INT(fx.Row(steamid).GetInt(0, 1), 3);

		// The demotion: the personal message is driven by slot 7 (lr_show_leveldown_message, 0 so
		// far - nothing is printed), the broadcast by the same slot.
		fx.core->LoadOrCreatePlayer(2, fx.NewSteamId(), "E");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 2; }));
		fx.level_messages.clear();
		fx.core->FindPlayer(1)->stats[levels_ranks::ST_EXP] = 0;
		const levels_ranks::RankChange demoted = fx.core->CheckRank(1);
		CHECK_EQ_INT(demoted.changed, 1);
		CHECK_EQ_INT(demoted.new_rank, 1);
		CHECK_EQ_INT(demoted.old_rank, 3);
		CHECK_EQ_INT(demoted.up, 0);
		CHECK(fx.level_messages.empty());

		// With slot 7 switched on the demotion prints to the player and to every other loaded
		// player (the original read the broadcast setting next to the personal one).
		talking = fx.core->GetSettings();
		talking.values[levels_ranks::LR_ShowAllLevelDownMessage] = 1;
		fx.core->SetSettings(talking);
		fx.level_messages.clear();
		fx.core->FindPlayer(1)->stats[levels_ranks::ST_EXP] = 400; // -> rank 3, up
		fx.core->CheckRank(1);
		fx.level_messages.clear();
		fx.core->FindPlayer(1)->stats[levels_ranks::ST_EXP] = 0; // -> rank 1, down
		fx.core->CheckRank(1);
		CHECK_EQ_INT(fx.level_messages.size(), 2);
		CHECK_EQ_STR(Front(fx.level_messages), "1:1:1:down:own");
		CHECK_EQ_STR(fx.level_messages.size() > 1 ? fx.level_messages[1] : std::string("<empty>"),
					 "2:1:1:down:all");
	}

	// --- reset ----------------------------------------------------------------------------
	{
		Section("reset player stats");
		Fixture fx(base + 5);
		if (!fx.Init(config)) {
			return;
		}
		const std::string steamid = fx.NewSteamId();
		fx.core->LoadOrCreatePlayer(4, steamid, "F");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 1; }));
		fx.core->FindPlayer(4)->stats[levels_ranks::ST_KILLS] = 12;
		fx.core->FindPlayer(4)->stats[levels_ranks::ST_EXP] = 800;
		fx.core->FindPlayer(4)->session[levels_ranks::ST_KILLS] = 12;
		fx.now += 45;
		fx.core->ResetPlayerStats(4);
		CHECK_EQ_INT(fx.resets.size(), 1);
		CHECK_EQ_STR(Front(fx.resets), "4:" + steamid);
		const PlayerInfo* info = fx.core->FindPlayer(4);
		CHECK_EQ_INT(info->stats[levels_ranks::ST_KILLS], 0);
		CHECK_EQ_INT(info->stats[levels_ranks::ST_EXP], 0);
		CHECK_EQ_INT(info->session[levels_ranks::ST_KILLS], 0);
		CHECK_EQ_INT(info->stats[levels_ranks::ST_PLAYTIME], static_cast<int>(fx.now)); // playtime restarts
		CHECK_EQ_INT(info->stats[levels_ranks::ST_RANK], 1);
		CHECK_EQ_STR(info->steamid, steamid);
	}

	// --- the place in the top ---------------------------------------------------------------
	{
		Section("place in top");
		Fixture fx(base + 6);
		if (!fx.Init(config)) {
			return;
		}
		const std::string first = fx.NewSteamId();
		const std::string second = fx.NewSteamId();
		fx.core->LoadOrCreatePlayer(0, first, "G");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 1; }));
		fx.core->LoadOrCreatePlayer(1, second, "H");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 2; }));
		fx.core->FindPlayer(0)->stats[levels_ranks::ST_EXP] = 1000;
		fx.core->FindPlayer(1)->stats[levels_ranks::ST_EXP] = 500;
		fx.core->SavePlayer(0);
		CHECK(Wait(*fx.db, [&] { return fx.positions.size() == 1; }));
		// The second player's row already carries a lastconnect (the INSERT sets it), so both
		// rows count for the play-time position - the first player's time is the 2nd one.
		CHECK_EQ_STR(Back(fx.positions), "0:1:2");
		fx.core->SavePlayer(1);
		CHECK(Wait(*fx.db, [&] { return fx.positions.size() == 2; }));
		CHECK_EQ_STR(Back(fx.positions), "1:2:2");
		// The session delta of the place is tracked: 0 was 1st and stays 1st.
		CHECK_EQ_INT(fx.core->FindPlayer(0)->session[levels_ranks::ST_PLACEINTOP], 0);
	}

	// --- top lists ---------------------------------------------------------------------------
	{
		Section("top lists");
		Fixture fx(base + 7);
		Settings settings = BaseSettings();
		settings.values[levels_ranks::LR_TopCount] = 2;
		if (!fx.Init(config, settings)) {
			return;
		}
		for (int i = 0; i < 3; ++i) {
			fx.core->LoadOrCreatePlayer(i, fx.NewSteamId(), "T" + std::to_string(i));
		}
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 3; }));
		fx.core->FindPlayer(0)->stats[levels_ranks::ST_EXP] = 300;
		fx.core->FindPlayer(1)->stats[levels_ranks::ST_EXP] = 200;
		fx.core->FindPlayer(2)->stats[levels_ranks::ST_EXP] = 100;
		fx.now += 3600 * 2;
		for (int i = 0; i < 3; ++i) {
			fx.core->SavePlayer(i);
		}
		Settle(*fx.db, 6);

		std::vector<levels_ranks::TopRow> rows;
		int calls = 0;
		fx.core->LoadTopPlayers(false, [&](std::vector<levels_ranks::TopRow> result) {
			rows = std::move(result);
			++calls;
		});
		CHECK(Wait(*fx.db, [&] { return calls == 1; }));
		CHECK_EQ_INT(rows.size(), 2); // lr_top_count
		CHECK_EQ_INT(static_cast<int>(RowAt(rows, 0).value), 300);
		CHECK_EQ_INT(static_cast<int>(RowAt(rows, 1).value), 200);
		// The name column comes from the live player name at save time (findPlayerName), not from
		// the argument of the INSERT.
		CHECK_EQ_STR(RowAt(rows, 0).name, "Player0");

		std::vector<levels_ranks::TopRow> by_time;
		int time_calls = 0;
		fx.core->LoadTopPlayers(true, [&](std::vector<levels_ranks::TopRow> result) {
			by_time = std::move(result);
			++time_calls;
		});
		CHECK(Wait(*fx.db, [&] { return time_calls == 1; }));
		CHECK_EQ_INT(by_time.size(), 2);
		CHECK(by_time[0].value >= 2.0 - 0.001 && by_time[0].value <= 2.0 + 0.001); // 7200 s in hours
	}

	// --- the cleanup command -------------------------------------------------------------------
	{
		Section("cleanup");
		Fixture fx(base + 8);
		if (!fx.Init(config)) {
			return;
		}
		const std::string steamid = fx.NewSteamId();
		fx.core->LoadOrCreatePlayer(6, steamid, "I");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 1; }));
		fx.core->FindPlayer(6)->stats[levels_ranks::ST_KILLS] = 9;
		fx.core->FindPlayer(6)->stats[levels_ranks::ST_EXP] = 777;
		fx.core->SavePlayer(6);
		Settle(*fx.db);

		// exp: value and rank only.
		bool done = false;
		fx.core->CleanupData(levels_ranks::LR_ExpData, [&done](bool ok) { done = ok; });
		CHECK(Wait(*fx.db, [&] { return done; }));
		CHECK_EQ_INT(fx.cleanup.size(), 1);
		CHECK_EQ_INT(fx.cleanup.empty() ? -1 : fx.cleanup.front(), levels_ranks::LR_ExpData);
		plgdb::Result row = fx.Row(steamid);
		CHECK_EQ_INT(row.GetInt(0, 0), 0);  // value = lr_start_points
		CHECK_EQ_INT(row.GetInt(0, 1), 0);  // rank
		CHECK_EQ_INT(row.GetInt(0, 2), 9);  // kills untouched

		// stats: the counters, not the experience.
		fx.core->FindPlayer(6)->stats[levels_ranks::ST_EXP] = 555;
		fx.core->SavePlayer(6);
		Settle(*fx.db);
		done = false;
		fx.core->CleanupData(levels_ranks::LR_StatsData, [&done](bool ok) { done = ok; });
		CHECK(Wait(*fx.db, [&] { return done; }));
		row = fx.Row(steamid);
		CHECK_EQ_INT(row.GetInt(0, 0), 555);
		CHECK_EQ_INT(row.GetInt(0, 2), 0);

		// all: the row disappears.
		done = false;
		fx.core->CleanupData(levels_ranks::LR_AllData, [&done](bool ok) { done = ok; });
		CHECK(Wait(*fx.db, [&] { return done; }));
		row = fx.Row(steamid);
		CHECK_QUERY_OK(row);
		CHECK_EQ_INT(row.RowCount(), 0);
	}

	// --- the expired-connection cleanup ---------------------------------------------------------
	{
		Section("clean database");
		Fixture fx(base + 9);
		Settings settings = BaseSettings();
		settings.values[levels_ranks::LR_CleanDB_Days] = 30;
		if (!fx.Init(config, settings)) {
			return;
		}
		const std::string steamid = fx.NewSteamId();
		fx.core->LoadOrCreatePlayer(2, steamid, "J");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 1; }));
		fx.core->FindPlayer(2)->stats[levels_ranks::ST_EXP] = 42;
		fx.core->SavePlayer(2);
		Settle(*fx.db);
		CHECK_EQ_INT(fx.Row(steamid).GetInt(0, 8), fx.now); // lastconnect

		// 31 days later the row loses its lastconnect (and with it its place in the tops).
		fx.now += 31LL * 86400;
		fx.core->CleanDatabase();
		Settle(*fx.db);
		CHECK_EQ_INT(fx.Row(steamid).GetInt(0, 8), 0);
		CHECK_EQ_INT(fx.Row(steamid).GetInt(0, 0), 42); // the statistics stay
	}

	// --- a slot reused while a load is in flight -------------------------------------------------
	{
		Section("slot reuse and stale load");
		Fixture fx(base + 11);
		if (!fx.Init(config)) {
			return;
		}
		// Two players with their own rows, so the loads below have distinct values to apply.
		const std::string first = fx.NewSteamId();
		const std::string second = fx.NewSteamId();
		fx.core->LoadOrCreatePlayer(0, first, "L");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 1; }));
		fx.core->FindPlayer(0)->stats[levels_ranks::ST_EXP] = 111;
		fx.core->SavePlayer(0);
		fx.core->LoadOrCreatePlayer(1, second, "M");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 2; }));
		fx.core->FindPlayer(1)->stats[levels_ranks::ST_EXP] = 222;
		fx.core->FindPlayer(1)->stats[levels_ranks::ST_KILLS] = 5;
		fx.core->SavePlayer(1);
		Settle(*fx.db);

		// The counters of a played session are left in the slot the next player takes over.
		PlayerInfo* info = fx.core->FindPlayer(1);
		info->session[levels_ranks::ST_KILLS] = 3;
		info->round_exp = 40;
		info->kill_streak = 4;
		info->have_bomb = true;
		info->stats[levels_ranks::ST_KILLS] = 9;

		// Loading the slot wipes it first (OnClientPutInServer did the same): the newcomer starts
		// from nothing, not from the counters of the previous occupant.
		fx.core->LoadOrCreatePlayer(1, first, "L");
		info = fx.core->FindPlayer(1);
		CHECK_EQ_INT(info->initialized, 0);
		CHECK_EQ_STR(info->steamid, first);
		CHECK_EQ_INT(info->session[levels_ranks::ST_KILLS], 0);
		CHECK_EQ_INT(info->round_exp, 0);
		CHECK_EQ_INT(info->kill_streak, 0);
		CHECK_EQ_INT(info->have_bomb, 0);
		CHECK_EQ_INT(info->stats[levels_ranks::ST_KILLS], 0);
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 3; }));
		CHECK_EQ_INT(fx.core->FindPlayer(1)->stats[levels_ranks::ST_EXP], 111);

		// Two loads race for one slot: the row of the player who is no longer on it is dropped.
		fx.loaded.clear();
		fx.core->LoadOrCreatePlayer(2, second, "M");
		fx.core->LoadOrCreatePlayer(2, first, "L");
		CHECK(Wait(*fx.db, [&] { return !fx.loaded.empty(); }));
		Settle(*fx.db, 4);
		CHECK_EQ_INT(fx.loaded.size(), 1);
		CHECK_EQ_STR(Front(fx.loaded), "2:" + first);
		CHECK_EQ_STR(fx.core->FindPlayer(2)->steamid, first);
		CHECK_EQ_INT(fx.core->FindPlayer(2)->stats[levels_ranks::ST_EXP], 111);
	}

	// --- a load that is still in flight is never re-issued ---------------------------------------
	{
		Section("in-flight load and slot reuse");
		Fixture fx(base + 13);
		if (!fx.Init(config)) {
			return;
		}
		const std::string first = fx.NewSteamId();
		const std::string second = fx.NewSteamId();

		// Nothing is polled between the calls, so the callback of the first load cannot have run:
		// its row query is still in flight.
		CHECK(fx.core->LoadOrCreatePlayer(0, first, "P"));
		CHECK(fx.core->LoadInFlight(0, first));
		CHECK(!fx.core->LoadInFlight(0, second)); // another player is another load
		CHECK(!fx.core->LoadInFlight(1, first));
		CHECK(!fx.core->LoadInFlight(-1, first));
		CHECK(!fx.core->LoadInFlight(levels_ranks::kMaxSlots, first));

		// The second ask for the same player is refused and does not touch the slot (before the fix
		// it wiped the slot and sent a second SELECT): whatever the slot holds meanwhile survives.
		fx.core->FindPlayer(0)->stats[levels_ranks::ST_KILLS] = 3;
		CHECK(!fx.core->LoadOrCreatePlayer(0, first, "P"));
		CHECK_EQ_INT(fx.core->FindPlayer(0)->stats[levels_ranks::ST_KILLS], 3);
		CHECK_EQ_INT(fx.core->FindPlayer(0)->initialized, 0);
		CHECK_EQ_STR(fx.core->FindPlayer(0)->steamid, first);

		// A different player on the slot is loaded at once and inherits nothing.
		CHECK(fx.core->LoadOrCreatePlayer(0, second, "Q"));
		CHECK_EQ_INT(fx.core->FindPlayer(0)->stats[levels_ranks::ST_KILLS], 0);
		CHECK(!fx.core->LoadInFlight(0, first));
		CHECK(fx.core->LoadInFlight(0, second));

		CHECK(Wait(*fx.db, [&] { return !fx.loaded.empty(); }));
		Settle(*fx.db, 4);
		// Only the newcomer's load landed; the abandoned one created no row and reported no error.
		CHECK_EQ_INT(fx.loaded.size(), 1);
		CHECK_EQ_STR(Front(fx.loaded), "0:" + second);
		CHECK(fx.core->IsInitialized(0));
		CHECK_EQ_STR(fx.core->FindPlayer(0)->steamid, second);
		CHECK_EQ_INT(fx.Row(first).RowCount(), 0);
		CHECK_EQ_INT(fx.Row(second).RowCount(), 1);
		CHECK_EQ_INT(fx.errors.size(), 0);
	}

	// --- the retry after a failed load and after the timeout --------------------------------------
	{
		Section("failed load and retry");
		Fixture fx(base + 14);
		if (!fx.Init(config)) {
			return;
		}
		const std::string steamid = fx.NewSteamId();
		// A load against a table that does not exist fails: the error is reported, the slot stays
		// uninitialised and the attempt is no longer in flight (the reconcile may retry it).
		Settings broken = fx.core->GetSettings();
		broken.table_name = "lrt_missing_" + std::to_string(fx.run_base % 1000000);
		fx.core->SetSettings(broken);
		CHECK(fx.core->LoadOrCreatePlayer(3, steamid, "R"));
		CHECK(Wait(*fx.db, [&] { return !fx.errors.empty(); }));
		CHECK_EQ_INT(fx.errors.size(), 1);
		CHECK(!fx.core->LoadInFlight(3, steamid));
		CHECK_EQ_INT(fx.core->FindPlayer(3)->initialized, 0);

		// The retry of the same player must not wipe what the slot gathered meanwhile...
		fx.core->FindPlayer(3)->stats[levels_ranks::ST_KILLS] = 7;
		fx.core->FindPlayer(3)->session[levels_ranks::ST_KILLS] = 7;
		Settings fixed = fx.core->GetSettings();
		fixed.table_name = "lrt_" + std::to_string(fx.run_base % 1000000);
		fx.core->SetSettings(fixed);
		CHECK(fx.core->LoadOrCreatePlayer(3, steamid, "R"));
		CHECK_EQ_INT(fx.core->FindPlayer(3)->stats[levels_ranks::ST_KILLS], 7);
		CHECK_EQ_INT(fx.core->FindPlayer(3)->session[levels_ranks::ST_KILLS], 7);
		// ... and it loads the row of a new player: the start values, the counted row, one failure.
		CHECK(Wait(*fx.db, [&] { return fx.core->IsInitialized(3); }));
		CHECK_EQ_INT(fx.core->FindPlayer(3)->stats[levels_ranks::ST_KILLS], 0);
		CHECK_EQ_INT(fx.core->FindPlayer(3)->session[levels_ranks::ST_KILLS], 0);
		CHECK_EQ_INT(fx.core->FindPlayer(3)->stats[levels_ranks::ST_RANK], 1);
		CHECK_EQ_INT(fx.core->PlayerCount(), 1);
		CHECK_EQ_INT(fx.errors.size(), 1); // the failure above; the retry itself was clean
		CHECK_EQ_INT(fx.Row(steamid).RowCount(), 1);
	}

	{
		Section("load timeout");
		Fixture fx(base + 15);
		if (!fx.Init(config)) {
			return;
		}
		const std::string steamid = fx.NewSteamId();
		CHECK(fx.core->LoadOrCreatePlayer(2, steamid, "T"));
		CHECK(fx.core->LoadInFlight(2, steamid));
		fx.now += levels_ranks::kLoadTimeoutSeconds - 1;
		CHECK(fx.core->LoadInFlight(2, steamid)); // within the timeout the load still holds the slot
		CHECK(!fx.core->LoadOrCreatePlayer(2, steamid, "T"));
		fx.now += 1; // the timeout expires: a retry may go out
		CHECK(!fx.core->LoadInFlight(2, steamid));
		CHECK(fx.core->LoadOrCreatePlayer(2, steamid, "T"));
		CHECK(fx.core->LoadInFlight(2, steamid));
		// Both SELECTs ran; the abandoned first answer is dropped (its generation is stale) and the
		// retry creates the row - no duplicate-key error, one onPlayerLoaded.
		CHECK(Wait(*fx.db, [&] { return fx.core->IsInitialized(2); }));
		Settle(*fx.db, 4);
		CHECK_EQ_INT(fx.loaded.size(), 1);
		CHECK_EQ_STR(Front(fx.loaded), "2:" + steamid);
		CHECK_EQ_INT(fx.Row(steamid).RowCount(), 1);
		CHECK_EQ_INT(fx.errors.size(), 0);
	}

	// --- two loads of a new player race their INSERT ----------------------------------------------
	{
		Section("racing inserts of a new player");
		Fixture fx(base + 16);
		if (!fx.Init(config)) {
			return;
		}
		const std::string steamid = fx.NewSteamId();
		// Both loads are issued before anything is polled: both SELECTs see an empty table, so both
		// INSERT the same primary key. The original failed here with error 1062; the tolerant INSERT
		// absorbs the second one and the loser reads the row that exists.
		CHECK(fx.core->LoadOrCreatePlayer(0, steamid, "U"));
		CHECK(fx.core->LoadOrCreatePlayer(1, steamid, "U"));
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 2; }));
		Settle(*fx.db, 4);
		CHECK_EQ_INT(fx.errors.size(), 0);
		CHECK_EQ_INT(fx.Row(steamid).RowCount(), 1);
		CHECK_EQ_INT(fx.core->PlayerCount(), 1); // one row was inserted, the tolerated one is not one
		CHECK(fx.core->IsInitialized(0));
		CHECK(fx.core->IsInitialized(1));
		CHECK_EQ_STR(fx.core->FindPlayer(0)->steamid, steamid);
		CHECK_EQ_STR(fx.core->FindPlayer(1)->steamid, steamid);
		CHECK_EQ_INT(fx.core->FindPlayer(0)->stats[levels_ranks::ST_KILLS], 0);
		CHECK_EQ_INT(fx.core->FindPlayer(1)->stats[levels_ranks::ST_KILLS], 0);
		CHECK_EQ_INT(fx.core->FindPlayer(0)->stats[levels_ranks::ST_EXP], 0);
		CHECK_EQ_INT(fx.core->FindPlayer(1)->stats[levels_ranks::ST_EXP], 0);
	}

	// --- the reload after the cleanup command ----------------------------------------------------
	{
		Section("cleanup reload");
		Fixture fx(base + 12);
		if (!fx.Init(config)) {
			return;
		}
		const std::string steamid = fx.NewSteamId();
		fx.core->LoadOrCreatePlayer(8, steamid, "N");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 1; }));
		fx.core->FindPlayer(8)->stats[levels_ranks::ST_EXP] = 500;
		fx.core->FindPlayer(8)->stats[levels_ranks::ST_KILLS] = 3;
		fx.core->SavePlayer(8);
		Settle(*fx.db);

		// The statistics cleanup empties the counters in the table. Without the reload the slot
		// keeps its own and the next save writes them back.
		bool done = false;
		fx.core->CleanupData(levels_ranks::LR_StatsData, [&done](bool ok) { done = ok; });
		CHECK(Wait(*fx.db, [&] { return done; }));
		fx.loaded.clear();
		fx.core->ReloadInitializedPlayers();
		CHECK(Wait(*fx.db, [&] { return !fx.loaded.empty(); }));
		CHECK_EQ_STR(Front(fx.loaded), "8:" + steamid);
		const PlayerInfo* info = fx.core->FindPlayer(8);
		CHECK(info != nullptr && info->initialized);
		CHECK_EQ_INT(info->stats[levels_ranks::ST_KILLS], 0);
		CHECK_EQ_INT(info->stats[levels_ranks::ST_EXP], 500);

		// A save right after the reload stores the cleaned counters.
		fx.core->SavePlayer(8);
		Settle(*fx.db);
		CHECK_EQ_INT(fx.Row(steamid).GetInt(0, 0), 500);
		CHECK_EQ_INT(fx.Row(steamid).GetInt(0, 2), 0);
	}

	// --- two slots, the same player (the row follows the steam id) --------------------------------
	{
		Section("new row counting");
		Fixture fx(base + 10);
		if (!fx.Init(config)) {
			return;
		}
		fx.core->ConfigureDatabase();
		CHECK(Wait(*fx.db, [&] { return fx.core_ready == 1; }));
		const std::string first = fx.NewSteamId();
		fx.core->LoadOrCreatePlayer(0, first, "K");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 1; }));
		CHECK_EQ_INT(fx.core->PlayerCount(), 1);
		// The same row again does not count twice.
		fx.core->SavePlayer(0, true);
		Settle(*fx.db);
		fx.core->LoadOrCreatePlayer(1, first, "K");
		CHECK(Wait(*fx.db, [&] { return fx.loaded.size() == 2; }));
		CHECK_EQ_INT(fx.core->PlayerCount(), 1);
		CHECK(fx.core->IsInitialized(1));
		fx.core->MarkUninitialized(1);
		CHECK(!fx.core->IsInitialized(1));
		CHECK(fx.core->IsInitialized(99) == false);
		CHECK(fx.core->FindPlayer(99) == nullptr);
		CHECK(fx.core->FindPlayer(-1) == nullptr);
	}
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not swallow the section it happened in
	std::printf("levels_ranks core tests\n");

	TestSettingsDefaults();
	TestSettingsAliasing();
	TestSettingsCoefficientClamp();
	TestShippedSettingsFile();
	TestPermissionsByTableName();
	TestRankMath();
	TestKillMath();

	RunDatabaseSuite(SqliteConfig(), "sqlite (:memory:)");

	plgdb::Config mysql;
	std::string error;
	if (MySqlConfig(mysql, error)) {
		std::printf("== mysql: %s:%u/%s\n", mysql.host.c_str(), mysql.port, mysql.database.c_str());
		RunDatabaseSuite(mysql, "mysql");
	} else if (!error.empty()) {
		std::printf("FATAL: %s\n", error.c_str());
		++g_failures;
	} else {
		std::printf("== mysql: skipped (MYSQL_HOST not set)\n");
	}

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
