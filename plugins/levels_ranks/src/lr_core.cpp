// Engine-independent core of the levels_ranks plugin; see lr_core.hpp for the compatibility
// contract with ref/src/cs2-lvl_ranks/levels_ranks.cpp. Every comment that starts with "PARITY:"
// marks either a deviation or a quirk of the original that is preserved on purpose.

#include "lr_core.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>

namespace levels_ranks {

namespace {

constexpr int kMinKillCoefficientPercent = 50;   // 0.5 (lr_killcoeff lower clamp)
constexpr int kMaxKillCoefficientPercent = 150;  // 1.5 (lr_killcoeff upper clamp)

// A plain SQL identifier: the table name is interpolated, never bound.
bool IsIdentifier(std::string_view text) {
	if (text.empty() || text.size() > 32) {
		return false;
	}
	for (const char c : text) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		if (!ok) {
			return false;
		}
	}
	return true;
}

// `value` / `playtime / 3600.0` of OverAllTopPlayers: the ORDER BY expression of the top list.
std::string TopOrderExpression(bool by_playtime) {
	return by_playtime ? "`playtime` / 3600.0" : "`value`";
}

} // namespace

// --- settings ---------------------------------------------------------------------------------

bool Settings::HasValidTableName() const {
	return IsIdentifier(table_name);
}

