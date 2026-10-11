// levels_ranks — lr_bench: an engine-free load/consistency benchmark for the core (lr_core).
//
// Test-only program (everything under tests/ is never packaged): it drives LrCore exactly the way
// plugin.cpp drives it on a live server, for a simulated 24-player match, and reports
//   * the game-thread time of every core call, per event type (count/avg/p99/max, us),
//   * the database side through a counting wrapper around plgdb::Database (statements sent,
//     statements/s, queue length, callback latency from the call to its callback in Poll),
//   * and a consistency check at the end: after the final disconnect saves and a full drain it
//     re-reads every bench player's row and compares it with the counters the bench tracked itself.
//
// The database is MySQL when MYSQL_HOST is set (the env names of tests/lr_core_tests.cpp:
// MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE) and SQLite ":memory:" otherwise.
// Only the bench's own table (lr_bench_<pid>, dropped by the bench before it creates it) is
// touched in that database.
//
// The match (defaults, all overridable on the command line — see --help):
//   * 24 players, steamids 76561198000100000 + i, names "bench_i", slots 0..23, teams 2 (T) and 3
//     (CT) alternating, loaded like on auth (OnClientAuthenticated -> LoadOrCreatePlayer) over the
//     first seconds;
//   * rounds of ~2 min of game time (uniform 102..120 s), compressed into real time at --speed
//     (default 64x): 30 rounds by default. Per round: kills at ~1 kill every 2..4 s of game time
//     (headshots ~40%, an assister ~35%, respawn ~3..7 s later so the stream keeps flowing), shots
//     for every live player every 1.5..3.5 s with ~20% turning into player_hurt (hits), a bomb
//     pickup/plant in ~half of the rounds (about half of the plants are defused, the rest go off
//     and end the round early), round_end with win/lose exp + MVP for the winners;
//   * some players disconnect and reconnect in the middle (the script in Setup()), the last one
//     never returns: the saved row keeps that player's final state;
//   * every core call is made from the main thread (the "game thread"); plgdb is polled once per
//     simulated frame at 64 fps, like OnPluginUpdate does, and the plugin's one-second
//     reconcile/gate timer runs once per simulated second;
//   * the core's clock is the simulated match clock, so playtime and lastconnect are game time.
//
// Divergences from a real server, on purpose (the bench is a model, not the plugin):
//   * the engine is gone: "connected"/"alive" are the bench's own state and replace the engine
//     checks of plugin.cpp (GetPawnEntity/IsFakeClient/IsAuthenticated are all true while a player
//     is connected; there are no bots);
//   * the hooks count their calls instead of printing chat/menus, so the measured per-event time
//     is the core call plus the plugin's own bookkeeping, not the engine's ClientPrint;
//   * the handlers below are copies of the plugin.cpp bodies (line references in the comments);
//     the phrase keys, the guards (result.status, initialized) and the counter updates are the
//     same, so the final rows can be compared with the bench's own counters.
//
// The bench's tracked counters follow the core across a load: every successful
// LoadOrCreatePlayer replaces the counter set with the row (a reconnect saves first, so that is a
// no-op). Until the fix the reconcile re-issued a load after kLoadRetrySeconds while the previous
// one was still unanswered, the re-issue reset the slot and its answer replaced the counters
// counted since the last save; the bench measures how many counters each reload discarded and
// reports them (rather than counting them against the consistency check, which compares the
// database with the state the core holds at the end). The fixed core refuses a load for a player
// whose previous load is in flight - the reconcile and the auth listener ask LoadInFlight first,
// and the bench reports how often an unanswered load was left alone (retry_load_skips) and how
// many loads were issued while one was unanswered (retry_loads, 0 with the fix).
//
// Exit code: 0 = match ran and every row matched, 1 = at least one row mismatched,
// 2 = the database/settings could not be used.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

#include <plgdb/db.hpp>

#include <plg/plugin.hpp>
#include <pisex/keyvalues.hpp>

#include "lr_core.hpp"
#include "lr_keyvalues.hpp"

// The plg globals a Plugify plugin defines through PLUGIFY_PLUGIN; pisexcompat (the KV1 parser
// used here) references a few of them, so the bench provides them (null = the fallbacks apply),
// exactly like tests/lr_core_tests.cpp does.
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

using Clock = std::chrono::steady_clock;
using levels_ranks::ExpResult;
using levels_ranks::LrCore;
using levels_ranks::PlayerInfo;
using levels_ranks::Settings;

constexpr int kBenchPlayers = 24;              // the size of the simulated server
constexpr int kSlots = levels_ranks::kMaxSlots;
constexpr double kFrameRate = 64.0;            // simulated frames per game second
constexpr std::uint64_t kSteamBase = 76561198000100000ULL;
constexpr int64_t kClockBase = 1'700'000'000;  // the core clock at match second 0
constexpr int64_t kLoadRetrySeconds = 5;       // lr_plugin.hpp kLoadRetrySeconds

double ToUs(Clock::duration duration) {
	return std::chrono::duration<double, std::micro>(duration).count();
}

// The phrases of GiveExpForStreakKills (plugin.cpp:130), for the streak bonuses.
constexpr const char* const kStreakPhrases[] = {"DoubleKill", "TripleKill", "Domination",   "Rampage",
												"MegaKill",   "Ownage",     "UltraKill",    "KillingSpree",
												"MonsterKill", "Unstoppable", "GodLike"};

// --- the counting database wrapper ---------------------------------------------------------------
// Everything the core sends goes through here, so the statement counts and the callback latencies
// are measured without touching lr_core. Query/Poll run on the game thread only (plgdb contract).
class CountingDatabase final : public plgdb::Database {
public:
	struct Snapshot {
		std::uint64_t async_statements = 0; // Query() calls (all of them carry a callback here)
		std::uint64_t sync_statements = 0;  // QuerySync() calls (schema, drops, the bench's reads)
		std::uint64_t transactions = 0;
		std::uint64_t callbacks = 0;
		double latency_total_us = 0.0;
		double latency_max_us = 0.0;
		double sync_blocking_us = 0.0;
		std::vector<float> latencies_us; // one sample per executed callback

		std::uint64_t Statements() const { return async_statements + sync_statements; }
		double LatencyAverageUs() const {
			return callbacks ? latency_total_us / static_cast<double>(callbacks) : 0.0;
		}
		double LatencyP99Us() const {
			if (latencies_us.empty()) {
				return 0.0;
			}
			std::vector<float> sorted = latencies_us;
			std::sort(sorted.begin(), sorted.end());
			std::size_t index = static_cast<std::size_t>(std::ceil(0.99 * static_cast<double>(sorted.size())));
			if (index > 0) {
				--index;
			}
			return sorted[std::min(index, sorted.size() - 1)];
		}
	};

	explicit CountingDatabase(std::unique_ptr<plgdb::Database> inner) : m_inner(std::move(inner)) {}

	plgdb::Driver GetDriver() const noexcept override { return m_inner->GetDriver(); }

	void Query(std::string sql, plgdb::Params params, plgdb::Callback callback) override {
		++m_async;
		const Clock::time_point enqueued = Clock::now();
		m_inner->Query(std::move(sql), std::move(params),
					   [this, enqueued, callback = std::move(callback)](const plgdb::Result& result) {
						   const double us = ToUs(Clock::now() - enqueued);
						   ++m_callbacks;
						   m_latency_total_us += us;
						   m_latency_max_us = std::max(m_latency_max_us, us);
						   m_latencies.push_back(static_cast<float>(us));
						   if (callback) {
							   callback(result);
						   }
					   });
	}

	void Transaction(std::vector<plgdb::Statement> statements, plgdb::Callback callback) override {
		++m_transactions;
		m_async += statements.size();
		const Clock::time_point enqueued = Clock::now();
		m_inner->Transaction(std::move(statements), [this, enqueued, callback = std::move(callback)](const plgdb::Result& result) {
			const double us = ToUs(Clock::now() - enqueued);
			++m_callbacks;
			m_latency_total_us += us;
			m_latency_max_us = std::max(m_latency_max_us, us);
			m_latencies.push_back(static_cast<float>(us));
			if (callback) {
				callback(result);
			}
		});
	}

	plgdb::Result QuerySync(std::string sql, plgdb::Params params) override {
		++m_sync;
		const Clock::time_point start = Clock::now();
		plgdb::Result result = m_inner->QuerySync(std::move(sql), std::move(params));
		m_sync_blocking_us += ToUs(Clock::now() - start);
		return result;
	}

	size_t Poll() override { return m_inner->Poll(); }
	size_t Pending() const noexcept override { return m_inner->Pending(); }

	Snapshot Take() const {
		Snapshot snapshot;
		snapshot.async_statements = m_async;
		snapshot.sync_statements = m_sync;
		snapshot.transactions = m_transactions;
		snapshot.callbacks = m_callbacks;
		snapshot.latency_total_us = m_latency_total_us;
		snapshot.latency_max_us = m_latency_max_us;
		snapshot.sync_blocking_us = m_sync_blocking_us;
		snapshot.latencies_us = m_latencies;
		return snapshot;
	}

