StickOS - system for the SEGAM M8 stick
=======================================

ADDING GAMES
  Copy ROMs into the right roms/<system> folder (each one has a
  _README.txt listing the accepted extensions). Games show up by themselves;
  a system appears in the menu only if its folder contains games.
  Console ROMs may also be inside a .zip.
  Arcade games are listed with their full title (e.g. sf2.zip shows as
  "Street Fighter II ..."); BIOS zips such as neogeo.zip are not listed.
  The menu remembers the last game played and starts from there.
  Neo Geo: games can go in roms/neogeo (with neogeo.zip in that same
  folder); they are run by FB Alpha and listed under "Arcade (FB Alpha)".

BIOS
  BIOS files go in the bios/ folder (see each system's _README.txt).
  Exception: arcade BIOS zips (e.g. neogeo.zip) go together with the games.

CONTROLS
  SELECT + START (held): in-game pause menu
    (resume, save state, load state, restart, quit).
  New joypad: StickOS offers to configure it; this can be redone from
  Settings > Joypad.

SETTINGS
  Settings > Language: menu language (English, Italiano, Francais,
  Deutsch, Espanol).
  Settings > Joypad: button configuration (L2/R2/L3/R3 and sticks too).
  Settings > <emulator>: all the emulator's options, including the
  controller type (e.g. "dualshock" for the PlayStation analog sticks).

STICKOS FILES (this folder)
  system.sqfs        the system (do not touch)
  stickos.cfg        general settings (menu language, last game played)
  logo.bmp           optional: your own boot logo (uncompressed 24/32 bit
                     BMP, ideally 1280x720); without it the built-in logo
                     is shown
  joypad.cfg         joypad buttons
  options/*.cfg      emulator settings
  logs/*.log         logs, useful when something goes wrong
  disabled           if you create a file with this name, the stick starts
                     the original stock system even with the SD inserted

SAVES
  roms/<system>/save/: game saves (.srm) and save states (.state), next to
  the games of that system. Saves found in the old saves/<system>/ folder
  are moved there automatically at startup.
