// The console and chat commands of the Pisex cs2-lvl_ranks core:
//
//   mm_lvl_give_admin <userid|steamid>  (console)  -> g_bAdminAccess[slot] = true
//   mm_lvl_reset <all|exp|stats>        (console)  -> the statistics cleanup of the original
//   mm_lvl / sm_lvl        + !lvl / lvl / !дмд / дмд        -> the main menu
//   mm_rank / sm_rank      + !rank / rank / !кфтл / кфтл    -> the rank message
//   mm_session / sm_session + !session / session / !ыуыышщт -> (the original registered an empty
//                                                              handler; kept so the chat command is
//                                                              consumed by the same names)
//   mm_toptime / sm_toptime + !toptime ...                  -> the play time top
//   mm_top / sm_top        + !top / top / !ещз / ещз         -> the experience top
//
// pisexcompat registers console names for the server console and chat names for the chat; the
// handler receives the typed line (command name first), which is what the original's Tokenize()
// saw.

#include "lr_plugin.hpp"

#include <charconv>
#include <ctime>
#include <string>

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

} // namespace

void LrPlugin::RegisterCommands() {
	// --- mm_lvl_give_admin ---------------------------------------------------------------------
	pisex::g_pUtils->RegCommand({"mm_lvl_give_admin"}, {}, [](int /*slot*/, const char* content) {
		pisex::CommandArgs args(content);
		if (args.ArgC() > 1 && ContainsOnlyDigits(args[1])) {
			long long wanted = 0;
			const std::string text(args[1]);
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), wanted);
			if (parsed.ec != std::errc{}) {
				pisex::PrintToConsole(-1, "[LR] Usage: mm_lvl_give_admin <userid|steamid>");
				return false;
			}
			bool found = false;
			for (int slot = 0; slot < kMaxSlots; ++slot) {
				if (!pisex::g_pPlayers->IsConnected(slot)) {
					continue;
				}
				// PARITY: the original compared the account id of the controller, the slot
				// itself and the 64-bit steam id against the argument. The account-id export is
				// one of the optional s2sdk bindings: without it only the other two comparisons run.
				const uint64_t steamid64 = pisex::g_pPlayers->GetSteamID64(slot);
				if (steamid64 == 0) {
					continue;
				}
				const bool account_matches =
					__s2sdk_GetClientAccountId != nullptr &&
					static_cast<int64_t>(s2sdk::GetClientAccountId(slot)) == static_cast<int64_t>(wanted);
				if (account_matches || slot == wanted || steamid64 == static_cast<uint64_t>(wanted)) {
					found = true;
					g_lr_plugin.SetAdminAccess(slot, true);
					break;
				}
			}
			if (found) {
				pisex::PrintToConsole(-1, "[LR] Admin access granted");
			} else {
				pisex::PrintToConsole(-1, "[LR] Player not found");
			}
		} else {
			pisex::PrintToConsole(-1, "[LR] Usage: mm_lvl_give_admin <userid|steamid>");
		}
		return false;
	});

	// --- mm_lvl_reset <type> --------------------------------------------------------------------
	pisex::g_pUtils->RegCommand({"mm_lvl_reset"}, {}, [](int /*slot*/, const char* content) {
		pisex::CommandArgs args(content);
		if (args.ArgC() <= 1) {
			pisex::PrintToConsole(-1, "Usage: mm_lvl_reset <type>");
			return false;
		}
		int type = -1;
		const std::string requested(args[1]);
		if (requested == "all") {
			type = LR_AllData;
		} else if (requested == "exp") {
			type = LR_ExpData;
		} else if (requested == "stats") {
			type = LR_StatsData;
		}
		if (type < 0) {
			pisex::PrintToConsole(-1, "[LR] %s - invalid type of cleaning.", requested.c_str());
			return false;
		}
		g_lr_plugin.RunDatabaseCleanup(type);
		return false;
	});

	// --- the menu and report commands -------------------------------------------------------------
	pisex::g_pUtils->RegCommand({"mm_lvl", "sm_lvl"}, {"!lvl", "lvl", "!дмд", "дмд"},
								[](int slot, const char*) {
									g_lr_plugin.ShowMainMenu(slot);
									return false;
								});

	pisex::g_pUtils->RegCommand({"mm_rank", "sm_rank"}, {"!rank", "rank", "!кфтл", "кфтл"},
								[](int slot, const char*) {
									g_lr_plugin.ShowRankReport(slot);
									return false;
								});

	// The original registered a handler that does nothing (the session numbers are shown in the
	// menu); the names are kept so the chat line is consumed exactly like before.
	pisex::g_pUtils->RegCommand({"mm_session", "sm_session"},
								{"!session", "session", "!ыуыышщт", "ыуыышщт"},
								[](int /*slot*/, const char*) { return false; });

	pisex::g_pUtils->RegCommand({"mm_toptime", "sm_toptime"},
								{"!toptime", "toptime", "!ещзешьу", "ещзешьу"},
								[](int slot, const char*) {
									g_lr_plugin.ShowTopPlayers(slot, true);
									return false;
								});

	pisex::g_pUtils->RegCommand({"mm_top", "sm_top"}, {"!top", "top", "!ещз", "ещз"},
								[](int slot, const char*) {
									g_lr_plugin.ShowTopPlayers(slot, false);
									return false;
								});
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
		pisex::PrintToConsole(-1, "[LR] Successful clearing %s data in the database!", name);
		if (m_core == nullptr) {
			return;
		}
		for (const int slot : m_core->InitializedSlots()) {
			if (m_steamids[static_cast<std::size_t>(slot)].empty()) {
				continue;
			}
			m_core->LoadOrCreatePlayer(slot, m_steamids[static_cast<std::size_t>(slot)], PlayerNameOf(slot));
		}
	});
}

void LrPlugin::ShowRankReport(int slot) {
	// The mm_rank handler of the original: one line per player when lr_show_rankmessage is on,
	// otherwise the caller's own line only.
	if (m_core == nullptr) {
		return;
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
		return pisex::FormatString(Phrase("RankPlayer").c_str(), name.c_str(), place, m_core->PlayerCount(), exp,
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
