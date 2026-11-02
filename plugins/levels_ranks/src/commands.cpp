// The console and chat commands of the Pisex cs2-lvl_ranks core:
//
//   mm_lvl_give_admin <userid|steamid>  (server console) -> grants the admin menu to a player
//   mm_lvl_reset <all|exp|stats>        (server console) -> the statistics cleanup of the original
//   mm_lvl / sm_lvl        + !lvl / lvl / !дмд / дмд        -> the main menu
//   mm_rank / sm_rank      + !rank / rank / !кфтл / кфтл    -> the rank message
//   mm_session / sm_session + !session / session / !ыуыышщт -> (the original registered an empty
//                                                              handler; kept so the chat command is
//                                                              consumed by the same names)
//   mm_toptime / sm_toptime + !toptime ...                  -> the play time top
//   mm_top / sm_top        + !top / top / !ещз / ещз         -> the experience top
//
// The registration/dispatch layer (console context, chat context, trigger-less say hook, the
// server-console caller check and the A7 teardown) lives in lr_commands.hpp/cpp, so these
// handlers only contain the original's logic.

#include "lr_plugin.hpp"

#include "lr_commands.hpp"
#include "lr_phrases.hpp"

#include <charconv>
#include <ctime>
#include <string>
#include <string_view>

namespace levels_ranks {

namespace {

bool ContainsOnlyDigits(const char* text) {
	if (text == nullptr || *text == '\0') {
		return false;
	}
	for (const char* cursor = text; *cursor != '\0'; ++cursor) {
		if (*cursor < '0' || *cursor > '9') {
			return false;
		}
	}
	return true;
}

// --- the menu commands -----------------------------------------------------------------------

void MainMenuCommand(void* user, int32_t caller, bool /*from_chat*/, const CommandArgs& /*args*/) {
	if (caller < 0) {
		return; // the original's CheckStatus(-1) answered false; a console invocation does nothing
	}
	static_cast<LrPlugin*>(user)->ShowMainMenu(caller);
}

void RankCommand(void* user, int32_t caller, bool /*from_chat*/, const CommandArgs& /*args*/) {
	if (caller < 0) {
		return;
	}
	static_cast<LrPlugin*>(user)->ShowRankReport(caller);
}

void SessionCommand(void* /*user*/, int32_t /*caller*/, bool /*from_chat*/, const CommandArgs& /*args*/) {
	// The original registered a handler that does nothing (the session numbers are shown in the
	// menu); the names are kept so the chat line is consumed exactly like before.
}

void TopTimeCommand(void* user, int32_t caller, bool /*from_chat*/, const CommandArgs& /*args*/) {
	if (caller < 0) {
		return;
	}
	static_cast<LrPlugin*>(user)->ShowTopPlayers(caller, true);
}

void TopCommand(void* user, int32_t caller, bool /*from_chat*/, const CommandArgs& /*args*/) {
	if (caller < 0) {
		return;
	}
	static_cast<LrPlugin*>(user)->ShowTopPlayers(caller, false);
}

// --- the server-console commands (the registry already rejected every caller != -1) ----------

void GiveAdminCommand(void* user, int32_t /*caller*/, bool /*from_chat*/, const CommandArgs& args) {
	LrPlugin* plugin = static_cast<LrPlugin*>(user);
	if (args.ArgC() > 1 && ContainsOnlyDigits(args.Arg(1))) {
		long long wanted = 0;
		const std::string_view text(args.Arg(1));
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), wanted);
		if (parsed.ec != std::errc{}) {
			plugin->PrintToConsole("[LR] Usage: mm_lvl_give_admin <userid|steamid>");
			return;
		}
		bool found = false;
		for (int slot = 0; slot < kMaxSlots; ++slot) {
			if (!IsClientConnected(slot)) {
				continue;
			}
			const uint64_t steamid64 = ClientSteamId64(slot);
			if (steamid64 == 0) {
				continue;
			}
			const bool account_matches =
				static_cast<int64_t>(ClientAccountId(slot)) == static_cast<int64_t>(wanted);
			if (account_matches || slot == wanted || steamid64 == static_cast<uint64_t>(wanted)) {
				found = plugin->SetAdminAccess(slot, true);
				break;
			}
		}
		plugin->PrintToConsole(found ? "[LR] Admin access granted" : "[LR] Player not found");
	} else {
		plugin->PrintToConsole("[LR] Usage: mm_lvl_give_admin <userid|steamid>");
	}
}

void ResetCommand(void* user, int32_t /*caller*/, bool /*from_chat*/, const CommandArgs& args) {
	LrPlugin* plugin = static_cast<LrPlugin*>(user);
	if (args.ArgC() <= 1) {
		plugin->PrintToConsole("Usage: mm_lvl_reset <type>");
		return;
	}
	int type = -1;
	const std::string requested(args.Arg(1));
	if (requested == "all") {
		type = LR_AllData;
	} else if (requested == "exp") {
		type = LR_ExpData;
	} else if (requested == "stats") {
		type = LR_StatsData;
	}
	if (type < 0) {
		plugin->PrintToConsole("[LR] " + requested + " - invalid type of cleaning.");
		return;
	}
	plugin->RunDatabaseCleanup(type);
}

} // namespace