	std::uint64_t Statements() const { return m_async + m_sync; }

private:
	std::unique_ptr<plgdb::Database> m_inner;
	std::uint64_t m_async = 0;
	std::uint64_t m_sync = 0;
	std::uint64_t m_transactions = 0;
	std::uint64_t m_callbacks = 0;
	double m_latency_total_us = 0.0;
	double m_latency_max_us = 0.0;
	double m_sync_blocking_us = 0.0;
	std::vector<float> m_latencies;
};

// --- per-event timing -----------------------------------------------------------------------------
struct EventStats {
	std::uint64_t count = 0;
	double total_us = 0.0;
	double max_us = 0.0;
	std::vector<float> samples_us;

	void Add(double us) {
		++count;
		total_us += us;
		max_us = std::max(max_us, us);
		samples_us.push_back(static_cast<float>(us));
	}

	double AverageUs() const { return count ? total_us / static_cast<double>(count) : 0.0; }

	double P99Us() const {
		if (samples_us.empty()) {
			return 0.0;
		}
		std::vector<float> sorted = samples_us;
		std::sort(sorted.begin(), sorted.end());
		std::size_t index = static_cast<std::size_t>(std::ceil(0.99 * static_cast<double>(sorted.size())));
		if (index > 0) {
			--index;
		}
		return sorted[std::min(index, sorted.size() - 1)];
	}
};

// --- the bench's own view of a player ------------------------------------------------------------
struct Counters {
	long long exp = 0; // the `value` column
	long long kills = 0;
	long long deaths = 0;
	long long headshots = 0;
	long long assists = 0;
	long long shots = 0;
	long long hits = 0;
	long long wins = 0;
	long long loses = 0;
};

struct PlayerTrack {
	int slot = -1;
	std::string steamid;
	std::string name;
	int team = 0;
	bool connected = false;
	bool alive = false;
	bool shot_running = false; // exactly one weapon_fire chain per connected player
	double next_shot = 0.0;
	int64_t last_load_attempt = 0;
	int round_kills = 0;         // bench-only (MVP pick)
	Counters tracked;
};

// The tracked fields that mirror a column of the table (the reload re-sync below walks this).
struct CounterField {
	long long Counters::*field;
	int stat;
	const char* column;
};

constexpr CounterField kTrackedFields[] = {
	{&Counters::kills, levels_ranks::ST_KILLS, "kills"},
	{&Counters::deaths, levels_ranks::ST_DEATHS, "deaths"},
	{&Counters::headshots, levels_ranks::ST_HEADSHOTS, "headshots"},
	{&Counters::assists, levels_ranks::ST_ASSISTS, "assists"},
	{&Counters::shots, levels_ranks::ST_SHOOTS, "shoots"},
	{&Counters::hits, levels_ranks::ST_HITS, "hits"},
	{&Counters::wins, levels_ranks::ST_ROUNDSWIN, "round_win"},
	{&Counters::loses, levels_ranks::ST_ROUNDSLOSE, "round_lose"},
};

enum class EventKind {
	Connect,
	Disconnect,
	RoundStart,
	RoundEnd,
	Mvp,
	MatchEnd,
	Kill,
	Shot,
	Respawn,
	BombPickup,
	BombDrop,
	BombPlant,
	BombDefuse,
	MenuTop,
};

struct Event {
	double time = 0.0;
	EventKind kind{};
	int slot = -1;
	int round = 0; // round-scoped events carry the round they belong to (0 = global)
};

struct EventLater {
	bool operator()(const Event& a, const Event& b) const { return a.time > b.time; }
};

struct Options {
	int rounds = 30;
	double round_seconds = 120.0;
	double speed = 64.0; // game seconds per real second; 0 = run flat out
	std::uint64_t seed = 0;
	bool help = false;
};

// --- the bench ------------------------------------------------------------------------------------
class Bench {
public:
	Bench(const Options& options, Settings settings, const plgdb::Config& config, bool mysql,
		  std::string settings_path)
		: m_options(options), m_settings(std::move(settings)), m_config(config), m_mysql(mysql),
		  m_settings_path(std::move(settings_path)) {}

	int Run();

private:
	// --- setup and reporting --------------------------------------------------------------------
	bool Setup();
	void InstallHooks();
	void PrintHeader() const;
	void PrintMatchSummary() const;
	void PrintEventTable() const;
	void PrintHistory() const;
	void PrintDatabase(const CountingDatabase::Snapshot& db, double seconds, std::uint64_t match_statements) const;
	int CheckConsistency();

	// --- the frame loop -------------------------------------------------------------------------
	void Match();
	void DispatchDue();
	void Dispatch(const Event& event);
	void SecondTick();
	void Reconcile();
	bool IssueLoad(int slot, int64_t now); // one LoadOrCreatePlayer (+ the retry-load meter)
	void Drain(int timeout_ms);
	void Pace() const;
	// The raw round trip of the connection (`SELECT 1`), so the statement latencies below can be
	// separated from what the network/handshake costs. Returns the average; |min_us| the minimum.
	double ProbeRoundTripUs(int samples, double& min_us);

	// --- the plugin.cpp bodies (engine checks replaced by the bench's connected/alive state) ----
	void Connect(int slot);
	void Disconnect(int slot);
	void FinalSaves();
	void RoundStart();
	void RoundEnd(int round);
	void Mvp(int round);
	void DoKill(int round);
	void DoShot(int slot);
	void BombPickup(int slot, int round);
	void BombDrop(int slot, int round);
	void BombPlant(int slot, int round);
	void BombDefuse(int slot, int round);
	void MenuTop();
	void DeathBody(int attacker, int victim, bool headshot, int assister);
	void RoundEndBody(int winner);
	void StreakBody(int slot); // GiveExpForStreakKills of plugin.cpp:923
	void FireBody(int slot);   // OnOtherEvent, weapon_fire
	void HurtBody(int victim, int attacker); // OnOtherEvent, player_hurt
	void BombBody(std::string_view name, int slot);

	// --- helpers --------------------------------------------------------------------------------
	ExpResult GiveExp(int slot, int value, const char* phrase);
	std::string Table() const { return "`" + m_settings.table_name + "`"; }
	int64_t NowClock() const { return kClockBase + static_cast<int64_t>(m_sim_now); }
	bool IsConnected(int slot) const { return slot >= 0 && slot < kSlots && m_tracks[slot].connected; }
	bool IsAlive(int slot) const { return slot >= 0 && slot < kSlots && m_tracks[slot].connected && m_tracks[slot].alive; }
	PlayerInfo* InfoOf(int slot) { return m_core->FindPlayer(slot); }
	void Schedule(EventKind kind, double time, int slot = -1, int round = 0) {
		m_queue.push(Event{time, kind, slot, round});
	}
	int PickSlot(const std::vector<int>& slots) {
		return slots[static_cast<std::size_t>(UniformInt(0, static_cast<int>(slots.size()) - 1))];
	}
	std::vector<int> AliveSlots(int team) const;
	double Uniform(double low, double high) {
		return std::uniform_real_distribution<double>(low, high)(m_rng);
	}
	int UniformInt(int low, int high) { return std::uniform_int_distribution<int>(low, high)(m_rng); }
	bool Bernoulli(double probability) {
		return std::uniform_real_distribution<double>(0.0, 1.0)(m_rng) < probability;
	}
	template <typename Body>
	void Measure(const char* name, Body&& body) {
		const Clock::time_point start = Clock::now();
		body();
		Stats(name).Add(ToUs(Clock::now() - start));
	}
	EventStats& Stats(const char* name) {
		for (auto& entry : m_stats) {
			if (entry.first == name) {
				return entry.second;
			}
		}
		m_stats.emplace_back(name, EventStats{});
		return m_stats.back().second;
	}

	// --- state ----------------------------------------------------------------------------------
	Options m_options;
	Settings m_settings;
	plgdb::Config m_config;
	bool m_mysql = false;
	std::string m_settings_path;

	std::unique_ptr<CountingDatabase> m_db;
	std::unique_ptr<LrCore> m_core;

	std::array<PlayerTrack, kSlots> m_tracks;
	std::map<int, std::vector<std::pair<bool, int>>> m_script; // round -> (connect?, slot)

	std::priority_queue<Event, std::vector<Event>, EventLater> m_queue;
	std::vector<std::pair<std::string, EventStats>> m_stats;

	std::mt19937_64 m_rng{0};
	double m_sim_now = 0.0;
	double m_frame_dt = 1.0 / kFrameRate;
	int m_last_second = -1;
	int m_round = 0;
	bool m_round_active = false;
	bool m_match_over = false;
	double m_round_end_time = 0.0;
	int m_planned_winner = 0; // 0 = coin flip; 2 = the bomb went off, 3 = defused
	int m_last_winner = 0;

	Clock::time_point m_real_start{};      // the whole run (setup included)
	Clock::time_point m_real_match_start{};
	Clock::time_point m_real_match_end{};
	double m_schema_us = 0.0;
	double m_round_trip_start_us = 0.0; // SELECT 1 before the match
	double m_round_trip_start_min_us = 0.0;
	double m_round_trip_end_us = 0.0; // ... and after the drain
	double m_round_trip_end_min_us = 0.0;

	std::size_t m_queue_max = 0;
	std::uint64_t m_frames = 0;
	std::uint64_t m_frames_with_queue = 0;
	std::uint64_t m_statements_at_match_start = 0; // the setup statements are reported separately