Settings LoadSettings(const ConfigView& view, std::string* error) {
	Settings settings;
	std::string problem;

	const auto note = [&problem](std::string text) {
		if (problem.empty()) {
			problem = std::move(text);
		}
	};

	// --- MainSettings ---------------------------------------------------------------------
	// The defaults are applied even when the block is missing: the original logged the missing
	// block and unloaded itself, the port logs and keeps rating with the documented defaults of
	// each key (a Failed OnPluginStart would stop *every* Plugify plugin).
	constexpr std::string_view kMain = "MainSettings";
	if (!view.HasSection(kMain)) {
		note("Not found MainSettings in levels ranks config");
	}
	settings.table_name = view.GetString(kMain, "lr_table", "lvl_base");
	settings.plugin_title = view.GetString(kMain, "lr_plugin_title", "");
	settings.type_statistics = view.GetInt(kMain, "lr_type_statistics", 0);
	settings.values[LR_TypeStatistics] = settings.type_statistics;
	settings.values[LR_DB_Allow_UTF8MB4] = view.GetInt(kMain, "lr_db_allow_utf8mb4", 1);
	settings.values[LR_MinplayersCount] = view.GetInt(kMain, "lr_minplayers_count", 4);
	settings.values[LR_ShowResetMyStats] = view.GetInt(kMain, "lr_show_resetmystats", 1);
	// PARITY: the original read the cooldown off the *root* node
	// (pKVConfig->GetInt("lr_resetmystats_cooldown", 86400)), where a key nested inside
	// MainSettings is not found - so the shipped value of the file was ignored and the cooldown
	// was always the default. Kept: section "" is the root.
	settings.values[LR_ResetMyStatsCooldown] = view.GetInt("", "lr_resetmystats_cooldown", 86400);
	settings.values[LR_ShowUsualMessage] = view.GetInt(kMain, "lr_show_usualmessage", 1);
	settings.values[LR_ShowSpawnMessage] = view.GetInt(kMain, "lr_show_spawnmessage", 1);
	settings.values[LR_ShowAllLevelUpMessage] = view.GetInt(kMain, "lr_show_all_levelup_message", 0);
	settings.values[LR_ShowAllLevelDownMessage] = view.GetInt(kMain, "lr_show_all_leveldown_message", 0);
	settings.values[LR_ShowRankMessage] = view.GetInt(kMain, "lr_show_rankmessage", 1);
	settings.values[LR_ShowRankList] = view.GetInt(kMain, "lr_show_ranklist", 1);
	settings.values[LR_GiveExpRoundEnd] = view.GetInt(kMain, "lr_giveexp_roundend", 1);
	settings.values[LR_BlockWarmup] = view.GetInt(kMain, "lr_block_warmup", 1);
	settings.values[LR_AllAgainstAll] = view.GetInt(kMain, "lr_allagainst_all", 0);
	settings.values[LR_CleanDB_Days] = view.GetInt(kMain, "lr_cleandb_days", 30);
	settings.values[LR_CleanDB_BanClient] = view.GetInt(kMain, "lr_cleandb_banclient", 1);
	settings.values[LR_DB_SaveDataPlayer_Mode] = view.GetInt(kMain, "lr_db_savedataplayer_mode", 1);
	settings.values[LR_DB_Charset_Type] = view.GetInt(kMain, "lr_db_character_type", 0);
	settings.values[LR_TopCount] = view.GetInt(kMain, "lr_top_count", 0);
	settings.values[LR_StartPoints] = view.GetInt(kMain, "lr_start_points", 0);
	settings.values[LR_OnlineID] = view.GetInt(kMain, "lr_online_id", 1);
	// PARITY: the core's own macro block names these two slots 6 and 7, i.e. the same slots as
	// LR_ShowAllLevelUp/DownMessage above: the later read wins, so the "all" keys of the file are
	// ignored and lr_show_levelup_message drives the personal *and* the broadcast message.
	// LR_ShowLevelUpMessage (22) / LR_ShowLevelDownMessage (23) stay 0, exactly as in the
	// original (where they were never written either).
	settings.values[kShowLevelUp] = view.GetInt(kMain, "lr_show_levelup_message", 0);
	settings.values[kShowLevelDown] = view.GetInt(kMain, "lr_show_leveldown_message", 0);

	// --- the statistics block of the selected mode -----------------------------------------
	std::string stats_section;
	switch (settings.type_statistics) {
		case 0: stats_section = "Funded_System"; break;
		case 1: stats_section = "Rating_Extended"; break;
		case 2: stats_section = "Rating_Simple"; break;
		default: stats_section = "Funded_System"; break;
	}

	if (!view.HasSection(stats_section)) {
		note("Not found " + stats_section + " in levels ranks config");
	}
	if (settings.type_statistics == 0) {
		settings.stats[LR_ExpKill] = view.GetInt(stats_section, "lr_kill", 0);
		settings.stats[LR_ExpKillIsBot] = view.GetInt(stats_section, "lr_kill_is_bot", 0);
		settings.stats[LR_ExpDeath] = view.GetInt(stats_section, "lr_death", 0);
		settings.stats[LR_ExpDeathIsBot] = view.GetInt(stats_section, "lr_death_is_bot", 0);
	} else if (settings.type_statistics == 1) {
		float coefficient = view.GetFloat(stats_section, "lr_killcoeff", 1.0f);
		if (coefficient < 0.5f) {
			coefficient = 0.5f;
		} else if (coefficient > 1.5f) {
			coefficient = 1.5f;
		}
		settings.stats[LR_ExpKillCoefficient] = static_cast<int>(coefficient * 100);
		settings.stats[LR_ExpKillCoefficient] =
			std::clamp(settings.stats[LR_ExpKillCoefficient], kMinKillCoefficientPercent, kMaxKillCoefficientPercent);
	}

	settings.stats[LR_ExpGiveHeadShot] = view.GetInt(stats_section, "lr_headshot", 1);
	settings.stats[LR_ExpGiveAssist] = view.GetInt(stats_section, "lr_assist", 1);
	settings.stats[LR_ExpGiveSuicide] = view.GetInt(stats_section, "lr_suicide", 0);
	settings.stats[LR_ExpGiveTeamKill] = view.GetInt(stats_section, "lr_teamkill", 0);
	settings.stats[LR_ExpRoundWin] = view.GetInt(stats_section, "lr_winround", 2);
	settings.stats[LR_ExpRoundLose] = view.GetInt(stats_section, "lr_loseround", 2);
	settings.stats[LR_ExpRoundMVP] = view.GetInt(stats_section, "lr_mvpround", 1);
	settings.stats[LR_ExpBombPlanted] = view.GetInt(stats_section, "lr_bombplanted", 2);
	settings.stats[LR_ExpBombDefused] = view.GetInt(stats_section, "lr_bombdefused", 2);
	settings.stats[LR_ExpBombDropped] = view.GetInt(stats_section, "lr_bombdropped", 1);
	settings.stats[LR_ExpBombPickup] = view.GetInt(stats_section, "lr_bombpickup", 1);
	settings.stats[LR_ExpHostageKilled] = view.GetInt(stats_section, "lr_hostagekilled", 0);
	settings.stats[LR_ExpHostageRescued] = view.GetInt(stats_section, "lr_hostagerescued", 2);

	// --- the streak bonuses (not used by the "Simple" rating mode) --------------------------
	if (settings.type_statistics != 2) {
		constexpr std::string_view kBonuses = "Special_Bonuses";
		if (!view.HasSection(kBonuses)) {
			note("Not found Special_Bonuses in levels ranks config");
		}
		for (std::size_t i = 0; i < settings.bonus.size(); ++i) {
			const std::string key = "lr_bonus_" + std::to_string(i + 1);
			settings.bonus[i] = view.GetInt(kBonuses, key, 0);
		}
	}

	// --- the ranks --------------------------------------------------------------------------
	if (!view.HasSection("Ranks")) {
		note("Not found Ranks in levels ranks config");
	} else {
		for (auto& [name, exp] : view.GetIntValues("Ranks")) {
			settings.rank_names.push_back(std::move(name));
			settings.rank_exp.push_back(exp);
		}
	}

	if (error != nullptr) {
		*error = problem;
	}
	return settings;
}

