// The menu tree of the Pisex cs2-lvl_ranks core, ported onto libs/pisexcompat:
//
//   OnLRMenu            -> ShowMainMenu       (rank, experience, place, my stats, tops, ranks, admin)
//   MyStats             -> ShowMyStats        (+ ShowSessionStats, ShowStatsReset)
//   MenuTops            -> ShowTopsMenu       -> OverAllTopPlayers (exp / play time)
//   OverAllRanks        -> ShowRanksMenu
//   MenuAdmin           -> ShowAdminMenu      (reload configs, give/take experience)
//   GiveTakeValue       -> ShowGiveTakeMenu   -> MenuValues (ShowGiveTakeValues)
//
// The item numbers of the original are preserved: 1..5 are the selectable items (pisexcompat maps
// the engine's page index back to them) and 7 is the "back" cancel reason. Everything the original
// switched on is kept verbatim, including the disabled info rows and the placeholders of the
// phrases (they are printed with the engine's printf formatting).

#include "lr_plugin.hpp"

#include <cmath>
#include <ctime>
#include <string>

namespace levels_ranks {

namespace {

// RoundToCeil() of the original (the KDR/accuracy/winrate percentages round up).
int RoundToCeil(double value) {
	return static_cast<int>(std::ceil(value));
}

} // namespace

// --- the main menu ---------------------------------------------------------------------------------

void LrPlugin::ShowMainMenu(int slot) {
	if (!CheckStatus(slot)) {
		return;
	}
	const PlayerInfo* info = Core()->FindPlayer(slot);
	if (info == nullptr) {
		return;
	}
	const int rank = info->stats[ST_RANK];
	const int max_ranks = static_cast<int>(m_settings.rank_names.size());

	std::string exp_text;
	if (rank == max_ranks) {
		exp_text = std::to_string(info->stats[ST_EXP]);
	} else {
		const int next = rank >= 0 && rank < max_ranks ? m_settings.rank_exp[static_cast<std::size_t>(rank)] : 0;
		exp_text = std::to_string(next) + " / " + std::to_string(info->stats[ST_EXP]);
	}

	const std::size_t name_index = rank > 0 ? static_cast<std::size_t>(rank - 1) : 0;
	const std::string rank_name =
		name_index < m_settings.rank_names.size() ? Phrase(m_settings.rank_names[name_index]) : std::string();

	pisex::Menu menu;
	pisex::SetTitleMenu(menu, m_settings.plugin_title.c_str());
	pisex::AddItemMenu(menu, "", pisex::FormatString(Phrase("MainMenuRank").c_str(), rank_name.c_str()).c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "", pisex::FormatString(Phrase("MainMenuExp").c_str(), exp_text.c_str()).c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MainMenuPlace").c_str(), info->stats[ST_PLACEINTOP],
										   Core()->PlayerCount())
						   .c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "1", Phrase("MainMenu_MyStats").c_str());
	pisex::AddItemMenu(menu, "3", Phrase("MainMenu_TopPlayers").c_str());
	if (m_settings.Value(LR_ShowRankList) != 0) {
		pisex::AddItemMenu(menu, "4", Phrase("MainMenu_Ranks").c_str());
	}
	if (HasAdminAccess(slot)) {
		pisex::AddItemMenu(menu, "0", Phrase("MainMenu_Admin").c_str());
	}

	pisex::SetExitMenu(menu, true);
	pisex::SetBackMenu(menu, false);
	pisex::SetCallback(menu, [](const char* back, const char*, int item, int player_slot) {
		if (item < 7) {
			switch (std::atoi(back)) {
				case 0: g_lr_plugin.ShowAdminMenu(player_slot); break;
				case 1: g_lr_plugin.ShowMyStats(player_slot); break;
				case 3: g_lr_plugin.ShowTopsMenu(player_slot); break;
				case 4: g_lr_plugin.ShowRanksMenu(player_slot); break;
				default: break;
			}
		}
	});
	pisex::DisplayPlayerMenu(menu, slot);
}

// --- my statistics ------------------------------------------------------------------------------------