	std::vector<std::string> m_errors;
	std::uint64_t m_core_ready = 0;
	std::uint64_t m_player_loads = 0;
	std::uint64_t m_retry_loads = 0;        // loads issued while the previous one was still unanswered
	std::uint64_t m_retry_load_skips = 0;   // ticks that left an unanswered load alone (no re-issue)
	Counters m_reloaded_away;               // increments a mid-session reload threw away
	std::uint64_t m_connects = 0;
	std::uint64_t m_disconnects = 0;
	std::uint64_t m_give_exp_calls = 0;
	std::uint64_t m_rank_change_saves = 0; // ExpResult::saved (lr_db_savedataplayer_mode)
	std::uint64_t m_streak_saves = 0;      // GiveExpForStreakKills' SavePlayer
	std::uint64_t m_streak_bonuses = 0;
	std::uint64_t m_rank_changes = 0;
	std::uint64_t m_level_messages = 0;
	std::uint64_t m_exp_messages = 0;
	std::uint64_t m_pos_in_top = 0;
	std::uint64_t m_reset_events = 0;
	std::uint64_t m_cleanup_events = 0;
	std::uint64_t m_menu_tops = 0;
	std::uint64_t m_menu_rows = 0;
};

// --- setup ----------------------------------------------------------------------------------------

bool Bench::Setup() {
	std::unique_ptr<plgdb::Database> inner = plgdb::Database::Create(m_config);
	if (inner == nullptr) {
		std::printf("FATAL: plgdb::Database::Create returned null\n");
		return false;
	}
	m_db = std::make_unique<CountingDatabase>(std::move(inner));
	m_core = std::make_unique<LrCore>(*m_db);
	m_core->SetSettings(m_settings);
	m_core->SetClock([this] { return NowClock(); });
	InstallHooks();

	// The bench owns exactly one table; it drops and recreates it so repeated runs start clean.
	const plgdb::Result dropped = m_db->QuerySync("DROP TABLE IF EXISTS " + Table() + ";", {});
	if (!dropped.Ok()) {
		std::printf("FATAL: dropping %s failed: %s\n", Table().c_str(), dropped.Error().c_str());
		return false;
	}
	const Clock::time_point schema_start = Clock::now();
	try {
		m_core->CreateSchema();
	} catch (const std::exception& ex) {
		std::printf("FATAL: %s\n", ex.what());
		return false;
	}
	m_schema_us = ToUs(Clock::now() - schema_start);
	m_core->ConfigureDatabase();
	Drain(15000);
	if (m_core_ready == 0) {
		std::printf("FATAL: the core never became ready (is the database reachable?)\n");
		return false;
	}
	m_round_trip_start_us = ProbeRoundTripUs(5, m_round_trip_start_min_us);

	for (int i = 0; i < kBenchPlayers; ++i) {
		PlayerTrack& player = m_tracks[static_cast<std::size_t>(i)];
		player.slot = i;
		player.steamid = std::to_string(kSteamBase + static_cast<std::uint64_t>(i));
		player.name = "bench_" + std::to_string(i);
		player.team = (i % 2 == 0) ? 2 : 3; // 2 = T, 3 = CT
	}

	// The disconnect/reconnect script: two players leave and come back, one leaves for good, one
	// leaves late and returns for the last rounds. Round numbers are the simulated ones.
	m_script = {
		{5, {{false, 3}, {false, 9}}},
		{8, {{true, 3}}},
		{11, {{true, 9}}},
		{20, {{false, 17}}},
		{25, {{false, 21}}},
		{27, {{true, 21}}},
	};
	return true;
}

void Bench::InstallHooks() {
	// The hook fan-out of the plugin (plugin.cpp:317) is replaced by counters: printing chat and
	// menus is engine work that the bench cannot do (and does not want to measure).
	m_core->findPlayerName = [this](int slot) { return m_tracks[static_cast<std::size_t>(slot)].name; };
	m_core->onError = [this](std::string_view message) { m_errors.emplace_back(message); };
	m_core->onCoreReady = [this] { ++m_core_ready; };
	m_core->onPlayerLoaded = [this](int slot, const std::string&) {
		++m_player_loads;
		PlayerTrack& player = m_tracks[static_cast<std::size_t>(slot)];
		if (player.slot < 0) {
			return;
		}
		const PlayerInfo* info = m_core->FindPlayer(slot);
		if (info == nullptr) {
			return;
		}
		// The load has replaced the whole slot with the row: the bench's expectation follows it.
		// On a reconnect that is a no-op (the disconnect saved first); a load that replaced a slot
		// which was already playing throws away the counters of the last unsaved seconds - that is
		// accumulated into m_reloaded_away and reported.
		for (const CounterField& field : kTrackedFields) {
			const long long from_row = info->stats[static_cast<std::size_t>(field.stat)];
			const long long bench_value = player.tracked.*(field.field);
			if (bench_value > from_row) {
				m_reloaded_away.*(field.field) += bench_value - from_row;
			}
			player.tracked.*(field.field) = from_row;
		}
		if (player.tracked.exp > info->stats[levels_ranks::ST_EXP]) {
			m_reloaded_away.exp += player.tracked.exp - info->stats[levels_ranks::ST_EXP];
		}
		player.tracked.exp = info->stats[levels_ranks::ST_EXP];
	};
	m_core->onResetPlayerStats = [this](int, const std::string&) { ++m_reset_events; };
	m_core->onLevelChangedPre = [](int, int&, int) {};
	m_core->onLevelChangedPost = [this](int, int, int) { ++m_rank_changes; };
	m_core->onPlayerPosInTop = [this](int, int, int) { ++m_pos_in_top; };
	m_core->onExpChangedPre = [](int, int&) {};
	m_core->onExpChangedPost = [](int, int, int) {};
	m_core->onDatabaseCleanup = [this](int) { ++m_cleanup_events; };
	m_core->onLevelMessage = [this](int, int, int, bool, bool) { ++m_level_messages; };
	m_core->onExpMessage = [this](int, const std::string&, int, int) { ++m_exp_messages; };
}

// --- the frame loop -------------------------------------------------------------------------------

void Bench::Match() {
	m_real_match_start = Clock::now();
	m_statements_at_match_start = m_db->Statements();

	// The joins of the first seconds (the auth listener of the plugin) and the first round.
	for (int i = 0; i < kBenchPlayers; ++i) {
		Schedule(EventKind::Connect, 0.1 + 0.15 * i, i, 0);
	}
	Schedule(EventKind::MenuTop, 4.0, -1, 0);
	Schedule(EventKind::RoundStart, 8.0, -1, 0);

	for (;;) {
		m_sim_now += m_frame_dt;
		DispatchDue();
		m_db->Poll();
		const std::size_t pending = m_db->Pending();
		m_queue_max = std::max(m_queue_max, pending);
		if (pending > 0) {
			++m_frames_with_queue;
		}
		++m_frames;

		const int second = static_cast<int>(m_sim_now);
		if (second != m_last_second) {
			m_last_second = second;
			SecondTick();
		}
		if (m_match_over) {
			break;
		}
		Pace();
	}

	FinalSaves();
	Drain(30000);
	m_real_match_end = Clock::now();
}

void Bench::DispatchDue() {
	while (!m_queue.empty() && m_queue.top().time <= m_sim_now) {
		const Event event = m_queue.top();
		m_queue.pop();
		if (event.round != 0 && event.round != m_round) {
			continue; // a leftover of an earlier round (a defuse may end a round early)
		}
		Dispatch(event);
	}
}

void Bench::Dispatch(const Event& event) {
	switch (event.kind) {
	case EventKind::Connect: Connect(event.slot); break;
	case EventKind::Disconnect: Disconnect(event.slot); break;
	case EventKind::RoundStart: RoundStart(); break;
	case EventKind::RoundEnd:
		if (m_round_active) {
			RoundEnd(event.round);
		}
		break;
	case EventKind::Mvp: Mvp(event.round); break;
	case EventKind::MatchEnd: m_match_over = true; break;
	case EventKind::Kill: DoKill(event.round); break;
	case EventKind::Shot: DoShot(event.slot); break;
	case EventKind::Respawn:
		if (IsConnected(event.slot)) {
			m_tracks[static_cast<std::size_t>(event.slot)].alive = true;
		}
		break;
	case EventKind::BombPickup: BombPickup(event.slot, event.round); break;
	case EventKind::BombDrop: BombDrop(event.slot, event.round); break;
	case EventKind::BombPlant: BombPlant(event.slot, event.round); break;
	case EventKind::BombDefuse: BombDefuse(event.slot, event.round); break;
	case EventKind::MenuTop: MenuTop(); break;
	}
}

void Bench::SecondTick() {
	// OnSecondTick() of plugin.cpp:854: the reconcile and the statistics gate.
	Measure("second_tick (reconcile+gate)", [this] {
		Reconcile();
		int players = 0;
		for (const PlayerTrack& player : m_tracks) {
			if (player.slot >= 0 && player.connected) {
				++players;
			}
		}
		// CheckAllowStatistic() of the plugin: lr_minplayers_count players with a team; the bench
		// is never in warmup (there is no game rules object to read).
		m_core->SetAllowStatistic(players >= m_settings.Value(levels_ranks::LR_MinplayersCount));
	});
}