// --- pure logic --------------------------------------------------------------------------------

int LrCore::RankForExp(int exp, const std::vector<int>& rank_exp) {
	// CheckRank() of the original:
	//   int iRank = iMaxRanks + 1;
	//   while (--iRank && g_hRankExp[iRank - 1] > iExp) {}
	const int max_ranks = static_cast<int>(rank_exp.size());
	if (max_ranks == 0) {
		return 0;
	}
	int rank = max_ranks + 1;
	while (--rank && rank_exp[static_cast<std::size_t>(rank - 1)] > exp) {
	}
	return rank;
}

int LrCore::RoundToNearest(double value) {
	return static_cast<int>(std::round(value));
}

int LrCore::RoundToCeil(double value) {
	return static_cast<int>(std::ceil(value));
}

int LrCore::KillExpAttacker(int type, int attacker_exp, int victim_exp) {
	if (type == 1) {
		// PARITY: the original divided by the attacker's experience; an attacker at 0 threw a
		// division by zero (the result is undefined and would have been used as an experience
		// value). The port treats a non-positive attacker experience as a ratio of 1.
		const double ratio = attacker_exp > 0 ? static_cast<double>(victim_exp) / static_cast<double>(attacker_exp) : 1.0;
		return RoundToNearest(ratio * 5.0);
	}
	if (type == 2) {
		return victim_exp - attacker_exp;
	}
	return 0; // the "Funded" mode reads lr_kill / lr_kill_is_bot instead
}

int LrCore::RatingVictimExp(int attacker_gain, int coefficient_percent) {
	const int loss = RoundToNearest(static_cast<double>(attacker_gain) * (static_cast<double>(coefficient_percent) / 100.0));
	return loss < 1 ? 1 : loss;
}

int LrCore::SimpleModeExp(int raw) {
	return raw < 3 ? 2 : (raw / 100) + 2;
}

int LrCore::StreakBonusIndex(int kill_streak) {
	int index = kill_streak - 2;
	if (index > static_cast<int>(kBonusCount) - 1) {
		index = static_cast<int>(kBonusCount) - 1;
	}
	return index;
}

// --- lifetime -----------------------------------------------------------------------------------

LrCore::LrCore(plgdb::Database& db)
	: m_db(db), m_clock([] { return static_cast<int64_t>(std::time(nullptr)); }), m_life(std::make_shared<int>(0)) {}

LrCore::~LrCore() = default;

void LrCore::SetClock(std::function<int64_t()> clock) {
	m_clock = clock ? std::move(clock) : std::function<int64_t()>([] { return static_cast<int64_t>(std::time(nullptr)); });
}

int64_t LrCore::Now() const {
	return m_clock();
}

void LrCore::SetSettings(Settings settings) {
	// The table name is interpolated into every statement, so an invalid one is refused and the
	// default kept (admin_core::SetConfig does the same with its prefix).
	if (!settings.HasValidTableName()) {
		ReportError("levels_ranks: '" + settings.table_name + "' is not a usable table name, using lvl_base");
		settings.table_name = "lvl_base";
	}
	m_settings = std::move(settings);
}

std::string LrCore::Table() const {
	return "`" + m_settings.table_name + "`";
}

void LrCore::Query(std::string sql, plgdb::Params params, plgdb::Callback callback) {
	// One lifetime token is enough: a result landing after the core is gone is dropped instead of
	// touching freed state.
	std::weak_ptr<void> token = m_life;
	m_db.Query(std::move(sql), std::move(params),
			   [token, callback = std::move(callback)](const plgdb::Result& result) {
				   if (token.expired()) {
					   return;
				   }
				   if (callback) {
					   callback(result);
				   }
			   });
}

void LrCore::ReportError(std::string_view message) const {
	if (onError) {
		onError(message);
	}
}

bool LrCore::SlotInRange(int slot) const {
	return slot >= 0 && slot < kMaxSlots;
}

bool LrCore::Status(int slot) const {
	// The database half of CheckStatus(): the slot's row is loaded. The engine half (real player,
	// pawn, steam id, fully authenticated) belongs to the plugin.
	return SlotInRange(slot) && m_players[static_cast<std::size_t>(slot)].initialized;
}

void LrCore::MarkUninitialized(int slot) {
	if (SlotInRange(slot)) {
		m_players[static_cast<std::size_t>(slot)].initialized = false;
	}
}

