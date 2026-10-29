# Tarkov Hideout for Switch

A hideout companion for Escape From Tarkov that runs as Nintendo Switch homebrew.
It shows every hideout station, the items each level needs, what you still have
to find across the whole hideout, and the crafts you can run, with craft timers.

It never talks to Battlestate's servers. Game data comes from
[tarkov.dev](https://tarkov.dev), and your progress can be pulled from
[TarkovTracker](https://tarkovtracker.org).

## Install

1. Download `TarkovHideout.nro` from the latest release.
2. Copy it to `sdmc:/switch/` on your SD card.
3. Launch it from the Homebrew Menu. If the on-screen keyboard for the token
   doesn't open, launch through title override (hold R while starting a game).

Progress and settings are saved in `sdmc:/switch/TarkovHideout/`.

## TarkovTracker sync

1. On tarkovtracker.org, open Settings and create an API token with the
   **Get Progression** permission. Tokens are tied to a game mode, so make a
   PVE token if you play PVE.
2. In the app, open Settings, choose the token row and enter the token.
3. The app syncs right away and on every launch (you can turn that off).

TarkovTracker's API is read-only for the hideout, so sync replaces the app's
hideout progress with what TarkovTracker has. Changes you make in the app stay
on the Switch until the next sync overwrites them.

## Controls

| Where | Button | Action |
| --- | --- | --- |
| Anywhere | L / R | Switch tab |
| Anywhere | + | Quit |
| Stations | Up / Down | Pick a station |
| Stations | ZL / ZR | View previous / next level |
| Stations | A | Mark the shown level as built |
| Stations | X | Undo the last built level |
| Stations | Right | Edit item counts for the shown level |
| Item counts | A / X / Y | +1 / -1 / all or none |
| Needed items | Left / Right | Page through the list |
| Crafts | Left / Right | Filter by station |
| Crafts | A | Start a timer, or collect a finished one |
| Crafts, Timers | X | Cancel a timer |

## Data

`.github/workflows/data.yml` runs daily. It pulls the full hideout, item, craft
and trader data from json.tarkov.dev, resolves names and publishes a small file
per game mode to the `data` branch. The app downloads that file, since the full
item catalogue is too large to fetch on the Switch. Release builds also bundle
a copy for offline use.

## Building

```sh
python3 tools/build_data.py romfs   # optional, bundles offline data
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make
```

Not affiliated with Battlestate Games, tarkov.dev or TarkovTracker.
