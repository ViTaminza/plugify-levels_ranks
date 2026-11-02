# levels_ranks (Plugify)

Levels Ranks core: experience, ranks, kill/death/round statistics, the LR menus and the plugin-facing ILRApi. A port of the Metamod:Source plugin 'cs2-lvl_ranks' by Pisex; the database layout (one table, name from lr_table, default lvl_base) is kept identical so a Metamod and a Plugify server can share one database. The eight Pisex LR modules (hits, weapons, maps, geoip, unusual kills, fakeranks, namereward, vip multiplier) talk to this plugin through the exported methods below - see API.md and include/lr_api.hpp.

Plugify port (fork) of **cs2-lvl_ranks** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-lvl_ranks
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.3.0
- Plugify dependencies: s2sdk (>=2.18.2), hudmenu (>=1.0.0, optional), permissions (>=1.3.1, optional)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `levels_ranks-1.3.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/levels_ranks`.

## License
GPL-3.0, see LICENSE.