void Bench::Reconcile() {
	// ReconcileSlots() of plugin.cpp: a connected player without his row is loaded again, rate
	// limited by kLoadRetrySeconds. A load that is still in flight for the same slot and steam id
	// is never re-issued (the fixed reconcile asks LoadInFlight first): the old code re-issued it
	// after kLoadRetrySeconds, which reset the slot and discarded the counters counted since the
	// last save once the second answer landed - those re-issues are counted as skipped here.
	const int64_t now = NowClock();
	for (PlayerTrack& player : m_tracks) {
		if (player.slot < 0 || !player.connected) {
			continue;
		}
		const PlayerInfo* info = m_core->FindPlayer(player.slot);
		if (info != nullptr && info->initialized && info->steamid == player.steamid) {
			continue;
		}
		if (m_core->LoadInFlight(player.slot, player.steamid)) {
			if (now - player.last_load_attempt >= kLoadRetrySeconds) {
				++m_retry_load_skips;
			}
			continue;
		}
		if (now - player.last_load_attempt < kLoadRetrySeconds) {
			continue;
		}
		// A retry is only possible now: the previous attempt failed or has not answered for
		// kLoadTimeoutSeconds (the core's own guard would refuse an in-flight load besides).
		IssueLoad(player.slot, now);
	}
}

bool Bench::IssueLoad(int slot, int64_t now) {
	PlayerTrack& player = m_tracks[static_cast<std::size_t>(slot)];
	player.last_load_attempt = now;
	// The meter of the reported bug: a load that goes out while the previous load of the same
	// player is still unanswered. The core refuses exactly those (LoadOrCreatePlayer returns false
	// then, and the callers above ask LoadInFlight first), so it stays 0 with the fix and was 16 in
	// the pre-fix run of this bench.
	const bool unanswered = m_core->LoadInFlight(slot, player.steamid);
	const bool issued = m_core->LoadOrCreatePlayer(slot, player.steamid, player.name);
	if (unanswered && issued) {
		++m_retry_loads;
	}
	return issued;
}

double Bench::ProbeRoundTripUs(int samples, double& min_us) {
	min_us = 0.0;
	double total = 0.0;
	int done = 0;
	for (int i = 0; i < samples; ++i) {
		const Clock::time_point start = Clock::now();
		const plgdb::Result result = m_db->QuerySync("SELECT 1;", {});
		const double us = ToUs(Clock::now() - start);
		if (!result.Ok()) {
			break;
		}
		total += us;
		min_us = done == 0 ? us : std::min(min_us, us);
		++done;
	}
	return done != 0 ? total / static_cast<double>(done) : 0.0;
}

