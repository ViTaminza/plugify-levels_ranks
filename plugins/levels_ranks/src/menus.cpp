// The menu tree of the Pisex cs2-lvl_ranks core, built on the native s2sdk menu layer:
//
//   OnLRMenu            -> ShowMainMenu       (rank, experience, place, my stats, tops, ranks, admin)
//   MyStats             -> ShowMyStats        (+ ShowSessionStats, ShowStatsReset)
//   MenuTops            -> ShowTopsMenu       -> OverAllTopPlayers (exp / play time)
//   OverAllRanks        -> ShowRanksMenu
//   MenuAdmin           -> ShowAdminMenu      (reload configs, give/take experience)
//   GiveTakeValue       -> ShowGiveTakeMenu   -> MenuValues (ShowGiveTakeValues)
//
// The item numbers of the original are preserved: 1..5 are the selectable items of the visible
// page and 7 is the "back" cancel reason. Everything the original switched on is kept verbatim,
// including the disabled info rows and the placeholders of the phrases (they are printed with the
// engine's printf formatting). Every action is queued by lr_menus and re-validated against the
// SteamID64 + controller handle of the player it was built for.

#include "lr_plugin.hpp"

#include "lr_menus.hpp"
#include "lr_phrases.hpp"

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <string>

namespace levels_ranks {

namespace {

// RoundToCeil() of the original (the KDR/accuracy/winrate percentages round up).
int RoundToCeil(double value) {
	return static_cast<int>(std::ceil(value));
}

MenuPlan NewPlan(LrPlugin& plugin, std::string title, MenuSelectHandler select, MenuBackHandler back,
				 MenuContext context = {}) {
	MenuPlan plan;
	plan.title = std::move(title);
	plan.onSelect = select;
	plan.onBack = back;
	plan.user = &plugin;
	plan.context = context;
	return plan;
}

void AddInfo(MenuPlan& plan, std::string text) {
	plan.items.push_back(MenuItemPlan{"", std::move(text), /*disabled=*/true});
}

void AddAction(MenuPlan& plan, std::string info, std::string text) {
	plan.items.push_back(MenuItemPlan{std::move(info), std::move(text), /*disabled=*/false});
}

bool ShowPlan(LrPlugin& plugin, int slot, MenuPlan plan) {
	OwnerIdentity owner;
	if (!CaptureOwner(slot, owner)) {
		return false;
	}
	plan.user = &plugin;
	return EnqueueMenu(owner, std::move(plan));
}

bool ShowPlan(LrPlugin& plugin, const OwnerIdentity& owner, MenuPlan plan) {
	plan.user = &plugin;
	return EnqueueMenu(owner, std::move(plan));
}

void BackToMain(void* user, const OwnerIdentity& owner, const MenuContext& /*context*/) {
	static_cast<LrPlugin*>(user)->ShowMainMenu(owner.slot);
}

void BackToAdmin(void* user, const OwnerIdentity& owner, const MenuContext& /*context*/) {
	static_cast<LrPlugin*>(user)->ShowAdminMenu(owner.slot);
}

void BackToMyStats(void* user, const OwnerIdentity& owner, const MenuContext& /*context*/) {
	static_cast<LrPlugin*>(user)->ShowMyStats(owner.slot);
}

void BackToGiveTake(void* user, const OwnerIdentity& owner, const MenuContext& /*context*/) {
	static_cast<LrPlugin*>(user)->ShowGiveTakeMenu(owner.slot);
}

// --- the main menu ---------------------------------------------------------------------------------

void MainSelect(void* user, const OwnerIdentity& owner, const MenuContext& /*context*/,
				const std::string& info, int32_t /*item*/) {
	LrPlugin* plugin = static_cast<LrPlugin*>(user);
	switch (std::atoi(info.c_str())) {
	case 0:
		plugin->ShowAdminMenu(owner.slot);
		break;
	case 1:
		plugin->ShowMyStats(owner.slot);
		break;
	case 3:
		plugin->ShowTopsMenu(owner.slot);
		break;
	case 4:
		plugin->ShowRanksMenu(owner.slot);
		break;
	default:
		break;
	}
}

void MyStatsSelect(void* user, const OwnerIdentity& owner, const MenuContext& /*context*/,
				   const std::string& info, int32_t /*item*/) {
	LrPlugin* plugin = static_cast<LrPlugin*>(user);
	switch (std::atoi(info.c_str())) {
	case 0:
		plugin->ShowSessionStats(owner.slot);
		break;
	case 2:
		plugin->ShowStatsReset(owner.slot);
		break;
	default:
		break;
	}
}

void SessionStatsSelect(void* /*user*/, const OwnerIdentity& /*owner*/, const MenuContext& /*context*/,
						const std::string& /*info*/, int32_t /*item*/) {}

void StatsResetSelect(void* user, const OwnerIdentity& owner, const MenuContext& /*context*/,
					  const std::string& info, int32_t /*item*/) {
	LrPlugin* plugin = static_cast<LrPlugin*>(user);
	switch (std::atoi(info.c_str())) {
	case 0:
		plugin->ShowMyStats(owner.slot);
		break;
	case 1:
		// ResetMenuHandle() of the original: the cooldown cookie and the reset.
		plugin->SetCookie(owner.slot, "ResetMyStatsCooldown",
						  std::to_string(std::time(nullptr) + plugin->Cfg().Value(LR_ResetMyStatsCooldown)));
		plugin->ResetPlayerStats(owner.slot);
		break;
	default:
		break;
	}
}

void TopsSelect(void* user, const OwnerIdentity& owner, const MenuContext& /*context*/,
				const std::string& info, int32_t /*item*/) {
	LrPlugin* plugin = static_cast<LrPlugin*>(user);
	switch (std::atoi(info.c_str())) {
	case 0:
		plugin->ShowTopPlayers(owner.slot, false);
		break;
	case 1:
		plugin->ShowTopPlayers(owner.slot, true);
		break;
	default:
		break;
	}
}

void RanksSelect(void* /*user*/, const OwnerIdentity& /*owner*/, const MenuContext& /*context*/,
				 const std::string& /*info*/, int32_t /*item*/) {}

void AdminSelect(void* user, const OwnerIdentity& owner, const MenuContext& /*context*/,
				 const std::string& info, int32_t /*item*/) {
	LrPlugin* plugin = static_cast<LrPlugin*>(user);
	// The admin page is only actionable while the grant still belongs to this exact player.
	if (!plugin->HasAdminAccess(owner.slot)) {
		return;
	}
	switch (std::atoi(info.c_str())) {
	case 0:
		plugin->ReloadConfig();
		plugin->PrintToChatRaw(owner.slot, plugin->Phrase("ConfigUpdated"));
		plugin->ShowMainMenu(owner.slot);
		break;
	case 1:
		plugin->ShowGiveTakeMenu(owner.slot);
		break;
	default:
		break;
	}
}

void GiveTakeSelect(void* user, const OwnerIdentity& owner, const MenuContext& /*context*/,
					const std::string& info, int32_t /*item*/) {
	LrPlugin* plugin = static_cast<LrPlugin*>(user);
	if (!plugin->HasAdminAccess(owner.slot)) {
		return;
	}
	plugin->ShowGiveTakeValues(owner.slot, std::atoi(info.c_str()));
}

void GiveTakeValuesSelect(void* user, const OwnerIdentity& owner, const MenuContext& context,
						  const std::string& info, int32_t /*item*/) {
	LrPlugin* plugin = static_cast<LrPlugin*>(user);
	// Re-check the admin grant and the target in the same tick the action runs: the grant may have
	// been revoked and the slot re-used between the menu display and this selection.
	if (!plugin->HasAdminAccess(owner.slot) || !context.target.IsSet() ||
		!OwnerMatches(context.target) || !plugin->CheckStatus(context.target.slot) ||
		plugin->Core() == nullptr) {
		return;
	}
	const int target = context.target.slot;
	const int value = std::atoi(info.c_str());
	const char* phrase = value > 0 ? "AdminGive" : "AdminTake";
	// NotifClient(iTarget, iValue, phrase, true) of the original - the "true" bypasses the
	// statistic gate, an admin action always counts.
	if (plugin->Core()->GiveExp(target, value, phrase, /*allow=*/true).status) {
		const PlayerInfo* target_info = plugin->Core()->FindPlayer(target);
		const int new_exp = target_info != nullptr ? target_info->stats[ST_EXP] : 0;
		plugin->PrintToChat(owner.slot,
							FormatString(plugin->Phrase("ExpChange").c_str(),
										 plugin->PlayerNameOf(target).c_str(), new_exp,
										 value > 0 ? "+" : "", value));
		LogToFile("levels_ranks", "%s give/take %i exp to %s", plugin->PlayerNameOf(owner.slot).c_str(), value,
				  plugin->PlayerNameOf(target).c_str());
	}
	plugin->ShowGiveTakeValues(owner.slot, target);
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

	MenuPlan plan = NewPlan(*this, m_settings.plugin_title, &MainSelect, nullptr);
	AddInfo(plan, FormatString(Phrase("MainMenuRank").c_str(), rank_name.c_str()));
	AddInfo(plan, FormatString(Phrase("MainMenuExp").c_str(), exp_text.c_str()));
	AddInfo(plan, FormatString(Phrase("MainMenuPlace").c_str(), info->stats[ST_PLACEINTOP], Core()->PlayerCount()));
	AddAction(plan, "1", Phrase("MainMenu_MyStats"));
	AddAction(plan, "3", Phrase("MainMenu_TopPlayers"));
	if (m_settings.Value(LR_ShowRankList) != 0) {
		AddAction(plan, "4", Phrase("MainMenu_Ranks"));
	}
	if (HasAdminAccess(slot)) {
		AddAction(plan, "0", Phrase("MainMenu_Admin"));
	}
	plan.exitButton = true;
	plan.exitBackButton = false;
	ShowPlan(*this, slot, std::move(plan));
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

	MenuPlan plan = NewPlan(*this, Phrase("MyStatsInfoTitle"), &MyStatsSelect, &BackToMain);
	AddAction(plan, "0", Phrase("MyStatsSession"));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoPlayed").c_str(), play_time / 3600, play_time / 60 % 60,
							   play_time % 60));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoKills").c_str(), kills));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoDeaths").c_str(), deaths));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoAssists").c_str(), info->stats[ST_ASSISTS]));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoHeadshots").c_str(), headshots,
							   RoundToCeil(100.0 / (kills ? kills : 1) * headshots)));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoKDR").c_str(), kills / (deaths ? float(deaths) : 1.0f)));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoAccuracy").c_str(),
							   RoundToCeil(100.0 / (shots ? float(shots) : 1.0f) * info->session[ST_HITS])));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoWinRate").c_str(),
							   RoundToCeil(100.0 / (rounds_all ? float(rounds_all) : 1.0f) * rounds_win)));

	if (m_settings.Value(LR_ShowResetMyStats) != 0) {
		const std::string cooldown_text = Cookie(slot, "ResetMyStatsCooldown");
		int cooldown = cooldown_text.empty() ? 0 : std::atoi(cooldown_text.c_str());
		if (cooldown == 0 || cooldown < static_cast<int>(std::time(nullptr))) {
			AddAction(plan, "2", Phrase("MyStatsReset"));
		} else {
			cooldown -= static_cast<int>(std::time(nullptr));
			AddInfo(plan, FormatString(Phrase("MyStatsResetCooldown").c_str(), cooldown / 3600, cooldown / 60 % 60));
		}
	}

	plan.exitButton = true;
	plan.exitBackButton = true;
	ShowPlan(*this, slot, std::move(plan));
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

	MenuPlan plan = NewPlan(*this, Phrase("MyStatsSessionInfoTitle"), &SessionStatsSelect, &BackToMyStats);
	AddInfo(plan, FormatString(Phrase("MyStatsSessionInfoExp").c_str(), exp_value.c_str()));
	AddInfo(plan, FormatString(Phrase("MyStatsSessionInfoPlace").c_str(), place_value.c_str()));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoPlayed").c_str(), play_time / 3600, play_time / 60 % 60,
							   play_time % 60));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoKills").c_str(), kills));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoDeaths").c_str(), deaths));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoAssists").c_str(), info->session[ST_ASSISTS]));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoHeadshots").c_str(), headshots,
							   RoundToCeil(100.0 / (kills ? kills : 1) * headshots)));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoKDR").c_str(), kills / (deaths ? float(deaths) : 1.0f)));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoAccuracy").c_str(),
							   RoundToCeil(100.0 / (shots ? float(shots) : 1.0f) * info->session[ST_HITS])));
	AddInfo(plan, FormatString(Phrase("MyStatsInfoWinRate").c_str(),
							   RoundToCeil(100.0 / (rounds_all ? float(rounds_all) : 1.0f) * rounds_win)));
	plan.exitButton = true;
	plan.exitBackButton = true;
	ShowPlan(*this, slot, std::move(plan));
}

