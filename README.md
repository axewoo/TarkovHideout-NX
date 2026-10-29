# Tarkov Hideout for Switch

A hideout companion for Escape From Tarkov that runs as Nintendo Switch homebrew.
It shows every hideout station, the items each level needs (with item icons),
what you still have to find across the whole hideout, and the crafts you can
run, with craft timers. It also has interactive maps with extracts, transits
and boss spawns, a database of quests, ammo, keys and items, and a profile page
with your level, raid stats and skills.

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

## Database

The Database tab has four sections (ZL / ZR):

- **Quests**: every quest by trader. A quest page shows the wiki guide on the
  left and, on the right, requirements, keys and starting gear, objectives
  (with Gunsmith build specs) and rewards. Objectives with a known location
  open the map with a pin on it. Quests completed on TarkovTracker are marked.
- **Ammo**: damage, penetration, armor damage, fragmentation, speed and flea
  price, filterable by caliber.
- **Keys**: what each key is for, and where its lock is on the map.
- **Items**: search any item for prices, description and what it is needed for
  (hideout, quests, crafts).

Everything is stored on the SD card, so it works offline. Quest guides are
plain-text extracts from the [Escape from Tarkov Wiki](https://escapefromtarkov.fandom.com),
licensed CC BY-SA.

## Profile

The Profile tab is labelled with your player name and game mode, and shows the public snapshot tarkov.dev keeps of your account.
Search your in-game name on [tarkov.dev/players](https://tarkov.dev/players),
open your profile and enter the number at the end of the link in the app. The
snapshot refreshes when a profile is looked up on tarkov.dev, so it can lag
behind the game by a few days.

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
| Needed items | Y | Show only found-in-raid items |
| Maps | ZL / ZR | Previous / next map |
| Maps | Left stick, touch | Pan |
| Maps | Right stick, A / X | Zoom in / out |
| Maps | Left / Right | Jump between markers |
| Maps | Up / Down | Filter marker type |
| Maps | Y / B | Toggle labels / reset view |
| Maps | - | Fullscreen map |
| Database | ZL / ZR | Switch section |
| Database | Left / Right | Trader or caliber filter (page in Keys and Items) |
| Database | Y / X | Search / clear search |
| Database | A | Open quest, show key lock on the map |
| Quest page | Up / Down, A | Pick an objective or item, show it on the map |
| Quest page | Right stick, ZL / ZR | Scroll the guide |
| Quest page | Y | Cycle guide + details, guide only, details only |
| Profile | A / X | Refresh / change profile ID |

## Data

`.github/workflows/data.yml` runs daily. It pulls the full hideout, item, craft
and trader data from json.tarkov.dev, resolves names and publishes a small file
per game mode to the `data` branch. With `--assets` it also packs the icons of
every item the hideout uses into one atlas (`icons.png`) and renders each map
from tarkov.dev's map art to a JPEG, converting extract, transit and boss
positions to image coordinates (`maps-<mode>.json`). The app downloads these
files, since the full item catalogue is too large to fetch on the Switch.
Builds bundle a copy of everything for offline use.

## Building

```sh
python3 tools/fetch_bundle.py romfs  # optional, bundles offline data, icons and maps
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make
```

Not affiliated with Battlestate Games, tarkov.dev or TarkovTracker.