void Bench::Drain(int timeout_ms) {
	const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
	for (;;) {
		const std::size_t ran = m_db->Poll();
		if (ran == 0 && m_db->Pending() == 0) {
			return;
		}
		if (Clock::now() >= deadline) {
			std::printf("lr_bench: drainage timed out with %zu statements pending\n", m_db->Pending());
			return;
		}
		if (ran == 0) {
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
	}
}

void Bench::Pace() const {
	if (m_options.speed <= 0.0) {
		// Unpaced stress mode: the game thread never waits. The database is then expected to fall
		// behind (that is what the queue metrics show), and the match itself degenerates - events
		// arrive before the players are even loaded, like a real server only if the DB were dead.
		return;
	}
	const auto target = m_real_match_start +
						std::chrono::duration_cast<Clock::duration>(
							std::chrono::duration<double>(m_sim_now / m_options.speed));
	if (const Clock::time_point now = Clock::now(); now < target) {
		std::this_thread::sleep_until(target);
	}
}

// --- the plugin.cpp bodies ------------------------------------------------------------------------

void Bench::Connect(int slot) {
	PlayerTrack& player = m_tracks[static_cast<std::size_t>(slot)];
	if (player.slot < 0 || player.connected) {
		return;
	}
	player.connected = true;
	player.alive = true;
	// The player starts firing (and keeps rescheduling through DoShot until he leaves again).
	if (!player.shot_running) {
		player.shot_running = true;
		player.next_shot = m_sim_now + Uniform(0.5, 3.0);
		Schedule(EventKind::Shot, player.next_shot, slot, 0);
	}
	player.last_load_attempt = NowClock();
	++m_connects;
	// OnClientAuthenticatedListener() of plugin.cpp:162 (and the reconcile's load). A load of the
	// same player that is still in flight is never re-issued - the listener asks the core first,
	// and the core refuses it besides; the row lands with the outstanding answer.
	if (m_core->LoadInFlight(slot, player.steamid)) {
		++m_retry_load_skips;
		return;
	}
	Measure("load_or_create (auth/reconnect)", [this, slot] { (void)IssueLoad(slot, NowClock()); });
}

void Bench::Disconnect(int slot) {
	PlayerTrack& player = m_tracks[static_cast<std::size_t>(slot)];
	if (player.slot < 0 || !player.connected) {
		return;
	}
	player.connected = false;
	player.alive = false;
	++m_disconnects;
	// OnClientDisconnectListener() of plugin.cpp:170.
	if (m_core->IsInitialized(slot)) {
		Measure("player_disconnect (save+online=0)", [this, slot] { m_core->SavePlayer(slot, true); });
	}
}

void Bench::FinalSaves() {
	// The server empties at the end of the match: every remaining player is saved like on a
	// disconnect, so the table holds the final state the consistency check reads back.
	for (int slot = 0; slot < kBenchPlayers; ++slot) {
		PlayerTrack& player = m_tracks[static_cast<std::size_t>(slot)];
		if (!player.connected) {
			continue;
		}
		player.connected = false;
		player.alive = false;
		++m_disconnects;
		if (m_core->IsInitialized(slot)) {
			Measure("final_disconnect (save+online=0)", [this, slot] { m_core->SavePlayer(slot, true); });
		}
	}
}

void Bench::RoundStart() {
	++m_round;
	m_round_active = true;
	m_planned_winner = 0;
	for (PlayerTrack& player : m_tracks) {
		if (player.slot < 0) {
			continue;
		}
		player.round_kills = 0;
		if (player.connected) {
			player.alive = true;
		}
	}
	// OnRoundEvent(), "round_start" (plugin.cpp:1109): CheckAllowStatistic(true).
	Measure("round_start", [this] {
		int players = 0;
		for (const PlayerTrack& player : m_tracks) {
			if (player.slot >= 0 && player.connected) {
				++players;
			}
		}
		m_core->SetAllowStatistic(players >= m_settings.Value(levels_ranks::LR_MinplayersCount));
	});

	const double duration = Uniform(0.85 * m_options.round_seconds, m_options.round_seconds);
	m_round_end_time = m_sim_now + duration;
	Schedule(EventKind::RoundEnd, m_round_end_time, -1, m_round);
	Schedule(EventKind::Kill, m_sim_now + Uniform(6.0, 12.0), -1, m_round);

	// The scripted leaves and returns of this round.
	if (const auto script = m_script.find(m_round); script != m_script.end()) {
		for (const auto& [connect, slot] : script->second) {
			Schedule(connect ? EventKind::Connect : EventKind::Disconnect,
					 m_sim_now + Uniform(15.0, duration - 15.0), slot, 0);
		}
	}

	// The bomb: a pickup halfway through the round in about half of the rounds.
	if (Bernoulli(0.5)) {
		const std::vector<int> ts = AliveSlots(2);
		if (!ts.empty()) {
			Schedule(EventKind::BombPickup, m_sim_now + Uniform(10.0, std::max(12.0, duration - 45.0)),
					 PickSlot(ts), m_round);
		}
	}
}

void Bench::RoundEnd(int round) {
	m_round_active = false;
	// A planted, undefused bomb wins the round; a defuse sets the winner to CT (BombDefuse below);
	// otherwise the round is a coin flip.
	const int winner = m_planned_winner != 0 ? m_planned_winner : (Bernoulli(0.5) ? 2 : 3);
	m_last_winner = winner;
	Measure("round_end (win/lose exp)", [this, winner] { RoundEndBody(winner); });
	Schedule(EventKind::Mvp, m_sim_now + 1.0, -1, round);
	if (round >= m_options.rounds) {
		Schedule(EventKind::MatchEnd, m_sim_now + 2.0, -1, 0);
	} else {
		Schedule(EventKind::RoundStart, m_sim_now + Uniform(4.0, 8.0), -1, 0);
	}
}

void Bench::Mvp(int round) {
	// OnRoundEvent(), "round_mvp" (plugin.cpp:1114): the best killer of the winning team, the
	// plugin only credits a slot with a pawn (alive).
	int best = -1;
	int best_kills = -1;
	for (const PlayerTrack& player : m_tracks) {
		if (player.slot < 0 || !IsAlive(player.slot) || player.team != m_last_winner) {
			continue;
		}
		if (player.round_kills > best_kills) {
			best_kills = player.round_kills;
			best = player.slot;
		}
	}
	if (best < 0) {
		return;
	}
	(void)round;
	Measure("round_mvp", [this, best] {
		(void)GiveExp(best, m_settings.Stats(levels_ranks::LR_ExpRoundMVP), "RoundMVP");
	});
}

void Bench::DoKill(int round) {
	if (!m_round_active) {
		return;
	}
	// A kill every 2..4 s of game time: a random alive attacker and an alive victim of the other
	// team (lr_allagainst_all = 0), an assister from the attacker's team ~35% of the time.
	std::vector<int> alive_t = AliveSlots(2);
	std::vector<int> alive_ct = AliveSlots(3);
	if (alive_t.empty() || alive_ct.empty()) {
		Schedule(EventKind::Kill, m_sim_now + 1.0, -1, round);
		return;
	}
	const bool victim_is_t = Bernoulli(0.5);
	const std::vector<int>& victims = victim_is_t ? alive_t : alive_ct;
	const std::vector<int>& attackers = victim_is_t ? alive_ct : alive_t;
	const int victim = PickSlot(victims);
	const int attacker = PickSlot(attackers);
	int assister = -1;
	if (Bernoulli(0.35) && attackers.size() > 1) {
		std::vector<int> mates;
		for (const int slot : attackers) {
			if (slot != attacker) {
				mates.push_back(slot);
			}
		}
		assister = PickSlot(mates);
	}
	const bool headshot = Bernoulli(0.4);
	Measure("player_death (assist/kill/death/headshot/streak)", [this, attacker, victim, headshot, assister] {
		DeathBody(attacker, victim, headshot, assister);
	});
	m_tracks[static_cast<std::size_t>(victim)].alive = false;
	Schedule(EventKind::Respawn, m_sim_now + Uniform(3.0, 7.0), victim, 0);
	Schedule(EventKind::Kill, m_sim_now + Uniform(2.0, 4.0), -1, round);
}

void Bench::DeathBody(int attacker, int victim, bool headshot, int assister) {
	// OnPlayerDeath() of plugin.cpp:950, engine checks replaced by the bench's state.
	if (assister >= 0 && assister < kSlots && IsAlive(assister)) {
		if (GiveExp(assister, m_settings.Stats(levels_ranks::LR_ExpGiveAssist), "AssisterKill").status) {
			if (PlayerInfo* info = InfoOf(assister); info != nullptr) {
				info->stats[levels_ranks::ST_ASSISTS]++;
				info->session[levels_ranks::ST_ASSISTS]++;
				++m_tracks[static_cast<std::size_t>(assister)].tracked.assists;
			}
		}
	}

	const PlayerInfo* victim_info = m_core->FindPlayer(victim);
	const PlayerInfo* attacker_info = m_core->FindPlayer(attacker);
	if (victim_info == nullptr || attacker_info == nullptr) {
		return;
	}
	const int victim_exp = victim_info->stats[levels_ranks::ST_EXP];
	const int attacker_exp = attacker_info->stats[levels_ranks::ST_EXP];
	// The plugin ORed pisex::IsFakeClient in; the bench has no bots.
	const bool fake_client = !victim_info->initialized;
	const bool fake_attacker = !attacker_info->initialized;

	int exp_attacker = 0;
	int exp_victim = 0;
	if (m_settings.type_statistics == 0) {
		exp_attacker = m_settings.Stats(levels_ranks::LR_ExpKill + (fake_client ? 1 : 0));
		exp_victim = m_settings.Stats(levels_ranks::LR_ExpDeath + (fake_attacker ? 1 : 0));
	} else if (!fake_client && !fake_attacker) {
		if (m_settings.type_statistics == 1) {
			exp_attacker = LrCore::KillExpAttacker(1, attacker_exp, victim_exp);
			if (exp_attacker < 1) {
				exp_attacker = 1;
			}
			exp_victim = LrCore::RatingVictimExp(exp_attacker, m_settings.Stats(levels_ranks::LR_ExpKillCoefficient));
		} else {
			exp_attacker = LrCore::SimpleModeExp(LrCore::KillExpAttacker(2, attacker_exp, victim_exp));
			exp_victim = exp_attacker;
		}
	}

	const ExpResult attacker_result = GiveExp(attacker, exp_attacker, "Kill");
	const ExpResult victim_result = GiveExp(victim, -exp_victim, "MyDeath");
	// PARITY: the original summed the two booleans to enter this block.
	if (attacker_result.status || victim_result.status) {
		if (!fake_attacker) {
			if (headshot &&
				GiveExp(attacker, m_settings.Stats(levels_ranks::LR_ExpGiveHeadShot), "HeadShotKill").status) {
				if (PlayerInfo* info = InfoOf(attacker); info != nullptr) {
					info->stats[levels_ranks::ST_HEADSHOTS]++;
					info->session[levels_ranks::ST_HEADSHOTS]++;
					++m_tracks[static_cast<std::size_t>(attacker)].tracked.headshots;
				}
			}
			if (PlayerInfo* info = InfoOf(attacker); info != nullptr) {
				info->stats[levels_ranks::ST_KILLS]++;
				info->session[levels_ranks::ST_KILLS]++;
				info->kill_streak++;
				++m_tracks[static_cast<std::size_t>(attacker)].tracked.kills;
				++m_tracks[static_cast<std::size_t>(attacker)].round_kills;
			}
		}
		if (!fake_client) {
			if (PlayerInfo* info = InfoOf(victim); info != nullptr) {
				info->stats[levels_ranks::ST_DEATHS]++;
				info->session[levels_ranks::ST_DEATHS]++;
				++m_tracks[static_cast<std::size_t>(victim)].tracked.deaths;
			}
		}
	}

	// GiveExpForStreakKills() is called with the *victim* (plugin.cpp:1058): the dying player's
	// streak is reset and - with lr_db_savedataplayer_mode - the row is saved.
	StreakBody(victim);
}

void Bench::StreakBody(int slot) {
	// GiveExpForStreakKills() of plugin.cpp:923.
	PlayerInfo* info = InfoOf(slot);
	if (info == nullptr) {
		return;
	}
	if (info->kill_streak > 1) {
		const int index = LrCore::StreakBonusIndex(info->kill_streak);
		if (index >= 0 && index < levels_ranks::kBonusCount) {
			++m_streak_bonuses;
			(void)GiveExp(slot, m_settings.bonus[static_cast<std::size_t>(index)],
						  kStreakPhrases[static_cast<std::size_t>(index)]);
		}
	}
	info = InfoOf(slot);
	if (info != nullptr) {
		info->kill_streak = 0;
	}
	if (m_settings.Value(levels_ranks::LR_DB_SaveDataPlayer_Mode) != 0) {
		++m_streak_saves;
		m_core->SavePlayer(slot);
	}
}

void Bench::RoundEndBody(int winner) {
	// OnRoundEvent(), "round_end" (plugin.cpp:1123). Only players with a pawn are credited, exactly
	// like the plugin (a dead player gets no round exp and no streak reset).
	if (winner > 1) {
		for (int slot = 0; slot < kSlots; ++slot) {
			if (!IsAlive(slot)) {
				continue;
			}
			const int team = m_tracks[static_cast<std::size_t>(slot)].team;
			if (team > 1) {
				const bool lose = team != winner;
				const ExpResult result =
					lose ? GiveExp(slot, -m_settings.Stats(levels_ranks::LR_ExpRoundLose), "RoundLose")
						 : GiveExp(slot, m_settings.Stats(levels_ranks::LR_ExpRoundWin), "RoundWin");
				if (result.status) {
					if (PlayerInfo* info = InfoOf(slot); info != nullptr) {
						info->stats[levels_ranks::ST_ROUNDSWIN + (lose ? 1 : 0)]++;
						info->session[levels_ranks::ST_ROUNDSWIN + (lose ? 1 : 0)]++;
						if (lose) {
							++m_tracks[static_cast<std::size_t>(slot)].tracked.loses;
						} else {
							++m_tracks[static_cast<std::size_t>(slot)].tracked.wins;
						}
					}
				}
			}
			StreakBody(slot);
		}
	}
	if (m_settings.Value(levels_ranks::LR_GiveExpRoundEnd) == 0) {
		// The plugin defers this to the next frame (NextFrame); the shipped config has 1, so the
		// statistics gate stays open for the next round either way.
		m_core->SetAllowStatistic(false);
	}
}

void Bench::DoShot(int slot) {
	PlayerTrack& player = m_tracks[static_cast<std::size_t>(slot)];
	if (player.slot < 0) {
		return;
	}
	if (!player.connected) {
		player.shot_running = false; // the chain ends with the disconnect; Connect() starts a new one
		return;
	}
	if (player.alive && m_round_active) {
		Measure("weapon_fire", [this, slot] { FireBody(slot); });
		if (Bernoulli(0.2)) {
			// A share of the shots hits an enemy: player_hurt of the plugin (the same attacker).
			const int enemy_team = player.team == 2 ? 3 : 2;
			std::vector<int> enemies = AliveSlots(enemy_team);
			if (!enemies.empty()) {
				const int victim = PickSlot(enemies);
				Measure("player_hurt", [this, victim, slot] { HurtBody(victim, slot); });
			}
		}
		player.next_shot = m_sim_now + Uniform(1.5, 3.5);
	} else {
		player.next_shot = m_sim_now + Uniform(1.0, 3.0);
	}
	Schedule(EventKind::Shot, player.next_shot, slot, 0);
}

void Bench::FireBody(int slot) {
	// OnOtherEvent(), weapon_fire (plugin.cpp:1203).
	if (!m_core->CountPlayersAllowed()) {
		return;
	}
	if (PlayerInfo* info = InfoOf(slot); info != nullptr && info->initialized) {
		info->stats[levels_ranks::ST_SHOOTS]++;
		info->session[levels_ranks::ST_SHOOTS]++;
		++m_tracks[static_cast<std::size_t>(slot)].tracked.shots;
	}
}

void Bench::HurtBody(int victim, int attacker) {
	// OnOtherEvent(), player_hurt (plugin.cpp:1216); the attacker is credited.
	(void)victim;
	if (!m_core->CountPlayersAllowed()) {
		return;
	}
	if (PlayerInfo* info = InfoOf(attacker); info != nullptr && info->initialized) {
		info->stats[levels_ranks::ST_HITS]++;
		info->session[levels_ranks::ST_HITS]++;
		++m_tracks[static_cast<std::size_t>(attacker)].tracked.hits;
	}
}

void Bench::BombBody(std::string_view name, int slot) {
	// OnBombEvent() of plugin.cpp:1061 (the pawn check is the bench's alive state).
	if (name == "bomb_defused") {
		(void)GiveExp(slot, m_settings.Stats(levels_ranks::LR_ExpBombDefused), "BombDefused");
		return;
	}
	PlayerInfo* info = InfoOf(slot);
	if (info == nullptr) {
		return;
	}
	if (name == "bomb_planted") {
		if (GiveExp(slot, m_settings.Stats(levels_ranks::LR_ExpBombPlanted), "BombPlanted").status) {
			info->have_bomb = false;
		}
		return;
	}
	if (name == "bomb_dropped") {
		if (info->have_bomb &&
			GiveExp(slot, -m_settings.Stats(levels_ranks::LR_ExpBombDropped), "BombDropped").status) {
			info->have_bomb = false;
		}
		return;
	}
	if (!info->have_bomb && GiveExp(slot, m_settings.Stats(levels_ranks::LR_ExpBombPickup), "BombPickup").status) {
		info->have_bomb = true;
	}
}

void Bench::BombPickup(int slot, int round) {
	if (!m_round_active || !IsAlive(slot)) {
		return;
	}
	Measure("bomb_pickup", [this, slot] { BombBody("bomb_pickup", slot); });
	if (Bernoulli(0.15)) {
		Schedule(EventKind::BombDrop, m_sim_now + Uniform(3.0, 10.0), slot, round);
	} else {
		Schedule(EventKind::BombPlant, std::min(m_round_end_time - 5.0, m_sim_now + Uniform(5.0, 25.0)), slot, round);
	}
}

void Bench::BombDrop(int slot, int round) {
	if (!m_round_active || !IsAlive(slot)) {
		return;
	}
	Measure("bomb_dropped", [this, slot] { BombBody("bomb_dropped", slot); });
	// The same carrier retrieves the bomb shortly after.
	Schedule(EventKind::BombPickup, m_sim_now + Uniform(2.0, 6.0), slot, round);
}

void Bench::BombPlant(int slot, int round) {
	if (!m_round_active || !IsAlive(slot)) {
		return;
	}
	Measure("bomb_planted", [this, slot] { BombBody("bomb_planted", slot); });
	// The bomb decides the round: about half of the plants are defused, the rest go off (and the
	// round ends with the explosion instead of the round timer).
	const double defuse_time = m_sim_now + Uniform(8.0, 25.0);
	const std::vector<int> cts = AliveSlots(3);
	if (Bernoulli(0.5) && !cts.empty() && defuse_time < m_round_end_time - 3.0) {
		Schedule(EventKind::BombDefuse, defuse_time, PickSlot(cts), round);
		return;
	}
	m_planned_winner = 2;
	m_round_end_time = std::min(m_round_end_time, m_sim_now + Uniform(35.0, 40.0));
	Schedule(EventKind::RoundEnd, m_round_end_time, -1, round);
}

void Bench::BombDefuse(int slot, int round) {
	if (!m_round_active || !IsAlive(slot)) {
		// The would-be defuser is gone: the bomb goes off instead.
		if (m_round_active) {
			m_planned_winner = 2;
			m_round_end_time = std::min(m_round_end_time, m_sim_now + Uniform(35.0, 40.0));
			Schedule(EventKind::RoundEnd, m_round_end_time, -1, round);
		}
		return;
	}
	Measure("bomb_defused", [this, slot] { BombBody("bomb_defused", slot); });
	m_planned_winner = 3;
	// The round ends shortly after a defuse; the round_end scheduled at the original time carries
	// the same round id and finds the round already closed.
	m_round_end_time = m_sim_now + 3.0;
	Schedule(EventKind::RoundEnd, m_round_end_time, -1, round);
}

void Bench::MenuTop() {
	// The top list of the menu / !top command (menus.cpp:307): every 8..20 s of game time.
	std::vector<int> connected;
	for (const PlayerTrack& player : m_tracks) {
		if (player.slot >= 0 && player.connected) {
			connected.push_back(player.slot);
		}
	}
	const bool by_playtime = Bernoulli(0.25);
	if (!connected.empty()) {
		Measure("menu_top (LoadTopPlayers)", [this, by_playtime] {
			m_core->LoadTopPlayers(by_playtime, [this](std::vector<levels_ranks::TopRow> rows) {
				++m_menu_tops;
				m_menu_rows += rows.size();
			});
		});
	}
	Schedule(EventKind::MenuTop, m_sim_now + Uniform(8.0, 20.0), -1, 0);
}

// --- helpers --------------------------------------------------------------------------------------

ExpResult Bench::GiveExp(int slot, int value, const char* phrase) {
	++m_give_exp_calls;
	const ExpResult result = m_core->GiveExp(slot, value, phrase);
	if (result.saved) {
		++m_rank_change_saves;
	}
	if (result.applied) {
		m_tracks[static_cast<std::size_t>(slot)].tracked.exp = result.new_exp;
	}
	return result;
}

std::vector<int> Bench::AliveSlots(int team) const {
	std::vector<int> slots;
	for (const PlayerTrack& player : m_tracks) {
		if (player.slot >= 0 && player.team == team && IsAlive(player.slot)) {
			slots.push_back(player.slot);
		}
	}
	return slots;
}

// --- reporting ------------------------------------------------------------------------------------

void Bench::PrintHeader() const {
	std::printf("levels_ranks lr_bench — engine-free load/consistency bench for the Levels Ranks core\n");
	std::printf("settings:  %s (lr_type_statistics=%d, lr_db_savedataplayer_mode=%d, lr_minplayers_count=%d,\n",
				m_settings_path.c_str(), m_settings.type_statistics, m_settings.Value(levels_ranks::LR_DB_SaveDataPlayer_Mode),
				m_settings.Value(levels_ranks::LR_MinplayersCount));
	std::printf("           lr_start_points=%d, lr_kill=%d, lr_death=%d, lr_winround=%d, lr_loseround=%d, ranks=%zu)\n",
				m_settings.Value(levels_ranks::LR_StartPoints), m_settings.Stats(levels_ranks::LR_ExpKill),
				m_settings.Stats(levels_ranks::LR_ExpDeath), m_settings.Stats(levels_ranks::LR_ExpRoundWin),
				m_settings.Stats(levels_ranks::LR_ExpRoundLose), m_settings.rank_exp.size());
	if (m_mysql) {
		// No user/password in the output.
		std::printf("database:  mysql %s:%u/%s\n", m_config.host.c_str(), m_config.port, m_config.database.c_str());
	} else {
		std::printf("database:  sqlite %s\n", m_config.sqlitePath.c_str());
	}
	std::printf("table:     %s (dropped and created by the bench; nothing else is touched)\n", Table().c_str());
	std::printf("players:   %d (slots 0..%d, steamids %llu..%llu, names bench_0..bench_%d, teams 2/3)\n",
				kBenchPlayers, kBenchPlayers - 1, static_cast<unsigned long long>(kSteamBase),
				static_cast<unsigned long long>(kSteamBase + kBenchPlayers - 1), kBenchPlayers - 1);
	std::printf("match:     %d rounds x ~%.0f s of game time, %.0f fps, speed %.1fx, seed %llu\n",
				m_options.rounds, m_options.round_seconds, kFrameRate, m_options.speed,
				static_cast<unsigned long long>(m_options.seed));
	std::printf("hooks:     counted (the plugin's chat/menu fan-out is engine work and is not measured)\n");
}

void Bench::PrintMatchSummary() const {
	long long kills = 0;
	long long death_count = 0;
	long long headshots = 0;
	long long assists = 0;
	long long shots = 0;
	long long hits = 0;
	long long wins = 0;
	long long loses = 0;
	for (const PlayerTrack& player : m_tracks) {
		if (player.slot < 0) {
			continue;
		}
		kills += player.tracked.kills;
		death_count += player.tracked.deaths;
		headshots += player.tracked.headshots;
		assists += player.tracked.assists;
		shots += player.tracked.shots;
		hits += player.tracked.hits;
		wins += player.tracked.wins;
		loses += player.tracked.loses;
	}
	const double real_match = std::chrono::duration<double>(m_real_match_end - m_real_match_start).count();
	const double real_total = std::chrono::duration<double>(Clock::now() - m_real_start).count();
	std::printf("\n== match\n");
	std::printf("game time: %.1f s simulated (%.1f min), %d rounds of %.0f s nominal\n", m_sim_now, m_sim_now / 60.0,
				m_options.rounds, m_options.round_seconds);
	std::printf("real time: %.2f s match + %.2f s setup (the schema alone blocked %.2f ms) -> %.1fx compression\n",
				real_match, real_total - real_match, m_schema_us / 1000.0,
				real_match > 0.0 ? m_sim_now / real_match : 0.0);
	std::printf("events:    %lld kills, %lld deaths, %lld headshots, %lld assists, %lld shots, %lld hits, "
				"%lld round wins, %lld round losses\n",
				kills, death_count, headshots, assists, shots, hits, wins, loses);
	std::printf("frames:    %llu simulated (%.1f/s of game time), %llu with database work queued\n",
				static_cast<unsigned long long>(m_frames), m_frames / m_sim_now,
				static_cast<unsigned long long>(m_frames_with_queue));
}

void Bench::PrintEventTable() const {
	static const char* const kOrder[] = {
		"load_or_create (auth/reconnect)",
		"player_disconnect (save+online=0)",
		"final_disconnect (save+online=0)",
		"second_tick (reconcile+gate)",
		"round_start",
		"round_end (win/lose exp)",
		"round_mvp",
		"player_death (assist/kill/death/headshot/streak)",
		"weapon_fire",
		"player_hurt",
		"bomb_pickup",
		"bomb_dropped",
		"bomb_planted",
		"bomb_defused",
		"menu_top (LoadTopPlayers)",
	};
	std::printf("\n== game-thread event times (us)\n");
	std::printf("%-48s %8s %10s %10s %10s\n", "event (whole handler body on the game thread)", "count", "avg", "p99", "max");
	std::vector<const std::pair<std::string, EventStats>*> printed;
	for (const char* name : kOrder) {
		for (const auto& entry : m_stats) {
			if (entry.first == name) {
				printed.push_back(&entry);
				std::printf("%-48s %8llu %10.2f %10.2f %10.2f\n", entry.first.c_str(),
							static_cast<unsigned long long>(entry.second.count), entry.second.AverageUs(),
							entry.second.P99Us(), entry.second.max_us);
			}
		}
	}
	for (const auto& entry : m_stats) {
		if (std::find_if(printed.begin(), printed.end(), [&entry](const auto* p) { return p->first == entry.first; }) ==
			printed.end()) {
			std::printf("%-48s %8llu %10.2f %10.2f %10.2f\n", entry.first.c_str(),
						static_cast<unsigned long long>(entry.second.count), entry.second.AverageUs(),
						entry.second.P99Us(), entry.second.max_us);
		}
	}
}

void Bench::PrintHistory() const {
	long long kills = 0;
	for (const PlayerTrack& player : m_tracks) {
		if (player.slot >= 0) {
			kills += player.tracked.kills;
		}
	}
	const std::uint64_t saves = m_streak_saves + m_rank_change_saves + m_disconnects;
	std::printf("\n== history (bench counters, plugin.cpp semantics)\n");
	std::printf("kills (total):            %lld\n", kills);
	std::printf("GiveExp calls:            %llu (assist/kill/death/headshot/streak/round/mvp/bomb)\n",
				static_cast<unsigned long long>(m_give_exp_calls));
	std::printf("SavePlayer calls:         %llu = %llu streak resets (per death, mode 1) + %llu rank changes + %llu disconnects\n",
				static_cast<unsigned long long>(saves), static_cast<unsigned long long>(m_streak_saves),
				static_cast<unsigned long long>(m_rank_change_saves), static_cast<unsigned long long>(m_disconnects));
	std::printf("rank changes:             %llu (level messages sent to the hooks: %llu)\n",
				static_cast<unsigned long long>(m_rank_changes), static_cast<unsigned long long>(m_level_messages));
	std::printf("exp messages:             %llu, streak bonuses: %llu\n",
				static_cast<unsigned long long>(m_exp_messages), static_cast<unsigned long long>(m_streak_bonuses));
	std::printf("player loads:             %llu (connects %llu, disconnects %llu)\n",
				static_cast<unsigned long long>(m_player_loads), static_cast<unsigned long long>(m_connects),
				static_cast<unsigned long long>(m_disconnects));
	std::printf("loads issued while the previous load was still unanswered (kLoadRetrySeconds=%lld): %llu\n",
				static_cast<long long>(kLoadRetrySeconds), static_cast<unsigned long long>(m_retry_loads));
	std::printf("  the fixed reconcile left an unanswered load alone on %llu tick(s) (no re-issue, no slot reset)\n",
				static_cast<unsigned long long>(m_retry_load_skips));
	long long reloaded_away_total = m_reloaded_away.exp;
	for (const CounterField& field : kTrackedFields) {
		reloaded_away_total += m_reloaded_away.*(field.field);
	}
	if (reloaded_away_total != 0) {
		std::printf("  of those reloads replaced a playing slot and discarded (counted since the last save):");
		for (const CounterField& field : kTrackedFields) {
			if (const long long lost = m_reloaded_away.*(field.field); lost != 0) {
				std::printf(" %s+%lld", field.column, lost);
			}
		}
		if (m_reloaded_away.exp != 0) {
			std::printf(" value+%lld", m_reloaded_away.exp);
		}
		std::printf("\n");
	}
	std::printf("position reads:           %llu (onPlayerPosInTop), menu tops: %llu covering %llu rows\n",
				static_cast<unsigned long long>(m_pos_in_top), static_cast<unsigned long long>(m_menu_tops),
				static_cast<unsigned long long>(m_menu_rows));
	std::printf("errors from the core:     %zu\n", m_errors.size());
	for (const std::string& error : m_errors) {
		std::printf("  error: %s\n", error.c_str());
	}
}

void Bench::PrintDatabase(const CountingDatabase::Snapshot& db, double seconds, std::uint64_t match_statements) const {
	long long kills = 0;
	for (const PlayerTrack& player : m_tracks) {
		if (player.slot >= 0) {
			kills += player.tracked.kills;
		}
	}
	std::printf("\n== database (plgdb, one worker thread, one connection)\n");
	std::printf("statements sent:          %llu (async %llu, sync %llu, transactions %llu)\n",
				static_cast<unsigned long long>(db.Statements()),
				static_cast<unsigned long long>(db.async_statements),
				static_cast<unsigned long long>(db.sync_statements),
				static_cast<unsigned long long>(db.transactions));
	std::printf("statements/s:             %.1f (over %.2f s of match)\n",
				seconds > 0.0 ? static_cast<double>(db.Statements()) / seconds : 0.0, seconds);
	std::printf("statements per kill:      %.2f (match phase: %llu statements / %lld kills)\n",
				kills > 0 ? static_cast<double>(match_statements) / static_cast<double>(kills) : 0.0,
				static_cast<unsigned long long>(match_statements), kills);
	std::printf("queue length max:         %zu (frames with a non-empty queue: %llu)\n", m_queue_max,
				static_cast<unsigned long long>(m_frames_with_queue));
	std::printf("callback latency:         avg %.1f us, p99 %.1f us, max %.1f us (n=%llu; call to callback in Poll)\n",
				db.LatencyAverageUs(), db.LatencyP99Us(), db.latency_max_us,
				static_cast<unsigned long long>(db.callbacks));
	std::printf("raw round trip (SELECT 1): avg %.1f us / min %.1f us before, avg %.1f us / min %.1f us after\n",
				m_round_trip_start_us, m_round_trip_start_min_us, m_round_trip_end_us, m_round_trip_end_min_us);
	std::printf("sync (blocking) time:     %.2f ms total (schema + DROP + the reads of this bench)\n",
				db.sync_blocking_us / 1000.0);
}

// --- the consistency check ------------------------------------------------------------------------

int Bench::CheckConsistency() {
	std::printf("\n== consistency: the final rows vs the counters the bench tracked itself\n");
	int failures = 0;
	int rows_found = 0;
	for (const PlayerTrack& player : m_tracks) {
		if (player.slot < 0) {
			continue;
		}
		const plgdb::Result row = m_db->QuerySync(
			"SELECT `name`, `value`, `rank`, `kills`, `deaths`, `shoots`, `hits`, `headshots`, `assists`, "
			"`round_win`, `round_lose`, `online` FROM " + Table() + " WHERE `steam` = ?;",
			{player.steamid});
		if (!row.Ok()) {
			std::printf("  [FAIL] %-14s (%s): the readback failed: %s\n", player.name.c_str(), player.steamid.c_str(),
						row.Error().c_str());
			++failures;
			continue;
		}
		if (row.RowCount() == 0) {
			std::printf("  [FAIL] %-14s (%s): no row in the table\n", player.name.c_str(), player.steamid.c_str());
			++failures;
			continue;
		}
		++rows_found;
		const std::string name = row.GetString(0, 0);
		const long long value = row.GetInt(0, 1);
		const long long rank = row.GetInt(0, 2);
		const long long kills = row.GetInt(0, 3);
		const long long deaths = row.GetInt(0, 4);
		const long long shoots = row.GetInt(0, 5);
		const long long hits = row.GetInt(0, 6);
		const long long headshots = row.GetInt(0, 7);
		const long long assists = row.GetInt(0, 8);
		const long long wins = row.GetInt(0, 9);
		const long long loses = row.GetInt(0, 10);
		const long long online = row.GetInt(0, 11);
		const long long expected_rank = LrCore::RankForExp(static_cast<int>(player.tracked.exp), m_settings.rank_exp);

		std::vector<std::string> mismatches;
		if (value != player.tracked.exp) {
			mismatches.push_back("value db=" + std::to_string(value) + " bench=" + std::to_string(player.tracked.exp));
		}
		if (rank != expected_rank) {
			mismatches.push_back("rank db=" + std::to_string(rank) + " bench=" + std::to_string(expected_rank));
		}
		if (kills != player.tracked.kills) {
			mismatches.push_back("kills db=" + std::to_string(kills) + " bench=" + std::to_string(player.tracked.kills));
		}
		if (deaths != player.tracked.deaths) {
			mismatches.push_back("deaths db=" + std::to_string(deaths) + " bench=" + std::to_string(player.tracked.deaths));
		}
		if (shoots != player.tracked.shots) {
			mismatches.push_back("shoots db=" + std::to_string(shoots) + " bench=" + std::to_string(player.tracked.shots));
		}
		if (hits != player.tracked.hits) {
			mismatches.push_back("hits db=" + std::to_string(hits) + " bench=" + std::to_string(player.tracked.hits));
		}
		if (headshots != player.tracked.headshots) {
			mismatches.push_back("headshots db=" + std::to_string(headshots) +
								 " bench=" + std::to_string(player.tracked.headshots));
		}
		if (assists != player.tracked.assists) {
			mismatches.push_back("assists db=" + std::to_string(assists) + " bench=" + std::to_string(player.tracked.assists));
		}
		if (wins != player.tracked.wins) {
			mismatches.push_back("round_win db=" + std::to_string(wins) + " bench=" + std::to_string(player.tracked.wins));
		}
		if (loses != player.tracked.loses) {
			mismatches.push_back("round_lose db=" + std::to_string(loses) + " bench=" + std::to_string(player.tracked.loses));
		}
		if (name != player.name) {
			mismatches.push_back("name db=\"" + name + "\" bench=\"" + player.name + "\"");
		}
		if (online != 0) {
			mismatches.push_back("online db=" + std::to_string(online) + " bench=0 (the final save clears it)");
		}

		if (mismatches.empty()) {
			std::printf("  [OK]   %-14s (%s): value=%lld rank=%lld kills=%lld deaths=%lld headshots=%lld assists=%lld "
						"shoots=%lld hits=%lld wins=%lld loses=%lld\n",
						player.name.c_str(), player.steamid.c_str(), value, rank, kills, deaths, headshots, assists,
						shoots, hits, wins, loses);
		} else {
			++failures;
			std::printf("  [FAIL] %-14s (%s):", player.name.c_str(), player.steamid.c_str());
			for (const std::string& mismatch : mismatches) {
				std::printf(" %s |", mismatch.c_str());
			}
			std::printf("\n");
		}
	}
	std::printf("%d/%d players read back; %s\n", rows_found, kBenchPlayers,
				failures == 0 ? "every row matches the tracked counters — OK"
							  : "MISMATCHES ABOVE — the rows do not match the tracked counters");
	return failures;
}

int Bench::Run() {
	m_real_start = Clock::now();
	const std::uint64_t seed =
		m_options.seed != 0 ? m_options.seed
							: (static_cast<std::uint64_t>(std::time(nullptr)) ^
							   (static_cast<std::uint64_t>(::getpid()) << 16));
	m_rng.seed(seed);
	m_options.seed = seed;

	if (!Setup()) {
		return 2;
	}
	PrintHeader();

	Match();

	const CountingDatabase::Snapshot db = m_db->Take();
	const double real_match_seconds = std::chrono::duration<double>(m_real_match_end - m_real_match_start).count();

	PrintMatchSummary();
	// The raw round trip after the match (the snapshot above already excludes it).
	m_round_trip_end_us = ProbeRoundTripUs(5, m_round_trip_end_min_us);
	PrintEventTable();
	PrintHistory();

	// The statements of the match phase (the setup statements are excluded through the snapshot
	// taken before the joins; see Match()).
	PrintDatabase(db, real_match_seconds, db.Statements() - m_statements_at_match_start);

	const int failures = CheckConsistency();
	std::printf("\nresult: %s (seed %llu, %s)\n", failures == 0 ? "OK" : "FAILURES",
				static_cast<unsigned long long>(seed), m_mysql ? "mysql" : "sqlite");

	if (failures == 0) {
		// The bench owns exactly this table; a green run leaves the shared test database as it was.
		const plgdb::Result dropped = m_db->QuerySync("DROP TABLE IF EXISTS " + Table() + ";", {});
		std::printf("the bench dropped %s again (%s)\n", Table().c_str(), dropped.Ok() ? "ok" : dropped.Error().c_str());
	} else {
		std::printf("the bench left %s in place for inspection\n", Table().c_str());
	}
	return failures == 0 ? 0 : 1;
}

// --- the command line -----------------------------------------------------------------------------

Options ParseOptions(int argc, char** argv) {
	Options options;
	options.seed = static_cast<std::uint64_t>(std::time(nullptr)) ^ (static_cast<std::uint64_t>(::getpid()) << 16);
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		const auto value = [&argc, &argv, &i, &arg]() -> std::string {
			if (i + 1 >= argc) {
				std::printf("lr_bench: %s needs a value\n", arg.c_str());
				std::exit(2);
			}
			return argv[++i];
		};
		if (arg == "--speed") {
			options.speed = std::atof(value().c_str()); // 0 = run flat out
		} else if (arg == "--rounds") {
			options.rounds = std::max(1, std::atoi(value().c_str()));
		} else if (arg == "--seed") {
			options.seed = std::strtoull(value().c_str(), nullptr, 10);
		} else if (arg == "--help" || arg == "-h") {
			options.help = true;
		} else {
			std::printf("lr_bench: unknown option %s (--help)\n", arg.c_str());
			std::exit(2);
		}
	}
	if (options.speed < 0.0) {
		options.speed = 0.0;
	}
	return options;
}