void LrPlugin::ShowMyStats(int slot) {
	if (!CheckStatus(slot)) {
		return;
	}
	const PlayerInfo* info = Core()->FindPlayer(slot);
	if (info == nullptr) {
		return;
	}
	const int rounds_win = info->stats[ST_ROUNDSWIN];
	const int rounds_all = rounds_win + info->stats[ST_ROUNDSLOSE];
	const int play_time = static_cast<int>(std::time(nullptr)) - info->stats[ST_PLAYTIME];
	const int kills = info->stats[ST_KILLS];
	const int deaths = info->stats[ST_DEATHS];
	const int headshots = info->stats[ST_HEADSHOTS];
	const int shots = info->stats[ST_SHOOTS];

	pisex::Menu menu;
	pisex::SetTitleMenu(menu, Phrase("MyStatsInfoTitle").c_str());
	pisex::AddItemMenu(menu, "0", Phrase("MyStatsSession").c_str());
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MyStatsInfoPlayed").c_str(), play_time / 3600, play_time / 60 % 60,
										   play_time % 60)
						   .c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(
		menu, "", pisex::FormatString(Phrase("MyStatsInfoKills").c_str(), kills).c_str(), pisex::ITEM_DISABLED);
	pisex::AddItemMenu(
		menu, "", pisex::FormatString(Phrase("MyStatsInfoDeaths").c_str(), deaths).c_str(), pisex::ITEM_DISABLED);
	pisex::AddItemMenu(
		menu, "", pisex::FormatString(Phrase("MyStatsInfoAssists").c_str(), info->stats[ST_ASSISTS]).c_str(),
		pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MyStatsInfoHeadshots").c_str(), headshots,
										   RoundToCeil(100.0 / (kills ? kills : 1) * headshots))
						   .c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MyStatsInfoKDR").c_str(), kills / (deaths ? float(deaths) : 1.0f))
						   .c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MyStatsInfoAccuracy").c_str(),
										   RoundToCeil(100.0 / (shots ? float(shots) : 1.0f) * info->session[ST_HITS]))
						   .c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MyStatsInfoWinRate").c_str(),
										   RoundToCeil(100.0 / (rounds_all ? float(rounds_all) : 1.0f) * rounds_win))
						   .c_str(),
					   pisex::ITEM_DISABLED);

	if (m_settings.Value(LR_ShowResetMyStats) != 0) {
		const std::string cooldown_text = Cookie(slot, "ResetMyStatsCooldown");
		int cooldown = cooldown_text.empty() ? 0 : std::atoi(cooldown_text.c_str());
		if (cooldown == 0 || cooldown < static_cast<int>(std::time(nullptr))) {
			pisex::AddItemMenu(menu, "2", Phrase("MyStatsReset").c_str());
		} else {
			cooldown -= static_cast<int>(std::time(nullptr));
			pisex::AddItemMenu(menu, "2",
							   pisex::FormatString(Phrase("MyStatsResetCooldown").c_str(), cooldown / 3600,
												   cooldown / 60 % 60)
								   .c_str(),
							   pisex::ITEM_DISABLED);
		}
	}

	pisex::SetExitMenu(menu, true);
	pisex::SetBackMenu(menu, true);
	pisex::SetCallback(menu, [](const char* back, const char*, int item, int player_slot) {
		if (item < 7) {
			switch (std::atoi(back)) {
				case 0: g_lr_plugin.ShowSessionStats(player_slot); break;
				case 2: g_lr_plugin.ShowStatsReset(player_slot); break;
				default: break;
			}
		} else if (item == 7) {
			g_lr_plugin.ShowMainMenu(player_slot);
		}
	});
	pisex::DisplayPlayerMenu(menu, slot);
}