void LrPlugin::ShowStatsReset(int slot) {
	// MyStatsReset() of the original: the warning page, then "Yes"/"No".
	MenuPlan plan = NewPlan(*this, Phrase("MyStatsResetInfoTitle"), &StatsResetSelect, &BackToMyStats);
	AddInfo(plan, Phrase("MyStatsResetInfo1"));
	AddInfo(plan, Phrase("MyStatsResetInfo2"));
	AddInfo(plan, Phrase("MyStatsResetInfo3"));
	AddAction(plan, "1", Phrase("Yes"));
	AddAction(plan, "0", Phrase("No"));
	plan.exitButton = true;
	plan.exitBackButton = true;
	ShowPlan(*this, slot, std::move(plan));
}

// --- the tops -------------------------------------------------------------------------------------------

void LrPlugin::ShowTopsMenu(int slot) {
	MenuPlan plan = NewPlan(*this, Phrase("MainMenu_TopPlayers"), &TopsSelect, &BackToMain);
	AddAction(plan, "0", Phrase("OverAllTopPlayersExp"));
	AddAction(plan, "1", Phrase("OverAllTopPlayersTime"));
	plan.exitButton = true;
	plan.exitBackButton = true;
	ShowPlan(*this, slot, std::move(plan));
}

void LrPlugin::ShowTopPlayers(int slot, bool by_playtime) {
	if (slot >= 0 && slot < kMaxSlots) {
		// Cooldown per player and per list: !top repeated by a key bind is throttled, while !toptime
		// (or the other entry of the menu) still works right after !top.
		const std::size_t list = by_playtime ? 1 : 0;
		int64_t& last_request = m_last_top_request[static_cast<std::size_t>(slot)][list];
		const int64_t now = static_cast<int64_t>(std::time(nullptr));
		if (now - last_request < kCommandCooldownSeconds) {
			return; // the core caches each list for kTopCacheSeconds as well
		}
		last_request = now;
	}
	if (!IsMapReady()) {
		return;
	}
	if (!CheckStatus(slot)) {
		return;
	}
	OwnerIdentity owner;
	if (!CaptureOwner(slot, owner)) {
		return;
	}
	const std::string own_steamid = SteamIdOf(slot);
	Core()->LoadTopPlayers(by_playtime, [this, slot, by_playtime, own_steamid,
										 owner](std::vector<TopRow> rows) {
		if (!OwnerMatches(owner) || !CheckStatus(slot)) {
			return; // the request's player left or the slot was re-used while the query ran
		}
		if (rows.empty()) {
			// The original printed NoData when the query returned no row.
			PrintToChat(slot, Phrase("NoData"));
			return;
		}
		const char* title_key = by_playtime ? "OverAllTopPlayersTime" : "OverAllTopPlayersExp";
		const char* slot_key = by_playtime ? "OverAllTopPlayersTime_Slot" : "OverAllTopPlayersExp_Slot";

		MenuPlan plan = NewPlan(*this, std::string(m_settings.plugin_title) + " | " + Phrase(title_key),
								nullptr, &BackToMain);
		int position = 1;
		for (const TopRow& row : rows) {
			std::string entry = row.steamid == own_steamid
									? std::string(Phrase(slot_key)) + " " + Phrase("You")
									: std::string(Phrase(slot_key));
			const std::string text = by_playtime
										 ? FormatString(entry.c_str(), position, row.value, row.name.c_str())
										 : FormatString(entry.c_str(), position, static_cast<int>(row.value),
														row.name.c_str());
			AddInfo(plan, text);
			++position;
		}
		plan.exitButton = true;
		plan.exitBackButton = true;
		ShowPlan(*this, owner, std::move(plan));
	});
}

