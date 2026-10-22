==============================================================================
 FALLOUT for PlayStation 2 - Community Edition      v1.0.1
 Unofficial, non-commercial native engine port. Bring your own game data.
==============================================================================

------------------------------------------------------------------------------
 LEGAL / DISCLAIMER
------------------------------------------------------------------------------
Fallout is a registered trademark of Bethesda Softworks LLC / ZeniMax Media
Inc. This is an unofficial, non-commercial, open-source engine port. It is
NOT affiliated with, endorsed by, or sponsored by Bethesda Softworks,
ZeniMax Media, Interplay or Black Isle Studios.

No copyrighted game assets are included in this release: no data files, no
graphics, no audio, no text. The engine code is original work, and the game
content must come from YOUR own legitimate copy of Fallout 1 (Steam, GOG or
the original CD). If you do not own the game, please buy it.

------------------------------------------------------------------------------
 WHAT'S NEW IN v1.0.1
------------------------------------------------------------------------------
- Hardware-stable boot: clean IOP reset with automatic host:/PCSX2 bypass,
  plus an optional NO_IOP_RESET marker for loaders that already own the IOP.
- 1 MB main stack, divide-by-zero traps disabled, R5900 exception handlers and
  a fatal-error screen with a crash log.
- USB/MX4SIO/BDM probing for mass:, mass0:, mass1:, mass2: and mass3: without
  opendir() on missing devices; case-insensitive MASTER.DAT/CRITTER.DAT.
- GS/VRAM validation, PAL/NTSC auto-detection and optional PS2VIDEO.CFG
  (mode=PAL or mode=NTSC).
- Local ps2sdk SIF RPC hardening fixed for fileio reads: fioRead() now receives
  its _fio_read_intr completion, so fallout.cfg loads reliably on strict PCSX2
  and on real hardware.
- Full hyper-strict PCSX2 smoke suite green (frontend, save/load, world tour,
  dialogue, combat and pad hotkeys).

------------------------------------------------------------------------------
 INSTALLATION
------------------------------------------------------------------------------
You need:
  - FALLOUT.ELF           the game binary (this bundle)
  - SYSTEM.CNF            the PS2 boot descriptor (this bundle)
  - MASTER.DAT            from your own Fallout 1 installation
  - CRITTER.DAT           from your own Fallout 1 installation

Layout on USB (FAT32), MX4SIO SD card, internal HDD, or a folder used with
OPL / uLaunchELF:

  mass:/FALLOUT/FALLOUT.ELF
  mass:/FALLOUT/SYSTEM.CNF
  mass:/FALLOUT/MASTER.DAT
  mass:/FALLOUT/CRITTER.DAT
  mass:/FALLOUT/data/...        optional, see below

Optional but recommended: copy the data/ folder (music, movies, sounds)
from your Fallout installation into the same folder. The engine reads loose
files first and falls back to the .DAT archives, so with data/ in place you
get the full soundtrack, voiced dialogue and movies. Without it the game
still runs, just quieter.

Launching:
  - OPL: copy the folder to your device and boot FALLOUT.ELF.
  - uLaunchELF: browse to FALLOUT.ELF and launch it.
  - Disc boot / PCSX2: use SYSTEM.CNF (BOOT2 points at
    cdrom0:\FALLOUT.ELF;1), e.g. when burning or mounting a folder.
  - PCSX2: File -> Boot ELF, with Host Filesystem enabled.

The game finds its data automatically: first next to the ELF (the folder it
was launched from), then mass:, cdrom0: and hdd0:, so the layout above works
without any configuration. Saves go to the Memory Card (mc0:, then mc1:),
falling back to mass:/FALLOUT/saves/ when no card is present. The USB/MX4SIO
drivers are embedded in the ELF, so a raw-ELF boot works even without OPL's
IOP stack.

If your loader already provides a working IOP stack and you prefer to keep it,
place an empty NO_IOP_RESET file next to FALLOUT.ELF (for example
mass:/FALLOUT/NO_IOP_RESET or mc0:/FALLOUT/NO_IOP_RESET). The port then skips
the clean IOP reset and reuses the loader's modules.

------------------------------------------------------------------------------
 MODS & CUSTOM CONTENT
------------------------------------------------------------------------------
Mods, total conversions, fan translations (including Spanish text patches)
and custom scripts work like they do on the PC: drop loose files into data/
next to the game data and they cleanly override the contents of
master.dat/critter.dat. The boot menu lets you enable or disable the mods
detected next to the game data, with the controller, before the game starts.

------------------------------------------------------------------------------
 CONTROLS (DualShock 2)
------------------------------------------------------------------------------
  Left stick ............ mouse cursor & menus (D-Pad nudges with arrows)
  Right stick ........... scroll wheel (lists / world map)
  Cross ................. left click / confirm / type (on-screen keyboard)
  Circle ................ right click / interact / cancel (on-screen keyboard)
  Square ................ inventory (backspace on the on-screen keyboard)
  Triangle .............. character sheet
  Select ................ Pip-Boy
  Start ................. game menu (Esc) / confirm (on-screen keyboard)
  R1 .................... attack / enter combat (A); in battle: end-combat
                          attempt (Return)
  R2 .................... end turn / skip dialog (Space)
  L1 .................... swap weapon hand (B)
  L2 .................... cycle weapon / item mode (N)
  R3 .................... automap (Tab)
  L3 .................... summon the on-screen keyboard at any time

The on-screen keyboard pops up automatically whenever the game asks for
text (save names, character names). Navigate with the D-Pad or left stick,
type with Cross, backspace with Square, confirm with Start and cancel with
Circle.

------------------------------------------------------------------------------
 CREDITS
------------------------------------------------------------------------------
This port stands entirely on the shoulders of the fallout1-ce project:
massive thanks to alexbatalov and everyone who reverse-engineered and
reimplemented the original Fallout engine in clean, modern C++. Without
that codebase this PS2 version would not exist.

Thanks as well to the ps2sdk maintainers and contributors (gsKit, libpad,
audsrv, fileXio, usbd/usbhdfsd/BDM), the PCSX2 team (this port was developed
and tested against it), and the retro homebrew community that keeps this
hardware alive.

------------------------------------------------------------------------------
 SUPPORT THE PORT
------------------------------------------------------------------------------
If you enjoy playing Fallout on the PlayStation 2 and want to buy me a
coffee for the time spent optimizing and porting the engine:
  - International (USD / PayPal): https://ko-fi.com/nicolasalucin
  - Argentina (Mercado Pago):     https://cafecito.app/nicolasalucin

No paywalls, no locked builds - just a thank you.

==============================================================================
 Sources, documentation and the issue tracker: see the repository README.
==============================================================================