void LrPlugin::ShowSessionStats(int slot) {
	if (!CheckStatus(slot)) {
		return;
	}
	const PlayerInfo* info = Core()->FindPlayer(slot);
	if (info == nullptr) {
		return;
	}
	const int rounds_win = info->session[ST_ROUNDSWIN];
	const int rounds_all = rounds_win + info->session[ST_ROUNDSLOSE];
	const int play_time = static_cast<int>(std::time(nullptr)) - info->session[ST_PLAYTIME];
	const int kills = info->session[ST_KILLS];
	const int deaths = info->session[ST_DEATHS];
	const int headshots = info->session[ST_HEADSHOTS];
	const int shots = info->session[ST_SHOOTS];

	const std::string exp_value = (info->session[ST_EXP] > 0 ? "+" : "") + std::to_string(info->session[ST_EXP]);
	const std::string place_value =
		(info->session[ST_PLACEINTOP] > 0 ? "+" : "") + std::to_string(info->session[ST_PLACEINTOP]);

	pisex::Menu menu;
	pisex::SetTitleMenu(menu, Phrase("MyStatsSessionInfoTitle").c_str());
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MyStatsSessionInfoExp").c_str(), exp_value.c_str()).c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MyStatsSessionInfoPlace").c_str(), place_value.c_str()).c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MyStatsInfoPlayed").c_str(), play_time / 3600, play_time / 60 % 60,
										   play_time % 60)
						   .c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(
		menu, "", pisex::FormatString(Phrase("MyStatsInfoKills").c_str(), kills).c_str(), pisex::ITEM_DISABLED);
	pisex::AddItemMenu(
		menu, "", pisex::FormatString(Phrase("MyStatsInfoDeaths").c_str(), deaths).c_str(), pisex::ITEM_DISABLED);
	pisex::AddItemMenu(
		menu, "", pisex::FormatString(Phrase("MyStatsInfoAssists").c_str(), info->session[ST_ASSISTS]).c_str(),
		pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MyStatsInfoHeadshots").c_str(), headshots,
										   RoundToCeil(100.0 / (kills ? kills : 1) * headshots))
						   .c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MyStatsInfoKDR").c_str(), kills / (deaths ? float(deaths) : 1.0f))
						   .c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MyStatsInfoAccuracy").c_str(),
										   RoundToCeil(100.0 / (shots ? float(shots) : 1.0f) * info->session[ST_HITS]))
						   .c_str(),
					   pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "",
					   pisex::FormatString(Phrase("MyStatsInfoWinRate").c_str(),
										   RoundToCeil(100.0 / (rounds_all ? float(rounds_all) : 1.0f) * rounds_win))
						   .c_str(),
					   pisex::ITEM_DISABLED);

	pisex::SetExitMenu(menu, true);
	pisex::SetBackMenu(menu, true);
	pisex::SetCallback(menu, [](const char*, const char*, int item, int player_slot) {
		if (item == 7) {
			g_lr_plugin.ShowMyStats(player_slot);
		}
	});
	pisex::DisplayPlayerMenu(menu, slot);
}

void LrPlugin::ShowStatsReset(int slot) {
	// MyStatsReset() of the original: the warning page, then "Yes"/"No".
	pisex::Menu menu;
	pisex::SetTitleMenu(menu, Phrase("MyStatsResetInfoTitle").c_str());
	pisex::AddItemMenu(menu, "", Phrase("MyStatsResetInfo1").c_str(), pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "", Phrase("MyStatsResetInfo2").c_str(), pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "", Phrase("MyStatsResetInfo3").c_str(), pisex::ITEM_DISABLED);
	pisex::AddItemMenu(menu, "1", Phrase("Yes").c_str());
	pisex::AddItemMenu(menu, "0", Phrase("No").c_str());
	pisex::SetExitMenu(menu, true);
	pisex::SetBackMenu(menu, true);
	pisex::SetCallback(menu, [](const char* back, const char*, int item, int player_slot) {
		if (item < 7) {
			switch (std::atoi(back)) {
				case 0: g_lr_plugin.ShowMyStats(player_slot); break;
				case 1:
					// ResetMenuHandle() of the original: the cooldown cookie and the reset.
					g_lr_plugin.SetCookie(player_slot, "ResetMyStatsCooldown",
										  std::to_string(std::time(nullptr) +
														 g_lr_plugin.Cfg().Value(LR_ResetMyStatsCooldown)));
					g_lr_plugin.ResetPlayerStats(player_slot);
					break;
				default: break;
			}
		} else if (item == 7) {
			g_lr_plugin.ShowMyStats(player_slot);
		}
	});
	pisex::DisplayPlayerMenu(menu, slot);
}

// --- the tops -------------------------------------------------------------------------------------------