void LrPlugin::ShowRanksMenu(int slot) {
	MenuPlan plan = NewPlan(*this, Phrase("MainMenu_Ranks"), &RanksSelect, &BackToMain);
	const int max_ranks = static_cast<int>(m_settings.rank_names.size());
	if (max_ranks > 0) {
		AddInfo(plan, Phrase(m_settings.rank_names[0]));
		for (int i = 1; i < max_ranks; ++i) {
			AddInfo(plan, FormatString("[%i] %s", m_settings.rank_exp[static_cast<std::size_t>(i)],
									   Phrase(m_settings.rank_names[static_cast<std::size_t>(i)]).c_str()));
		}
	}
	plan.exitButton = true;
	plan.exitBackButton = true;
	ShowPlan(*this, slot, std::move(plan));
}

// --- the admin menu (give/take experience) ----------------------------------------------------------------

void LrPlugin::ShowAdminMenu(int slot) {
	if (!HasAdminAccess(slot)) {
		return;
	}
	MenuPlan plan = NewPlan(*this, Phrase("MainMenu_Admin"), &AdminSelect, &BackToMain);
	AddAction(plan, "0", Phrase("ReloadAllConfigs"));
	AddAction(plan, "1", Phrase("GiveTakeMenuExp"));
	plan.exitButton = true;
	plan.exitBackButton = true;
	ShowPlan(*this, slot, std::move(plan));
}

