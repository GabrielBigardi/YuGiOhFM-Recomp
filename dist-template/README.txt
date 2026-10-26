YU-GI-OH! FORBIDDEN MEMORIES - NATIVE PC PORT
=============================================

This is a native PC (Windows / Linux) build of Yu-Gi-Oh! Forbidden Memories for the
PlayStation (USA, SLUS-01411), made from your own copy of the game. The PlayStation game code
was statically recompiled into native x86-64 code - there is no emulator and no PlayStation
BIOS involved. The PlayStation hardware the game talks to (graphics chip, sound chip, CD
drive, video decoder, controller ports, memory cards, geometry coprocessor, timers) and the
BIOS services it uses are reimplemented natively so the game behaves exactly like the
original.

RUNNING
-------
Run YuGiOhFM.exe (Windows) or ./YuGiOhFM (Linux).

Linux: requires SDL2 (normally already installed; otherwise e.g. "sudo apt install
libsdl2-2.0-0", "sudo dnf install SDL2" or "sudo pacman -S sdl2"). Make the file executable
if needed: chmod +x YuGiOhFM

{{GAME_DATA}}

CONTROLS
--------
                 Keyboard                 Xbox controller
  D-pad          Arrow keys               D-pad / left stick
  Cross (X)      X / Space / K            A
  Circle (O)     Z / L                    B
  Square         S / J                    X
  Triangle       A / I                    Y
  L1 / R1        Q / W                    LB / RB
  L2 / R2        1 / 2                    LT / RT
  Start          Enter                    Start (Menu)
  Select         Right Shift / Backspace  Back (View)

  F11 or Alt+Enter   toggle fullscreen         Esc   leave fullscreen
  F12                screenshot (screenshots folder)
  Tab (hold)         fast-forward (can be disabled in yugiohfm.ini)

The controllers act like PlayStation pads; the left stick works as the D-pad. The keyboard
and the first controller are player 1, a second controller is player 2 (2P Duel).
Controllers can be plugged in at any time. All keys and buttons can be remapped in
yugiohfm.ini.

SAVES, 2P DUEL AND TRADE
------------------------
Saving works exactly as on the PlayStation: SAVE in the main menu or at the shop after a
duel, and LOAD on the title screen. Two memory cards are inserted: saves\card1.mcd (slot 1)
and saves\card2.mcd (slot 2) - standard 128 KB raw memory card images, compatible with
DuckStation, ePSXe, PCSX and others, so you can copy an existing save here or take yours to
an emulator. If the game folder is not writable, the cards are kept in %APPDATA%\YuGiOhFM
(Windows) or ~/.local/share/YuGiOhFM (Linux).

2P Duel and Trade (title screen) load one player's save from each card, like two players
bringing their memory cards: player 1's save on card1.mcd, player 2's on card2.mcd (copy a
friend's card file there). The two saves must be different duelists.

NOTES
-----
* Video runs at the original NTSC rate (59.29 fps) with the original picture, scaled up.
  The movies play from the original video streams.
* A log file (YuGiOhFM.log) is written next to the executable - useful if something goes
  wrong.
* This folder is for your personal use only - do not share it. The executable contains the
  game's program code translated from your copy, which, like the game's data, remains the
  property of its copyright holders (Konami, Kazuki Takahashi).
  SDL2 is used under the zlib license (LICENSE-SDL2.txt).