const PlayerInfo* LrCore::FindPlayer(int slot) const {
	return SlotInRange(slot) ? &m_players[static_cast<std::size_t>(slot)] : nullptr;
}

PlayerInfo* LrCore::FindPlayer(int slot) {
	return SlotInRange(slot) ? &m_players[static_cast<std::size_t>(slot)] : nullptr;
}

bool LrCore::IsInitialized(int slot) const {
	return SlotInRange(slot) && m_players[static_cast<std::size_t>(slot)].initialized;
}

std::vector<int> LrCore::InitializedSlots() const {
	std::vector<int> slots;
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		if (m_players[static_cast<std::size_t>(slot)].initialized) {
			slots.push_back(slot);
		}
	}
	return slots;
}

void LrCore::ResetPlayerData(int slot) {
	if (!SlotInRange(slot)) {
		return;
	}
	PlayerInfo& info = m_players[static_cast<std::size_t>(slot)];
	// PARITY: the original's ResetPlayerData only cleared the two counter arrays and the kill
	// streak; szAuth, bInitialized and bHaveBomb survived it.
	const std::string steamid = info.steamid;
	const bool initialized = info.initialized;
	const bool have_bomb = info.have_bomb;
	info = PlayerInfo{};
	info.steamid = steamid;
	info.initialized = initialized;
	info.have_bomb = have_bomb;
	// PARITY: iStats[ST_PLAYTIME] is not a duration but the "start of the accumulated play time":
	// SaveDataPlayer() stores now - iStats[ST_PLAYTIME] as the playtime column and the load path
	// back-dates it again, so the total survives across sessions.
	info.stats[ST_PLAYTIME] = static_cast<int>(Now());
	info.stats[ST_EXP] = m_settings.type_statistics ? kRatingStartExp : m_settings.Value(LR_StartPoints);
}

// --- schema -------------------------------------------------------------------------------------

void LrCore::CreateSchema() {
	const bool mysql = m_db.GetDriver() == plgdb::Driver::MySQL;
	const std::string table = m_settings.table_name;

	// The CREATE TABLE of the original; the columns, defaults and the primary key are identical.
	// Only the driver-specific syntax differs (COLLATE/varchar/INT for MySQL, TEXT/INTEGER for the
	// SQLite fallback, which has no charset support).
	const std::string name_collate = m_settings.Value(LR_DB_Allow_UTF8MB4) ? "utf8mb4_unicode_ci" : "utf8_unicode_ci";
	const std::string create = mysql
		? "CREATE TABLE IF NOT EXISTS `" + table + "` ("
		  "`steam` varchar(22) COLLATE 'utf8_unicode_ci' PRIMARY KEY, "
		  "`name` varchar(32) COLLATE '" + name_collate + "', "
		  "`value` int NOT NULL DEFAULT 0, "
		  "`rank` int NOT NULL DEFAULT 0, "
		  "`kills` int NOT NULL DEFAULT 0, "
		  "`deaths` int NOT NULL DEFAULT 0, "
		  "`shoots` int NOT NULL DEFAULT 0, "
		  "`hits` int NOT NULL DEFAULT 0, "
		  "`headshots` int NOT NULL DEFAULT 0, "
		  "`assists` int NOT NULL DEFAULT 0, "
		  "`round_win` int NOT NULL DEFAULT 0, "
		  "`round_lose` int NOT NULL DEFAULT 0, "
		  "`playtime` int NOT NULL DEFAULT 0, "
		  "`lastconnect` int NOT NULL DEFAULT 0, "
		  "`online` int NOT NULL DEFAULT 0"
		  ");"
		: "CREATE TABLE IF NOT EXISTS `" + table + "` ("
		  "`steam` TEXT PRIMARY KEY, "
		  "`name` TEXT, "
		  "`value` INTEGER NOT NULL DEFAULT 0, "
		  "`rank` INTEGER NOT NULL DEFAULT 0, "
		  "`kills` INTEGER NOT NULL DEFAULT 0, "
		  "`deaths` INTEGER NOT NULL DEFAULT 0, "
		  "`shoots` INTEGER NOT NULL DEFAULT 0, "
		  "`hits` INTEGER NOT NULL DEFAULT 0, "
		  "`headshots` INTEGER NOT NULL DEFAULT 0, "
		  "`assists` INTEGER NOT NULL DEFAULT 0, "
		  "`round_win` INTEGER NOT NULL DEFAULT 0, "
		  "`round_lose` INTEGER NOT NULL DEFAULT 0, "
		  "`playtime` INTEGER NOT NULL DEFAULT 0, "
		  "`lastconnect` INTEGER NOT NULL DEFAULT 0, "
		  "`online` INTEGER NOT NULL DEFAULT 0"
		  ");";

	const plgdb::Result created = m_db.QuerySync(create, {});
	if (!created.Ok()) {
		throw std::runtime_error("levels_ranks: creating the " + table + " table failed: " + created.Error());
	}

	if (!mysql) {
		return; // the ALTER/CHARSET statements below are MySQL-only (the SQLite fallback is a
				// local test database, the schema above is already the final one)
	}

	// The statements the original ran right after the CREATE TABLE. The ADD COLUMN fails on every
	// server whose table already has the column (i.e. on every restart) and the two ALTERs only
	// normalise the charset of tables created by older versions, so a failure is not fatal.
	const std::string charset = m_settings.Value(LR_DB_Allow_UTF8MB4) ? "utf8mb4" : "utf8";
	const std::string suffix = m_settings.Value(LR_DB_Charset_Type) ? "_unicode_ci" : "_general_ci";

	const std::string statements[] = {
		"ALTER TABLE `" + table + "` ADD COLUMN `online` int NOT NULL DEFAULT 0;",
		"SET NAMES '" + charset + "';",
		"SET CHARSET '" + charset + "';",
		"ALTER TABLE `" + table + "` CHARACTER SET '" + charset + "' COLLATE '" + charset + suffix + "';",
		"ALTER TABLE `" + table + "` MODIFY COLUMN `name` varchar(128) CHARACTER SET '" + charset + "' COLLATE '" +
			charset + suffix + "' NOT NULL default '' AFTER `steam`;",
	};
	for (const std::string& statement : statements) {
		(void)m_db.QuerySync(statement, {}); // tolerated failures (see above)
	}
}