void LrPlugin::ShowGiveTakeMenu(int slot) {
	if (!HasAdminAccess(slot) || Core() == nullptr) {
		return;
	}
	MenuPlan plan = NewPlan(*this, Phrase("GiveTakeMenuExp"), &GiveTakeSelect, &BackToAdmin);
	for (int other = 0; other < kMaxSlots; ++other) {
		const PlayerInfo* info = Core()->FindPlayer(other);
		if (info == nullptr || !info->initialized) {
			continue;
		}
		const std::string text = FormatString("%s (%i)", PlayerNameOf(other).c_str(), info->stats[ST_EXP]);
		AddAction(plan, std::to_string(other), text);
	}
	plan.exitButton = true;
	plan.exitBackButton = true;
	ShowPlan(*this, slot, std::move(plan));
}

void LrPlugin::ShowGiveTakeValues(int slot, int target) {
	if (!HasAdminAccess(slot) || !IsMapReady() || Core() == nullptr) {
		return;
	}
	const PlayerInfo* target_info = Core()->FindPlayer(target);
	if (target_info == nullptr || !target_info->initialized) {
		return;
	}
	MenuContext context;
	if (!CaptureOwner(target, context.target)) {
		return;
	}
	const std::string title =
		FormatString("%s (%i)", PlayerNameOf(target).c_str(), target_info->stats[ST_EXP]);

	MenuPlan plan = NewPlan(*this, title, &GiveTakeValuesSelect, &BackToGiveTake, context);
	AddAction(plan, "10", "10");
	AddAction(plan, "100", "100");
	AddAction(plan, "1000", "1000");
	AddAction(plan, "-1000", "-1000");
	AddAction(plan, "-100", "-100");
	AddAction(plan, "-10", "-10");
	plan.exitButton = true;
	plan.exitBackButton = true;
	ShowPlan(*this, slot, std::move(plan));
}

} // namespace levels_ranks
