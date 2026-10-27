RETRON SQ - SD CARD
===================

This partition holds your games, saves and settings. You can edit it from
Windows, macOS or Linux.

  roms/gb, roms/gbc, roms/gba
      Your own Game Boy / Color / Advance ROMs (.gb .gbc .gba, or .zip inside
      the matching folder). In the menu: Load Content > Collections lists
      them by system, Load Content > Start directory browses this folder.

  rom/
      Copies of the cartridges you have played, with their saves (.srm) and
      save states. They are also listed under "Cartridge Dumps", so you can
      play them without the cartridge (Load Content > Collections).

  saves/
      Save backups, one folder per game. The cartridge's save is backed up
      here automatically whenever it changed (001-cartridge.sav, ...).

  retroarch/cores/
      Emulator cores. Add more by copying Linux armhf libretro cores
      (e.g. gambatte_libretro.so, from buildbot.libretro.com) here, then
      use them from Load Core or set them as the default in retronsq.cfg
      (e.g. GB_CORE=gambatte).

  retronsq.cfg          Firmware settings (default emulator cores, etc.)
  cart-overrides.txt    Pick an emulator core for a single cartridge
  retroarch/shaders     GLSL shaders (Quick Menu > Shaders in game)
  retroarch/system      Optional BIOS files (gba_bios.bin, gb_bios.bin, ...)
  logs/                 What the console did: launcher.log (cartridges,
                        games, saves), retroarch.log, boot.log. Include
                        them when reporting a problem.

Controls
  Start + Select   Open the RetroArch menu (save states, shaders, settings,
                   load another game)
  Reset button     Restart the game

Cartridge Tools (Load Content > Collections > Cartridge Tools)
  Play cartridge             start the cartridge in the slot
  Copy cartridge to SD card  dump the game (and its save) into roms/
  Back up cartridge save     dump the cartridge's save into saves/
  Restore a save...          choose which save to use with the cartridge;
                             the save it replaces is backed up first

Saves
  The console never writes to a cartridge: the slot is read-only. The
  cartridge's save is copied to the SD card the first time you play it, and
  from then on all saving happens on the SD card (rom/<game>.srm). In-game
  saves reach the card within about 2 seconds.

  If a game says its save is corrupted, every save the cartridge ever had
  is in saves/<game>/: pick one with Cartridge Tools > Restore a save...

Cartridges
  Cartridges can be inserted, pulled and swapped while the console is on.
  A cartridge is loaded immediately when it is detected: at power-on, or
  when inserted while the console runs (even during an SD card game, whose
  save is written first). With no cartridge the console boots into the menu.
  To have inserted cartridges wait instead (start them with Cartridge Tools
  > Play cartridge), set CART_AUTOLOAD=0 in retronsq.cfg.

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