void LrPlugin::ShowTopsMenu(int slot) {
	pisex::Menu menu;
	pisex::SetTitleMenu(menu, Phrase("MainMenu_TopPlayers").c_str());
	pisex::AddItemMenu(menu, "0", Phrase("OverAllTopPlayersExp").c_str());
	pisex::AddItemMenu(menu, "1", Phrase("OverAllTopPlayersTime").c_str());
	pisex::SetExitMenu(menu, true);
	pisex::SetBackMenu(menu, true);
	pisex::SetCallback(menu, [](const char* back, const char*, int item, int player_slot) {
		if (item < 7) {
			switch (std::atoi(back)) {
				case 0: g_lr_plugin.ShowTopPlayers(player_slot, false); break;
				case 1: g_lr_plugin.ShowTopPlayers(player_slot, true); break;
				default: break;
			}
		} else if (item == 7) {
			g_lr_plugin.ShowMainMenu(player_slot);
		}
	});
	pisex::DisplayPlayerMenu(menu, slot);
}

void LrPlugin::ShowTopPlayers(int slot, bool by_playtime) {
	pisex::ClosePlayerMenu(slot);
	if (!CheckStatus(slot)) {
		return;
	}
	const std::string own_steamid = SteamIdOf(slot);
	Core()->LoadTopPlayers(by_playtime, [this, slot, by_playtime, own_steamid](std::vector<TopRow> rows) {
		if (rows.empty()) {
			// The original printed NoData when the query returned no row.
			PrintToChat(slot, Phrase("NoData"));
			return;
		}
		if (!CheckStatus(slot)) {
			return;
		}
		const char* title_key = by_playtime ? "OverAllTopPlayersTime" : "OverAllTopPlayersExp";
		const char* slot_key = by_playtime ? "OverAllTopPlayersTime_Slot" : "OverAllTopPlayersExp_Slot";

		pisex::Menu menu;
		const std::string title =
			std::string(m_settings.plugin_title) + " | " + Phrase(title_key);
		pisex::SetTitleMenu(menu, title.c_str());

		int position = 1;
		for (const TopRow& row : rows) {
			std::string entry = row.steamid == own_steamid
									? std::string(Phrase(slot_key)) + " " + Phrase("You")
									: std::string(Phrase(slot_key));
			// The slot phrases: "%i - [ %i ] - %s" (experience) and "%i - [ %.1f h. ] - %s"
			// (play time) - position, value, name.
			const std::string text = by_playtime
										 ? pisex::FormatString(entry.c_str(), position, row.value, row.name.c_str())
										 : pisex::FormatString(entry.c_str(), position, static_cast<int>(row.value),
															   row.name.c_str());
			pisex::AddItemMenu(menu, "", text.c_str(), pisex::ITEM_DISABLED);
			++position;
		}
		pisex::SetExitMenu(menu, true);
		pisex::SetBackMenu(menu, true);
		pisex::SetCallback(menu, [](const char*, const char*, int item, int player_slot) {
			if (item == 7) {
				g_lr_plugin.ShowMainMenu(player_slot);
			}
		});
		pisex::DisplayPlayerMenu(menu, slot);
	});
}

void LrPlugin::ShowRanksMenu(int slot) {
	pisex::Menu menu;
	pisex::SetTitleMenu(menu, Phrase("MainMenu_Ranks").c_str());
	const int max_ranks = static_cast<int>(m_settings.rank_names.size());
	if (max_ranks > 0) {
		pisex::AddItemMenu(menu, "", Phrase(m_settings.rank_names[0]).c_str(), pisex::ITEM_DISABLED);
		for (int i = 1; i < max_ranks; ++i) {
			const std::string text = pisex::FormatString("[%i] %s", m_settings.rank_exp[static_cast<std::size_t>(i)],
														 Phrase(m_settings.rank_names[static_cast<std::size_t>(i)]).c_str());
			pisex::AddItemMenu(menu, "", text.c_str(), pisex::ITEM_DISABLED);
		}
	}
	pisex::SetExitMenu(menu, true);
	pisex::SetBackMenu(menu, true);
	pisex::SetCallback(menu, [](const char*, const char*, int item, int player_slot) {
		if (item == 7) {
			g_lr_plugin.ShowMainMenu(player_slot);
		}
	});
	pisex::DisplayPlayerMenu(menu, slot);
}

// --- the admin menu (give/take experience) ----------------------------------------------------------------

