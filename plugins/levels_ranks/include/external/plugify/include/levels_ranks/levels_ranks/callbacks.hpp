#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from levels_ranks.pplugin (group: callbacks)

namespace levels_ranks {

  using _OnCoreIsReady_Register = void (*)(CoreReadyCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnCoreIsReady_Register __levels_ranks_OnCoreIsReady_Register;
namespace levels_ranks {
  /**
   * @brief Registers the (single) OnCoreIsReady listener of the calling plugin - the HookOnCoreIsReady of the original, whose PluginId argument is gone: every plugin registers one function per event and registering the same function pointer again is a no-op. The listener fires once, when the database is up and settings.ini was read; a plugin that registers after that must check IsCoreLoaded() itself, because the event does not fire twice.
   * @param callback (function): Function to invoke.
   */
  inline void OnCoreIsReady_Register(CoreReadyCallback callback) {
    return __levels_ranks_OnCoreIsReady_Register(callback);
  }

  using _OnCoreIsReady_Unregister = void (*)(CoreReadyCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnCoreIsReady_Unregister __levels_ranks_OnCoreIsReady_Unregister;
namespace levels_ranks {
  /**
   * @brief Removes the OnCoreIsReady listener previously registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnCoreIsReady_Unregister(CoreReadyCallback callback) {
    return __levels_ranks_OnCoreIsReady_Unregister(callback);
  }

  using _OnLevelChangedPre_Register = void (*)(LevelChangedPreCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnLevelChangedPre_Register __levels_ranks_OnLevelChangedPre_Register;
namespace levels_ranks {
  /**
   * @brief Registers the listener that fires whenever a player's rank changes, before the new rank is stored. newLevel is passed by reference: a module may replace it (the change is honoured by the core, exactly like the int& of the original typedef) while iOldLevel is read-only.
   * @param callback (function): Function to invoke.
   */
  inline void OnLevelChangedPre_Register(LevelChangedPreCallback callback) {
    return __levels_ranks_OnLevelChangedPre_Register(callback);
  }

  using _OnLevelChangedPre_Unregister = void (*)(LevelChangedPreCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnLevelChangedPre_Unregister __levels_ranks_OnLevelChangedPre_Unregister;
namespace levels_ranks {
  /**
   * @brief Removes the OnLevelChangedPre listener previously registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnLevelChangedPre_Unregister(LevelChangedPreCallback callback) {
    return __levels_ranks_OnLevelChangedPre_Unregister(callback);
  }

  using _OnLevelChangedPost_Register = void (*)(LevelChangedPostCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnLevelChangedPost_Register __levels_ranks_OnLevelChangedPost_Register;
namespace levels_ranks {
  /**
   * @brief Registers the listener that fires after a rank change was applied (the rank stored, the messages printed and, with lr_db_savedataplayer_mode, the row saved).
   * @param callback (function): Function to invoke.
   */
  inline void OnLevelChangedPost_Register(LevelChangedPostCallback callback) {
    return __levels_ranks_OnLevelChangedPost_Register(callback);
  }

  using _OnLevelChangedPost_Unregister = void (*)(LevelChangedPostCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnLevelChangedPost_Unregister __levels_ranks_OnLevelChangedPost_Unregister;
namespace levels_ranks {
  /**
   * @brief Removes the OnLevelChangedPost listener previously registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnLevelChangedPost_Unregister(LevelChangedPostCallback callback) {
    return __levels_ranks_OnLevelChangedPost_Unregister(callback);
  }

  using _OnPlayerKilledPre_Register = void (*)(PlayerKilledPreCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnPlayerKilledPre_Register __levels_ranks_OnPlayerKilledPre_Register;
namespace levels_ranks {
  /**
   * @brief Registers the listener that fires while a kill is being rated, before the experience is applied: the LR modules use it to scale expCaused (VIP multiplier, name reward) or to inspect the kill (weapon). The original passed IGameEvent*; Plugify cannot put an engine pointer behind its own type check, so the s2sdk event handle crosses as int64 and lr_api.hpp rebuilds the pisex::IGameEvent* the modules are written against (valid for the duration of the callback). expCaused is by reference and the change is honoured; expVictim/expAttacker are the victims'/attacker's experience before the change.
   * @param callback (function): Function to invoke.
   */
  inline void OnPlayerKilledPre_Register(PlayerKilledPreCallback callback) {
    return __levels_ranks_OnPlayerKilledPre_Register(callback);
  }

  using _OnPlayerKilledPre_Unregister = void (*)(PlayerKilledPreCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnPlayerKilledPre_Unregister __levels_ranks_OnPlayerKilledPre_Unregister;
namespace levels_ranks {
  /**
   * @brief Removes the OnPlayerKilledPre listener previously registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnPlayerKilledPre_Unregister(PlayerKilledPreCallback callback) {
    return __levels_ranks_OnPlayerKilledPre_Unregister(callback);
  }

  using _OnPlayerKilledPost_Register = void (*)(PlayerKilledPostCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnPlayerKilledPost_Register __levels_ranks_OnPlayerKilledPost_Register;
namespace levels_ranks {
  /**
   * @brief Registers the listener that fires after a kill was rated and applied (kills, deaths, headshots and streaks counted). Same event handle and arguments as OnPlayerKilledPre, expCaused is the value that was applied.
   * @param callback (function): Function to invoke.
   */
  inline void OnPlayerKilledPost_Register(PlayerKilledPostCallback callback) {
    return __levels_ranks_OnPlayerKilledPost_Register(callback);
  }

  using _OnPlayerKilledPost_Unregister = void (*)(PlayerKilledPostCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnPlayerKilledPost_Unregister __levels_ranks_OnPlayerKilledPost_Unregister;
namespace levels_ranks {
  /**
   * @brief Removes the OnPlayerKilledPost listener previously registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnPlayerKilledPost_Unregister(PlayerKilledPostCallback callback) {
    return __levels_ranks_OnPlayerKilledPost_Unregister(callback);
  }

  using _OnPlayerLoaded_Register = void (*)(PlayerLoadedCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnPlayerLoaded_Register __levels_ranks_OnPlayerLoaded_Register;
namespace levels_ranks {
  /**
   * @brief Registers the listener that fires when the statistics of a player were loaded (an existing row) or created (a new row inserted). The steamid is the converted STEAM_1:x:y string the original's g_iPlayerInfo[slot].szAuth held.
   * @param callback (function): Function to invoke.
   */
  inline void OnPlayerLoaded_Register(PlayerLoadedCallback callback) {
    return __levels_ranks_OnPlayerLoaded_Register(callback);
  }

  using _OnPlayerLoaded_Unregister = void (*)(PlayerLoadedCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnPlayerLoaded_Unregister __levels_ranks_OnPlayerLoaded_Unregister;
namespace levels_ranks {
  /**
   * @brief Removes the OnPlayerLoaded listener previously registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnPlayerLoaded_Unregister(PlayerLoadedCallback callback) {
    return __levels_ranks_OnPlayerLoaded_Unregister(callback);
  }

  using _OnResetPlayerStats_Register = void (*)(ResetPlayerStatsCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnResetPlayerStats_Register __levels_ranks_OnResetPlayerStats_Register;
namespace levels_ranks {
  /**
   * @brief Registers the listener that fires after the statistics of a player were reset (the 'reset my stats' menu entry or the mm_lvl_reset command path that reloads the player). Modules use it to clear their own per-player tables.
   * @param callback (function): Function to invoke.
   */
  inline void OnResetPlayerStats_Register(ResetPlayerStatsCallback callback) {
    return __levels_ranks_OnResetPlayerStats_Register(callback);
  }

  using _OnResetPlayerStats_Unregister = void (*)(ResetPlayerStatsCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnResetPlayerStats_Unregister __levels_ranks_OnResetPlayerStats_Unregister;
namespace levels_ranks {
  /**
   * @brief Removes the OnResetPlayerStats listener previously registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnResetPlayerStats_Unregister(ResetPlayerStatsCallback callback) {
    return __levels_ranks_OnResetPlayerStats_Unregister(callback);
  }

  using _OnPlayerPosInTop_Register = void (*)(PlayerPosInTopCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnPlayerPosInTop_Register __levels_ranks_OnPlayerPosInTop_Register;
namespace levels_ranks {
  /**
   * @brief Registers the listener for the place-in-top refresh: after every save of a loaded player the core queries the player's position by experience and by playtime and reports both. This is the hook the original fired from the SaveDataPlayer callback.
   * @param callback (function): Function to invoke.
   */
  inline void OnPlayerPosInTop_Register(PlayerPosInTopCallback callback) {
    return __levels_ranks_OnPlayerPosInTop_Register(callback);
  }

  using _OnPlayerPosInTop_Unregister = void (*)(PlayerPosInTopCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnPlayerPosInTop_Unregister __levels_ranks_OnPlayerPosInTop_Unregister;
namespace levels_ranks {
  /**
   * @brief Removes the OnPlayerPosInTop listener previously registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnPlayerPosInTop_Unregister(PlayerPosInTopCallback callback) {
    return __levels_ranks_OnPlayerPosInTop_Unregister(callback);
  }

  using _OnExpChangedPre_Register = void (*)(ExpChangedPreCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnExpChangedPre_Register __levels_ranks_OnExpChangedPre_Register;
namespace levels_ranks {
  /**
   * @brief Registers the listener that fires before experience is added to a player (every NotifClient call, i.e. kills, assists, rounds, bombs, hostages, the admin give/take menu and the module's own ChangeClientValue path). giveExp is by reference and a module may replace it; a value changed to 0 makes the core skip the change, exactly like the original.
   * @param callback (function): Function to invoke.
   */
  inline void OnExpChangedPre_Register(ExpChangedPreCallback callback) {
    return __levels_ranks_OnExpChangedPre_Register(callback);
  }

  using _OnExpChangedPre_Unregister = void (*)(ExpChangedPreCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnExpChangedPre_Unregister __levels_ranks_OnExpChangedPre_Unregister;
namespace levels_ranks {
  /**
   * @brief Removes the OnExpChangedPre listener previously registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnExpChangedPre_Unregister(ExpChangedPreCallback callback) {
    return __levels_ranks_OnExpChangedPre_Unregister(callback);
  }

  using _OnExpChangedPost_Register = void (*)(ExpChangedPostCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnExpChangedPost_Register __levels_ranks_OnExpChangedPost_Register;
namespace levels_ranks {
  /**
   * @brief Registers the listener that fires after experience was applied to a player: giveExp is the amount actually applied (clamping and floor included, exactly the iExpBuffer of the original) and newExpCount the player's new total.
   * @param callback (function): Function to invoke.
   */
  inline void OnExpChangedPost_Register(ExpChangedPostCallback callback) {
    return __levels_ranks_OnExpChangedPost_Register(callback);
  }

  using _OnExpChangedPost_Unregister = void (*)(ExpChangedPostCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnExpChangedPost_Unregister __levels_ranks_OnExpChangedPost_Unregister;
namespace levels_ranks {
  /**
   * @brief Removes the OnExpChangedPost listener previously registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnExpChangedPost_Unregister(ExpChangedPostCallback callback) {
    return __levels_ranks_OnExpChangedPost_Unregister(callback);
  }

  using _OnDatabaseCleanup_Register = void (*)(DatabaseCleanupCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnDatabaseCleanup_Register __levels_ranks_OnDatabaseCleanup_Register;
namespace levels_ranks {
  /**
   * @brief Registers the listener that fires when the mm_lvl_reset command wiped the statistics table, so modules can wipe their own <table>_hits/_weapons/... tables with the same type. type is 0 = all data, 1 = experience, 2 = statistics.
   * @param callback (function): Function to invoke.
   */
  inline void OnDatabaseCleanup_Register(DatabaseCleanupCallback callback) {
    return __levels_ranks_OnDatabaseCleanup_Register(callback);
  }

  using _OnDatabaseCleanup_Unregister = void (*)(DatabaseCleanupCallback);
}
extern "C" PLUGIN_API levels_ranks::_OnDatabaseCleanup_Unregister __levels_ranks_OnDatabaseCleanup_Unregister;
namespace levels_ranks {
  /**
   * @brief Removes the OnDatabaseCleanup listener previously registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnDatabaseCleanup_Unregister(DatabaseCleanupCallback callback) {
    return __levels_ranks_OnDatabaseCleanup_Unregister(callback);
  }

} // namespace levels_ranks