void LrCore::ConfigureDatabase() {
	// The tail of the original's connect callback: the row count, then the online reset and the
	// core-ready notification.
	const std::string select = "SELECT COUNT(`steam`) FROM " + Table() + " WHERE `lastconnect` LIMIT 1;";
	Query(select, {}, [this](const plgdb::Result& result) {
		if (!result.Ok()) {
			ReportError("levels_ranks: counting the players failed: " + result.Error());
		} else if (result.RowCount() > 0) {
			m_player_count = static_cast<int>(result.GetInt(0, 0, 0));
		}

		const std::string update = "UPDATE " + Table() + " SET `online` = 0 WHERE `online` = ?;";
		Query(update, {static_cast<int64_t>(m_settings.Value(LR_OnlineID))}, [this](const plgdb::Result& result) {
			if (!result.Ok()) {
				ReportError("levels_ranks: resetting the online flag failed: " + result.Error());
			}
			FireCoreReady();
		});
	});
}

void LrCore::FireCoreReady() {
	if (onCoreReady) {
		onCoreReady();
	}
}

void LrCore::CleanDatabase() {
	// OnCleanDB(): players that did not connect for lr_cleandb_days days lose their lastconnect,
	// which hides them from the top lists (and from the place-in-top sub-queries).
	const int days = m_settings.Value(LR_CleanDB_Days);
	if (days == 0) {
		return;
	}
	const int64_t threshold = Now() - static_cast<int64_t>(days) * 86400;
	const std::string sql = "UPDATE " + Table() + " SET `lastconnect` = 0 WHERE `lastconnect` < ? AND `lastconnect`;";
	Query(sql, {threshold}, [](const plgdb::Result&) {});
}

// --- player loading ------------------------------------------------------------------------------

