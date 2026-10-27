RETRON SQ - SD CARD
===================

This partition holds your games, saves and settings. You can edit it from
Windows, macOS or Linux.

  roms/gb, roms/gbc, roms/gba
      Your own Game Boy / Color / Advance ROMs (.gb .gbc .gba, or .zip inside
      the matching folder). The home screen lists them by system.

  rom/
      Copies of the cartridges you have played, with their saves (.srm) and
      save states. The home screen lists them under "Cartridge dumps", so
      you can play them without the cartridge.

  saves/
      Save backups, one folder per game. The cartridge's save is backed up
      here automatically whenever it changed (001-cartridge.sav, ...).

  retroarch/cores/
      Emulator cores. Add more by copying Linux armhf libretro cores
      (e.g. gambatte_libretro.so, from buildbot.libretro.com) here, then
      pick them in the home screen's Settings (or retronsq.cfg, e.g.
      GB_CORE=gambatte).

  Box art (pictures on the home screen)
      A .png with the game's name next to it: roms/gba/Golden Sun.png for
      roms/gba/Golden Sun.gba. Or RetroArch's layout, see
      retroarch/thumbnails/README.txt.

  home/
      Recently played and favorites.

  retronsq.cfg          Firmware settings (default emulator cores, etc.)
  cart-overrides.txt    Pick an emulator core for a single cartridge
  retroarch/shaders     GLSL shaders (Quick Menu > Shaders in game)
  retroarch/system      Optional BIOS files (gba_bios.bin, gb_bios.bin, ...)
  logs/                 What the console did: launcher.log (cartridges,
                        games, saves), retroarch.log, boot.log. Include
                        them when reporting a problem.

Home screen
  Up/Down picks a page, A goes in, B goes back. In a list: A plays, Y marks
  a favorite, L/R jump a page. Settings changes the start-up behaviour and
  the emulator for each system.

Controls
  Start + Select   In a game: the RetroArch menu (save states, shaders,
                   settings; Close Content goes back home)
  Reset button     Restart the game

Cartridge (home screen > Cartridge)
  Play cartridge             start the cartridge in the slot
  Copy cartridge to SD card  dump the game (and its save) into roms/
  Back up cartridge save     dump the cartridge's save into saves/
  Restore a save...          choose which save to use with the cartridge;
                             the save it replaces is backed up first
  Write a save to the cartridge...
                             experimental: put a save onto the cartridge
                             itself (its own save is backed up first)

Saves
  Playing never writes to a cartridge: the slot is read-only (only Cartridge
  Tools > Write a save to the cartridge does, when you ask for it). The
  cartridge's save is copied to the SD card the first time you play it, and
  from then on all saving happens on the SD card (rom/<game>.srm). In-game
  saves reach the card within about 2 seconds.

  If a game says its save is corrupted, every save the cartridge ever had
  is in saves/<game>/: pick one with Cartridge > Restore a save...

Cartridges
  Cartridges can be inserted, pulled and swapped while the console is on.
  A cartridge is loaded immediately when it is detected: at power-on, or
  when inserted while the console runs (even during an SD card game, whose
  save is written first). With no cartridge the console starts on the home
  screen.
  To have inserted cartridges wait instead (start them from the Cartridge
  page), switch off Settings > Start cartridges when inserted.

  A cartridge is only read in full the first time (or when it changed);
  after that it starts from the copy in rom/. The first read shows a
  loading screen: the cartridge slot reads at USB 1.1 speed (about 1 MB a
  second), so a big GBA game takes up to about 35 seconds.

  Flash cartridges with their own menu (EverDrive, EZ-Flash...) can't run:
  the console reads the cartridge into a file and emulates it, and the
  emulator doesn't have the flash cartridge's hardware. Copy their games into
  roms/ instead. Reproduction cartridges and single-game flash carts work like
  normal cartridges; if one doesn't save, see cart-overrides.txt.

Factory files deleted from this card (settings, shaders, controller
configs) are put back at the next boot. Your own files are never touched.
