Tarkov Hideout (desktop)
========================

The Switch companion app as a window for your second screen: hideout stations,
needed items, crafts and timers, maps with extracts and floors, ammo chart,
quests with wiki guides, keys and items, your tarkov.dev profile, and
TarkovTracker sync (read only).

Start it
--------
Windows: unzip anywhere and run TarkovHideout.exe.
Linux:   extract and run ./TarkovHideout. It needs SDL2, SDL2_ttf, SDL2_image
         and curl, on Arch/CachyOS:
             sudo pacman -S --needed sdl2 sdl2_ttf sdl2_image curl
         Optional menu entry: copy TarkovHideout.desktop to
         ~/.local/share/applications (fix Exec= to the full path) and icon.png
         to ~/.local/share/icons/tarkov-hideout.png.

Your progress, token, downloads and window position are saved in
  Windows: %APPDATA%\TarkovHideout
  Linux:   ~/.local/share/TarkovHideout
so a new version can replace the old folder.

Controls
--------
Mouse     click tabs, rows, the button hints at the bottom and "< filter >"
          labels; double-click a row for its main action (open a quest,
          start a craft, pin a round, +1 on a hideout item).
          Wheel scrolls lists and the quest guide and zooms the map at the
          pointer; drag pans the map. Right-click goes back.
Keyboard  1-9 tabs, Q/E or Tab previous/next tab, arrows move, Enter selects,
          Esc goes back, PgUp/PgDn = ZL/ZR, X and Y as shown, Ctrl+F search,
          F map fullscreen, R map plan/render, F11 window fullscreen,
          Ctrl+Q quit. The hints at the bottom show the keys.
Gamepad   any controller works like the Switch pad; the hints switch back to
          Switch buttons when you use it.

The window scales to any size (the layout is 16:9) and text stays sharp.