void LrPlugin::ShowAdminMenu(int slot) {
	pisex::Menu menu;
	pisex::SetTitleMenu(menu, Phrase("MainMenu_Admin").c_str());
	pisex::AddItemMenu(menu, "0", Phrase("ReloadAllConfigs").c_str());
	pisex::AddItemMenu(menu, "1", Phrase("GiveTakeMenuExp").c_str());
	pisex::SetExitMenu(menu, true);
	pisex::SetBackMenu(menu, true);
	pisex::SetCallback(menu, [](const char* back, const char*, int item, int player_slot) {
		if (item < 7) {
			switch (std::atoi(back)) {
				case 0:
					g_lr_plugin.ReloadConfig();
					pisex::PrintToChat(player_slot, "%s", g_lr_plugin.Phrase("ConfigUpdated").c_str());
					g_lr_plugin.ShowMainMenu(player_slot);
					break;
				case 1: g_lr_plugin.ShowGiveTakeMenu(player_slot); break;
				default: break;
			}
		} else if (item == 7) {
			g_lr_plugin.ShowMainMenu(player_slot);
		}
	});
	pisex::DisplayPlayerMenu(menu, slot);
}

void LrPlugin::ShowGiveTakeMenu(int slot) {
	pisex::Menu menu;
	pisex::SetTitleMenu(menu, Phrase("GiveTakeMenuExp").c_str());
	for (int other = 0; other < kMaxSlots; ++other) {
		const PlayerInfo* info = Core()->FindPlayer(other);
		if (info == nullptr || !info->initialized) {
			continue;
		}
		const std::string text =
			pisex::FormatString("%s (%i)", pisex::g_pPlayers->GetPlayerName(other), info->stats[ST_EXP]);
		pisex::AddItemMenu(menu, std::to_string(other).c_str(), text.c_str());
	}
	pisex::SetExitMenu(menu, true);
	pisex::SetBackMenu(menu, true);
	pisex::SetCallback(menu, [](const char* back, const char*, int item, int player_slot) {
		if (item < 7) {
			g_lr_plugin.ShowGiveTakeValues(player_slot, std::atoi(back));
		} else if (item == 7) {
			g_lr_plugin.ShowAdminMenu(player_slot);
		}
	});
	pisex::DisplayPlayerMenu(menu, slot);
}

void LrPlugin::ShowGiveTakeValues(int slot, int target) {
	const PlayerInfo* target_info = Core()->FindPlayer(target);
	if (target_info == nullptr || !target_info->initialized) {
		return;
	}
	const std::string title =
		pisex::FormatString("%s (%i)", pisex::g_pPlayers->GetPlayerName(target), target_info->stats[ST_EXP]);

	pisex::Menu menu;
	pisex::SetTitleMenu(menu, title.c_str());
	pisex::AddItemMenu(menu, "10", "10");
	pisex::AddItemMenu(menu, "100", "100");
	pisex::AddItemMenu(menu, "1000", "1000");
	pisex::AddItemMenu(menu, "-1000", "-1000");
	pisex::AddItemMenu(menu, "-100", "-100");
	pisex::AddItemMenu(menu, "-10", "-10");
	pisex::SetExitMenu(menu, true);
	pisex::SetBackMenu(menu, true);
	pisex::SetCallback(menu, [target](const char* back, const char*, int item, int player_slot) {
		if (item < 7) {
			const int value = std::atoi(back);
			const char* phrase = value > 0 ? "AdminGive" : "AdminTake";
			// NotifClient(iTarget, iValue, phrase, true) of the original - the "true" bypasses the
			// statistic gate, an admin action always counts.
			if (g_lr_plugin.Core()->GiveExp(target, value, phrase, /*allow=*/true).status) {
				const PlayerInfo* info = g_lr_plugin.Core()->FindPlayer(target);
				const int new_exp = info != nullptr ? info->stats[ST_EXP] : 0;
				pisex::PrintToChat(player_slot, "%s %s",
								   g_lr_plugin.Phrase("Prefix").c_str(),
								   pisex::FormatString(g_lr_plugin.Phrase("ExpChange").c_str(),
													   pisex::g_pPlayers->GetPlayerName(target), new_exp,
													   value > 0 ? "+" : "", value)
									   .c_str());
				pisex::LogToFile("levels_ranks", "%s give/take %i exp to %s",
								 pisex::g_pPlayers->GetPlayerName(player_slot), value,
								 pisex::g_pPlayers->GetPlayerName(target));
			}
			g_lr_plugin.ShowGiveTakeValues(player_slot, target);
		} else if (item == 7) {
			g_lr_plugin.ShowGiveTakeMenu(player_slot);
		}
	});
	pisex::DisplayPlayerMenu(menu, slot);
}

} // namespace levels_ranks
