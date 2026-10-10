{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* hudmenu binding slots (defined by hudmenu_bindings through pisexcompat, filled by the
           plugify module from the hudmenu plugin's exported GetClientMenuType/SetClientMenuType) */
        __hudmenu_*;

        /* permissions binding slots (defined by permissions_bindings, filled by the plugify module
           from the official permissions plugin): the LR cookies (LR.ResetMyStatsCooldown) live in
           the permissions user cookies. */
        __permissions_*;

        /* Exported methods (levels_ranks.pplugin, group by group). Everything in
           plugins/levels_ranks/include/external/plugify/include is generated from the manifest with
           bin/plugify-gen - keep this list in sync. */
        IsCoreLoaded;
        GetSettingsValue;
        GetSettingsStatsValue;
        GetCountPlayers;
        GetTableName;
        GetRankNames;
        GetRankExp;
        GetClientStatus;
        CheckCountPlayers;
        GetClientInfo;
        RoundWithoutValue;
        ChangeClientValue;
        ResetPlayerStats;
        GetDatabaseConfig;
        PrintToChat;

        OnCoreIsReady_Register;
        OnCoreIsReady_Unregister;
        OnLevelChangedPre_Register;
        OnLevelChangedPre_Unregister;
        OnLevelChangedPost_Register;
        OnLevelChangedPost_Unregister;
        OnPlayerKilledPre_Register;
        OnPlayerKilledPre_Unregister;
        OnPlayerKilledPost_Register;
        OnPlayerKilledPost_Unregister;
        OnPlayerLoaded_Register;
        OnPlayerLoaded_Unregister;
        OnResetPlayerStats_Register;
        OnResetPlayerStats_Unregister;
        OnPlayerPosInTop_Register;
        OnPlayerPosInTop_Unregister;
        OnExpChangedPre_Register;
        OnExpChangedPre_Unregister;
        OnExpChangedPost_Register;
        OnExpChangedPost_Unregister;
        OnDatabaseCleanup_Register;
        OnDatabaseCleanup_Unregister;

    local: *;
};