void LrCore::LoadOrCreatePlayer(int slot, std::string steamid, std::string name) {
	if (!SlotInRange(slot)) {
		return;
	}
	PlayerInfo& info = m_players[static_cast<std::size_t>(slot)];
	// OnClientPutInServer() wiped the slot and stored the steam id before the query; the wiped
	// slot must not look initialized while the row is in flight.
	info.initialized = false;
	info.steamid = steamid;

	const std::string select =
		"SELECT `value`, `rank`, `kills`, `deaths`, `shoots`, `hits`, `headshots`, `assists`, `round_win`, `round_lose`, "
		"`playtime`, "
		"(SELECT COUNT(`steam`) FROM " + Table() + " WHERE `value` >= `player`.`value` AND `lastconnect`) AS `exppos`, "
		"(SELECT COUNT(`steam`) FROM " + Table() + " WHERE `playtime` >= `player`.`playtime` AND `lastconnect`) AS `timepos` "
		"FROM " + Table() + " `player` WHERE `steam` = ? LIMIT 1;";

	Query(select, {steamid}, [this, slot, steamid, name = std::move(name)](const plgdb::Result& result) {
		if (!result.Ok()) {
			ReportError("levels_ranks: loading the statistics failed: " + result.Error());
			return;
		}
		if (!SlotInRange(slot)) {
			return;
		}
		PlayerInfo& info = m_players[static_cast<std::size_t>(slot)];

		if (result.RowCount() > 0) {
			for (int i = ST_EXP; i != kStatsCount; ++i) {
				info.stats[static_cast<std::size_t>(i)] = static_cast<int>(result.GetInt(0, static_cast<std::size_t>(i), 0));
			}
			const int64_t now = Now();
			info.session[ST_PLAYTIME] = static_cast<int>(now);
			// PARITY: the in-memory playtime is back-dated so that SaveDataPlayer() recomputes the
			// accumulated value (see ResetPlayerData).
			info.stats[ST_PLAYTIME] = static_cast<int>(now - info.stats[ST_PLAYTIME]);
			info.initialized = true;

			const std::string update = "UPDATE " + Table() + " SET `online` = ?, `lastconnect` = ? WHERE `steam` = ?;";
			Query(update,
				  {static_cast<int64_t>(m_settings.Value(LR_OnlineID)), now, steamid},
				  [](const plgdb::Result&) {});

			if (onPlayerLoaded) {
				onPlayerLoaded(slot, steamid);
			}
			return;
		}

		// A player the table does not know yet: INSERT the start row and initialise the slot when
		// the insert succeeded (the original checked bInitialized again here, because a
		// disconnect can overtake the query).
		const std::string insert =
			"INSERT INTO " + Table() + " (`steam`, `name`, `value`, `lastconnect`) VALUES (?, ?, ?, ?);";
		const int64_t start_exp = m_settings.type_statistics ? kRatingStartExp : m_settings.Value(LR_StartPoints);
		Query(insert, {steamid, name, start_exp, Now()}, [this, slot, steamid](const plgdb::Result& result) {
			if (!result.Ok()) {
				ReportError("levels_ranks: inserting the statistics row failed: " + result.Error());
				return;
			}
			if (!SlotInRange(slot)) {
				return;
			}
			PlayerInfo& info = m_players[static_cast<std::size_t>(slot)];
			if (info.initialized) {
				return;
			}
			ResetPlayerData(slot);
			++m_player_count;
			info.initialized = true;
			info.stats[ST_EXP] =
				m_settings.type_statistics ? kRatingStartExp : m_settings.Value(LR_StartPoints);

			CheckRank(slot, false);

			if (onPlayerLoaded) {
				onPlayerLoaded(slot, steamid);
			}
		});
	});
}

// --- saving --------------------------------------------------------------------------------------

void LrCore::SavePlayer(int slot, bool disconnect) {
	if (!Status(slot)) {
		return;
	}
	PlayerInfo& info = m_players[static_cast<std::size_t>(slot)];
	const int64_t now = Now();
	const std::string steamid = info.steamid;
	const std::string name = findPlayerName ? findPlayerName(slot) : std::string();

	const std::string replace =
		"REPLACE INTO " + Table() +
		" (`steam`, `value`, `name`, `rank`, `kills`, `deaths`, `shoots`, `hits`, `headshots`, `assists`, "
		"`round_win`, `round_lose`, `playtime`, `lastconnect`, `online`) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

	Query(replace,
		  {steamid,
		   static_cast<int64_t>(info.stats[ST_EXP]),
		   name,
		   static_cast<int64_t>(info.stats[ST_RANK]),
		   static_cast<int64_t>(info.stats[ST_KILLS]),
		   static_cast<int64_t>(info.stats[ST_DEATHS]),
		   static_cast<int64_t>(info.stats[ST_SHOOTS]),
		   static_cast<int64_t>(info.stats[ST_HITS]),
		   static_cast<int64_t>(info.stats[ST_HEADSHOTS]),
		   static_cast<int64_t>(info.stats[ST_ASSISTS]),
		   static_cast<int64_t>(info.stats[ST_ROUNDSWIN]),
		   static_cast<int64_t>(info.stats[ST_ROUNDSLOSE]),
		   /*playtime*/ static_cast<int64_t>(now - info.stats[ST_PLAYTIME]),
		   now,
		   static_cast<int64_t>(m_settings.Value(LR_OnlineID))},
		  [this](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("levels_ranks: saving the statistics failed: " + result.Error());
			  }
		  });

	if (disconnect) {
		const std::string update = "UPDATE " + Table() + " SET `online` = 0 WHERE `steam` = ?;";
		Query(update, {steamid}, [](const plgdb::Result&) {});
		// the original wiped the whole slot on a disconnect (g_iPlayerInfo[iSlot] = g_iInfoNULL)
		m_players[static_cast<std::size_t>(slot)] = PlayerInfo{};
		return;
	}

	const std::string positions =
		"SELECT "
		"(SELECT COUNT(`steam`) FROM " + Table() + " WHERE `value` >= ? AND `lastconnect`) AS `exppos`, "
		"(SELECT COUNT(`steam`) FROM " + Table() + " WHERE `playtime` >= ? AND `lastconnect`) AS `timepos` "
		"LIMIT 1;";
	Query(positions,
		  {static_cast<int64_t>(info.stats[ST_EXP]), static_cast<int64_t>(now - info.stats[ST_PLAYTIME])},
		  [this, slot](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("levels_ranks: reading the place in the top failed: " + result.Error());
				  return;
			  }
			  if (result.RowCount() == 0 || !SlotInRange(slot)) {
				  return;
			  }
			  PlayerInfo& info = m_players[static_cast<std::size_t>(slot)];
			  const int old_place = info.stats[ST_PLACEINTOP];
			  const int old_place_time = info.stats[ST_PLACEINTOPTIME];

			  info.stats[ST_PLACEINTOP] = static_cast<int>(result.GetInt(0, 0, 0));
			  info.stats[ST_PLACEINTOPTIME] = static_cast<int>(result.GetInt(0, 1, 0));

			  if (old_place) {
				  info.session[ST_PLACEINTOP] += old_place - info.stats[ST_PLACEINTOP];
			  }
			  if (old_place_time) {
				  info.session[ST_PLACEINTOPTIME] += old_place_time - info.stats[ST_PLACEINTOPTIME];
			  }

			  if (onPlayerPosInTop) {
				  onPlayerPosInTop(slot, info.stats[ST_PLACEINTOP], info.stats[ST_PLACEINTOPTIME]);
			  }
		  });
}