void LrPlugin::RegisterCommands() {
	const CommandSpec specs[] = {
		{"mm_lvl_give_admin", false, true, &GiveAdminCommand},
		{"mm_lvl_reset", false, true, &ResetCommand},

		{"mm_lvl", false, false, &MainMenuCommand},
		{"sm_lvl", false, false, &MainMenuCommand},
		{"!lvl", true, false, &MainMenuCommand},
		{"lvl", true, false, &MainMenuCommand},
		{"!дмд", true, false, &MainMenuCommand},
		{"дмд", true, false, &MainMenuCommand},

		{"mm_rank", false, false, &RankCommand},
		{"sm_rank", false, false, &RankCommand},
		{"!rank", true, false, &RankCommand},
		{"rank", true, false, &RankCommand},
		{"!кфтл", true, false, &RankCommand},
		{"кфтл", true, false, &RankCommand},

		{"mm_session", false, false, &SessionCommand},
		{"sm_session", false, false, &SessionCommand},
		{"!session", true, false, &SessionCommand},
		{"session", true, false, &SessionCommand},
		{"!ыуыышщт", true, false, &SessionCommand},
		{"ыуыышщт", true, false, &SessionCommand},

		{"mm_toptime", false, false, &TopTimeCommand},
		{"sm_toptime", false, false, &TopTimeCommand},
		{"!toptime", true, false, &TopTimeCommand},
		{"toptime", true, false, &TopTimeCommand},
		{"!ещзешьу", true, false, &TopTimeCommand},
		{"ещзешьу", true, false, &TopTimeCommand},

		{"mm_top", false, false, &TopCommand},
		{"sm_top", false, false, &TopCommand},
		{"!top", true, false, &TopCommand},
		{"top", true, false, &TopCommand},
		{"!ещз", true, false, &TopCommand},
		{"ещз", true, false, &TopCommand},
	};
	levels_ranks::RegisterCommands(specs, sizeof(specs) / sizeof(specs[0]), this);
}

void LrPlugin::UnregisterCommands() {
	levels_ranks::UnregisterCommands();
}

void LrPlugin::RunDatabaseCleanup(int type) {
	if (m_core == nullptr) {
		return;
	}
	static const char* const kNames[] = {"all", "exp", "stats"};
	const char* name = type >= 0 && type <= 2 ? kNames[type] : "?";
	m_core->CleanupData(type, [this, name](bool ok) {
		if (!ok) {
			return;
		}
		// The original printed this from the query callback and then re-loaded every initialized
		// player (g_LR.OnClientPutInServer for each bInitialized slot).
		PrintToConsole(std::string("[LR] Successful clearing ") + name + " data in the database!");
		if (m_core == nullptr) {
			return;
		}
		m_core->ReloadInitializedPlayers();
	});
}

void LrPlugin::ShowRankReport(int slot) {
	// The mm_rank handler of the original: one line per player when lr_show_rankmessage is on,
	// otherwise the caller's own line only.
	if (m_core == nullptr || !IsMapReady()) {
		return;
	}
	if (slot >= 0 && slot < kMaxSlots) {
		const int64_t now = static_cast<int64_t>(std::time(nullptr));
		if (now - m_last_rank_request[static_cast<std::size_t>(slot)] < kCommandCooldownSeconds) {
			return; // the broadcast is up to 32 chat lines; a key-bound !rank must not repeat it
		}
		m_last_rank_request[static_cast<std::size_t>(slot)] = now;
	}
	const PlayerInfo* info = m_core->FindPlayer(slot);
	if (info == nullptr) {
		return;
	}
	const int kills = info->stats[ST_KILLS];
	const int deaths = info->stats[ST_DEATHS];
	const float kdr = kills / (deaths ? static_cast<float>(deaths) : 1.0f);
	const std::string name = PlayerNameOf(slot);

	const auto line = [this, slot, &name, kills, deaths, kdr] {
		const PlayerInfo* current = m_core->FindPlayer(slot);
		const int place = current != nullptr ? current->stats[ST_PLACEINTOP] : 0;
		const int exp = current != nullptr ? current->stats[ST_EXP] : 0;
		return FormatString(Phrase("RankPlayer").c_str(), name.c_str(), place, m_core->PlayerCount(), exp,
							kills, deaths, kdr);
	};

	if (m_settings.Value(LR_ShowRankMessage) != 0) {
		for (int other = 0; other < kMaxSlots; ++other) {
			if (CheckStatus(other)) {
				PrintToChat(other, line());
			}
		}
	} else {
		PrintToChat(slot, line());
	}
}

} // namespace levels_ranks
