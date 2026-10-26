RETRON SQ - SD CARD
===================

This partition holds your games, saves and settings. You can edit it from
Windows, macOS or Linux.

  roms/gb, roms/gbc, roms/gba
      Your own Game Boy / Color / Advance ROMs (.gb .gbc .gba, or .zip inside
      the matching folder). With no cartridge inserted they are listed in the
      menu under Playlists.

  rom/
      Copies of the cartridges you have played, with their saves (.srm) and
      save states. They are also listed under "Cartridge Dumps", so you can
      play them without the cartridge.

  retronsq.cfg          Firmware settings (default emulator cores, etc.)
  cart-overrides.txt    Pick an emulator core for a single cartridge
  retroarch/cores       Built-in cores, plus any extra armhf cores you add
  retroarch/shaders     GLSL shaders (Quick Menu > Shaders in game)
  retroarch/system      Optional BIOS files (gba_bios.bin, gb_bios.bin, ...)

Controls
  Start + Select   Open the RetroArch menu (save states, shaders, settings,
                   load another game)
  Reset button     Restart the game

Cartridges
  Inserting a cartridge always starts it. A cartridge is only read in full
  the first time (or when it changed); after that it starts from the copy in
  rom/.

  Flash cartridges with their own menu (EverDrive, EZ-Flash...) can't run:
  the console reads the cartridge into a file and emulates it, and the
  emulator doesn't have the flash cartridge's hardware. Copy their games into
  roms/ instead. Reproduction cartridges and single-game flash carts work like
  normal cartridges; if one doesn't save, see cart-overrides.txt.

Factory files deleted from this card (settings, shaders, controller
configs) are put back at the next boot. Your own files are never touched.