bool MySqlConfigFromEnv(plgdb::Config& config, std::string& error) {
	const char* host = std::getenv("MYSQL_HOST");
	if (host == nullptr || *host == '\0') {
		return false;
	}
	config.driver = plgdb::Driver::MySQL;
	config.host = host;
	config.user = std::getenv("MYSQL_USER") != nullptr ? std::getenv("MYSQL_USER") : "";
	config.password = std::getenv("MYSQL_PASSWORD") != nullptr ? std::getenv("MYSQL_PASSWORD") : "";
	config.database = std::getenv("MYSQL_DATABASE") != nullptr ? std::getenv("MYSQL_DATABASE") : "";
	if (const char* port = std::getenv("MYSQL_PORT"); port != nullptr && *port != '\0') {
		try {
			const int parsed = std::stoi(port);
			if (parsed < 1 || parsed > 65535) {
				error = "MYSQL_PORT out of range";
				return false;
			}
			config.port = static_cast<uint16_t>(parsed);
		} catch (const std::exception&) {
			error = "MYSQL_PORT is not a number";
			return false;
		}
	}
	return true;
}

void PrintUsage() {
	std::printf("usage: lr_bench [--rounds N] [--speed X] [--seed N]\n"
				"  --rounds N  rounds of the simulated match (default 30)\n"
				"  --speed X   game seconds per real second (default 64)\n"
				"              --speed 0 runs unpaced: a stress mode where the game thread outruns the\n"
				"              database (events then hit players whose rows are still loading)\n"
				"  --seed N    seed of the simulation (default: time ^ pid)\n"
				"  MySQL when MYSQL_HOST is set (MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE),\n"
				"  SQLite \":memory:\" otherwise. Only the bench's own table (lr_bench_<pid>) is used.\n"
				"exit code: 0 = all rows consistent, 1 = a row mismatched, 2 = the database failed.\n");
}

} // namespace

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const Options options = ParseOptions(argc, argv);
	if (options.help) {
		PrintUsage();
		return 0;
	}

	// The settings of the shipped settings.ini, through the plugin's own parser (the same path as
	// levels_ranks/LoadSettingsFile); lr_db_savedataplayer_mode is the shipped 1.
	const std::string settings_path = std::string(LR_TEST_DATA_DIR) + "/settings.ini";
	pisex::KeyValues kv("LR_Settings");
	if (!kv.LoadFromFile(settings_path)) {
		std::printf("FATAL: cannot read %s\n", settings_path.c_str());
		return 2;
	}
	std::string problem;
	Settings settings = levels_ranks::LoadSettings(levels_ranks::KeyValuesView(kv), &problem);
	if (!problem.empty()) {
		std::printf("FATAL: %s: %s\n", settings_path.c_str(), problem.c_str());
		return 2;
	}
	// The bench's own table (recreated per run); the plugin's lvl_base is never touched.
	settings.table_name = "lr_bench_" + std::to_string(static_cast<long>(::getpid()));

	plgdb::Config config;
	std::string error;
	const bool mysql = MySqlConfigFromEnv(config, error);
	if (!error.empty()) {
		std::printf("FATAL: %s\n", error.c_str());
		return 2;
	}
	if (!mysql) {
		config.driver = plgdb::Driver::SQLite;
		config.sqlitePath = ":memory:";
	}

	Bench bench(options, settings, config, mysql, settings_path);
	return bench.Run();
}
