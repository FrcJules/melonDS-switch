# melonDS Switch (unofficial fork)

Nintendo DS emulator for the Nintendo Switch (homebrew), based on the current melonDS core (1.x).
**Unofficial**: not made or supported by the melonDS team. Please do not report its bugs to them.
The official, older port is `melonDS-emu/melonDS-switch`.

**AI disclosure**: the Switch frontend and the changes made for this port were written with an AI assistant (Claude),
directed and tested on real hardware by FrcJules. The emulation core is melonDS (GPL-3.0), with one small fix (see below).

## Features
- ARM64 **JIT** + fastmem, OpenGL ES 3 renderer (Mesa) with **2x** internal resolution, or software renderer.
- **No BIOS or firmware needed** (melonDS FreeBIOS + direct boot). No game, BIOS, firmware or key is included.
- **CPU "Auto" mode**: 1224 MHz, raised to 1785 MHz within ~0.5 s when a game falls behind or after a state is loaded,
  lowered back when there is headroom (measured CPU load).
- **Touch screen without touching** (TV mode): the right stick moves a cursor on the bottom screen, hold the right stick
  click (R3) to touch (drag and handwriting work). The left stick works as the D-pad.
- **Per-game settings** (renderer, resolution, CPU) saved in `<rom>.ui`; 4th generation Pokémon start in software
  rendering (avoids the dotted black lines of the GPU renderer); games that change the display mid-frame (e.g.
  Professor Layton videos) switch to software rendering automatically.
- **Home screen**: recent games first, game browser with the titles and icons read from the ROMs, general settings,
  settings of one game (Y), help. Usable with the buttons or the touch screen.
- **Screen layouts**: vertical, horizontal, **hybrid** (one big screen on the left, both screens small on the right),
  swapped screens, one screen bigger or alone, "book" rotation (90° / 270°), integer or fitted scale, gap. Per game.
- **Automatic big screen**: while the bottom screen is black (0.5 s), the top screen is shown alone, as big as possible;
  the normal layout comes back as soon as the bottom screen shows something (can be turned off).
- **Save states** (4 slots + automatic slot on exit), LZ4-compressed (~2.5 MB), saved in the background, last state kept
  in memory for instant reloads. Audio rate control without pitch wobble.

Tested at 60 fps (default settings) on: Mario Kart DS, Pokémon White, New Super Mario Bros., Pokémon HeartGold,
Pokémon Diamond, Professor Layton and the Curious Village, StarFox Command, Metroid Prime Hunters, plus homebrew.
Firmware 20.2.0 / Atmosphère.

## Install
Copy `switch/melonDS-Switch/melonDS-Switch.nro` to `sdmc:/switch/melonDS-Switch/`.
**Run hbmenu in application mode** (hold R while starting a game, also when choosing the user): the applet mode
(album) does not have enough memory; started from there, the app shows how to do it and quits.
Your games (`.nds` files you dumped yourself) can be anywhere; the file browser starts in `sdmc:/roms/nds` if it exists.
Settings, saves and states are in `sdmc:/switch/melonDS-Switch/` (files of the older `melonds-fork` folder are moved
there automatically).

## Controls
| | |
|---|---|
| D-pad / left stick, A B X Y, L R, + (Start), - (Select) | DS buttons |
| Right stick + R3 held | touch the bottom screen |
| ZL + ZR + PLUS | menu (display, CPU, renderer, resolution, states, change game, quit) |
| ZL + ZR held, then X / Y / left / right | save state / load state / slot 1-4 |
| ZL + ZR held, then up / MINUS | next layout (vertical, horizontal, hybrid) / swap the screens (hybrid: big screen top or bottom) |
| L + R + PLUS + MINUS | quit (the state is saved in slot 0 and reloaded next time) |

## Files (`sdmc:/switch/melonDS-Switch/`)
| File | |
|---|---|
| `config.txt` | optional: `rom=` `romdir=` `overclock=<MHz>\|auto` `gpu=0\|1` `gpuscale=1..3` `autosave=0\|1` `autoload=0\|1` ... (see the top of `main.cpp`) |
| `ui.txt` | general settings (home screen, menu) |
| `<rom>.ui` | settings of that game: layout, renderer, resolution, CPU |
| `recent.txt`, `home.txt` | recent games, last folder of the game browser |
| `<rom>.sav`, `<rom>.ss0..4` | game save, states |
| `log.txt` | one line per second (fps, CPU clock, temperature...) |

## Build
devkitPro (devkitA64, libnx, portlibs `switch-sdl2`, `switch-sdl2_ttf`, `switch-freetype`, `switch-harfbuzz`,
`switch-libpng`, `switch-bzip2`, `switch-zlib`, `switch-lz4`, `switch-mesa`):
```
cmake -S . -B build-switch -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake -DBUILD_SWITCH_SDL=ON -DBUILD_QT_SDL=OFF \
      -DENABLE_OGLRENDERER=ON -DENABLE_OGL_COMPUTE=OFF -DSWITCH_PROFILE=OFF \
      "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -flto=1" "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -flto=1" \
      -DCMAKE_EXE_LINKER_FLAGS_RELEASE=-flto=1 -DMELONDS_LTO_EXCLUDE_DMA=ON -DMELONDS_LTO_EXCLUDE_GL=ON
cmake --build build-switch --target melonDS-switch
```
Result: `build-switch/src/frontend/switch-sdl/melonDS-switch.nro`.

## Core changes
Besides the frontend: geometry and JIT speed-ups (output identical, checked with fixed-rate audio recordings), a
GLES 3 path for the OpenGL renderer, and `NDSCart/CartRetailIR.cpp`: the IR cartridge returned an uninitialised value
for IR commands that are not emulated (also fixed upstream in pending PR #2727).

## Contact
FrcJules: Discord `rateur42`, e-mail rateur42@gmail.com. Bug reports: GitHub issues of this repository.
For this port only: please do not contact the melonDS team about it.

## License
The screen layouts use the melonDS `ScreenLayout` code (from its desktop frontend). The menus are new code; their look
was inspired by RSDuck's earlier Switch port (`RSDuck/melonDS`, branch `switch-new`, GPL-3.0), from which no code is
copied. Button icons and text use the console's shared system fonts, read at run time.

GPL-3.0-or-later, like melonDS. melonDS © the melonDS team (Arisotura, RSDuck and contributors); icon based on the
melonDS icon by limittox. Third-party libraries linked into the `.nro`: see `THIRD-PARTY-NOTICES.md`.
The menu text uses the console's shared system font, read at run time (not distributed).