// --- rank and experience --------------------------------------------------------------------------

RankChange LrCore::CheckRank(int slot, bool active) {
	RankChange change;
	change.slot = slot;
	if (!Status(slot)) {
		return change;
	}
	PlayerInfo& info = m_players[static_cast<std::size_t>(slot)];
	const int max_ranks = static_cast<int>(m_settings.rank_exp.size());
	if (max_ranks == 0) {
		return change;
	}

	const int old_rank = info.stats[ST_RANK];
	int rank = RankForExp(info.stats[ST_EXP], m_settings.rank_exp);
	if (rank == old_rank) {
		return change;
	}

	int new_rank = rank;
	if (onLevelChangedPre) {
		onLevelChangedPre(slot, new_rank, old_rank); // the int& of the original: may be rewritten
	}
	info.stats[ST_RANK] = new_rank;
	const bool up = new_rank > old_rank;

	change.changed = true;
	change.new_rank = new_rank;
	change.old_rank = old_rank;
	change.up = up;

	if (active) {
		info.session[ST_RANK] += new_rank - old_rank;

		// The player's own message (LR_ShowLevelUpMessage / LR_ShowLevelDownMessage of the core's
		// macro block = slots 6/7, see LoadSettings).
		const int personal_setting = up ? kShowLevelUp : kShowLevelDown;
		if (m_settings.Value(personal_setting) && onLevelMessage) {
			onLevelMessage(slot, slot, new_rank, up, true);
		}

		// The broadcast message: g_Settings[LR_ShowAllLevelUpMessage + !bIsUp] - the same two
		// slots - sent to every other initialized player.
		const int broadcast_setting = LR_ShowAllLevelUpMessage + (up ? 0 : 1);
		if (m_settings.Value(broadcast_setting) && onLevelMessage) {
			for (int other = 0; other < kMaxSlots; ++other) {
				if (other != slot && m_players[static_cast<std::size_t>(other)].initialized) {
					onLevelMessage(other, slot, new_rank, up, false);
				}
			}
		}

		if (m_settings.Value(LR_DB_SaveDataPlayer_Mode)) {
			SavePlayer(slot);
			change.saved = true;
		}
	}

	if (onLevelChangedPost) {
		onLevelChangedPost(slot, new_rank, old_rank);
	}
	return change;
}

ExpResult LrCore::GiveExp(int slot, int value, const char* phrase, bool allow) {
	ExpResult out;
	out.requested = value;
	if (!Status(slot) || (!allow && !m_allow_statistic)) {
		return out;
	}
	out.status = true;

	if (onExpChangedPre) {
		int requested = value;
		onExpChangedPre(slot, requested);
		value = requested;
		out.requested = value;
	}
	if (value == 0) {
		return out;
	}

	PlayerInfo& info = m_players[static_cast<std::size_t>(slot)];
	const int floor = m_settings.type_statistics ? kRatingFloor : 0;
	const int old_exp = info.stats[ST_EXP];

	if ((info.stats[ST_EXP] += value) < floor) {
		info.stats[ST_EXP] = floor;
	}

	const int applied = info.stats[ST_EXP] - old_exp;
	info.round_exp += applied;
	info.session[ST_EXP] += applied;
	out.applied = true;
	out.applied_value = applied;
	out.new_exp = info.stats[ST_EXP];

	const RankChange change = CheckRank(slot);
	out.saved = change.saved;

	if (onExpChangedPost) {
		onExpChangedPost(slot, applied, info.stats[ST_EXP]);
	}

	if (m_settings.Value(LR_ShowUsualMessage) == 1 && onExpMessage) {
		onExpMessage(slot, phrase != nullptr ? phrase : "", info.stats[ST_EXP], value);
	}
	return out;
}

