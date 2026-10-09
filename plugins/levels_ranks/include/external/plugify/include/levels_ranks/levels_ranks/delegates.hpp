#pragma once

#include "enums.hpp"
#include "aliases.hpp"
// Generated from levels_ranks.pplugin

namespace levels_ranks {

  // Called once the LR core is loaded.
  using CoreReadyCallback = void (*)();


  // Called before the rank of a player is changed; may rewrite newLevel.
  using LevelChangedPreCallback = void (*)(int32_t, int32_t&, int32_t);


  // Called after the rank of a player was changed.
  using LevelChangedPostCallback = void (*)(int32_t, int32_t, int32_t);


  // Called before the kill's experience is applied; may rewrite expCaused.
  using PlayerKilledPreCallback = void (*)(int64_t, int32_t&, int32_t, int32_t);


  // Called after the kill's experience was applied.
  using PlayerKilledPostCallback = void (*)(int64_t, int32_t, int32_t, int32_t);


  // Called when a player's statistics became available.
  using PlayerLoadedCallback = void (*)(int32_t, const plg::string&);


  // Called after a player's statistics were reset.
  using ResetPlayerStatsCallback = void (*)(int32_t, const plg::string&);


  // Called with the player's positions after a save.
  using PlayerPosInTopCallback = void (*)(int32_t, int32_t, int32_t);


  // Called before experience is applied; may rewrite giveExp.
  using ExpChangedPreCallback = void (*)(int32_t, int32_t&);


  // Called after experience was applied.
  using ExpChangedPostCallback = void (*)(int32_t, int32_t, int32_t);


  // Called when the LR data was cleared; type is LR_CleanupType (0 all, 1 exp, 2 stats).
  using DatabaseCleanupCallback = void (*)(int32_t);


} // namespace levels_ranks