bool LrCore::ChangeClientValue(int slot, int give_exp) {
	if (!Status(slot)) {
		return false;
	}
	PlayerInfo& info = m_players[static_cast<std::size_t>(slot)];
	const int floor = m_settings.type_statistics ? kRatingFloor : 0;
	const int old_exp = info.stats[ST_EXP];

	if ((info.stats[ST_EXP] += give_exp) < floor) {
		// PARITY: the original assigns the clamped value to iGiveExp here
		// (iGiveExp = (iStats[ST_EXP] = iExpMin) - iOldExp), so the round/session counters get the
		// clamped delta instead of the requested one.
		give_exp = (info.stats[ST_EXP] = floor) - old_exp;
	}

	info.round_exp += give_exp;
	info.session[ST_EXP] += give_exp;

	CheckRank(slot);
	return true;
}

void LrCore::ResetPlayerStats(int slot) {
	if (!SlotInRange(slot)) {
		return;
	}
	const std::string steamid = m_players[static_cast<std::size_t>(slot)].steamid;
	ResetPlayerData(slot);
	CheckRank(slot, false);
	if (onResetPlayerStats) {
		onResetPlayerStats(slot, steamid);
	}
}

// --- cleanup and top lists ------------------------------------------------------------------------

void LrCore::CleanupData(int type, std::function<void(bool)> done) {
	std::string sql;
	switch (type) {
		case LR_AllData:
			// SQLite has no TRUNCATE; the effect (an empty table) is the same.
			sql = m_db.GetDriver() == plgdb::Driver::MySQL ? "TRUNCATE TABLE " + Table() + ";" : "DELETE FROM " + Table() + ";";
			break;
		case LR_ExpData:
			sql = "UPDATE " + Table() + " SET `value` = ?, `rank` = 0;";
			break;
		case LR_StatsData:
			sql = "UPDATE " + Table() +
				  " SET `kills` = 0, `deaths` = 0, `shoots` = 0, `hits` = 0, `headshots` = 0, `assists` = 0, "
				  "`round_win` = 0, `round_lose` = 0;";
			break;
		default:
			ReportError("levels_ranks: invalid type of cleaning");
			if (done) {
				done(false);
			}
			return;
	}

	// The original fired the hook *before* the statement ran.
	if (onDatabaseCleanup) {
		onDatabaseCleanup(type);
	}

	plgdb::Params params;
	if (type == LR_ExpData) {
		params.push_back(static_cast<int64_t>(m_settings.type_statistics ? kRatingStartExp : m_settings.Value(LR_StartPoints)));
	}
	Query(sql, std::move(params), [this, done = std::move(done)](const plgdb::Result& result) {
		if (!result.Ok()) {
			ReportError("levels_ranks: cleaning the database failed: " + result.Error());
		}
		if (done) {
			done(result.Ok());
		}
	});
}

void LrCore::LoadTopPlayers(bool by_playtime, std::function<void(std::vector<TopRow>)> callback) {
	const std::string column = TopOrderExpression(by_playtime);
	const std::string sql = "SELECT `name`, `steam`, " + column + " FROM " + Table() + " WHERE `lastconnect` ORDER BY " +
							column + " DESC LIMIT ?;";
	Query(sql, {static_cast<int64_t>(m_settings.Value(LR_TopCount))},
		  [this, callback = std::move(callback)](const plgdb::Result& result) {
			  std::vector<TopRow> rows;
			  if (!result.Ok()) {
				  ReportError("levels_ranks: reading the top list failed: " + result.Error());
			  } else {
				  rows.reserve(result.RowCount());
				  for (std::size_t row = 0; row < result.RowCount(); ++row) {
					  TopRow entry;
					  entry.name = result.GetString(row, 0);
					  entry.steamid = result.GetString(row, 1);
					  entry.value = result.GetDouble(row, 2);
					  rows.push_back(std::move(entry));
				  }
			  }
			  if (callback) {
				  callback(std::move(rows));
			  }
		  });
}

} // namespace levels_ranks
