# Fallout for PS2 — v1.0.0-rc1

A **native port of Fallout 1 to the PlayStation 2**, standing on the
[fallout1-ce](https://github.com/alexbatalov/fallout1-ce) engine: the original
game logic recompiled for the Emotion Engine, rendering to the GS at 60 Hz,
with the music, voices and movies played back through the PS2's own audio
hardware. No emulation, no interpreter, no PC in the middle.

**Unofficial, non-commercial fan project.** Fallout is a registered trademark
of Bethesda Softworks LLC / ZeniMax Media Inc. No game data is included — you
bring your own copy of the original game (details below).

---

## ✨ What's in this release

**Feature-complete: all planned phases (F0–F9) are done.** From an empty repo
to a port you can play from USB, SD, HDD or disc, save on a Memory Card, and
mod like on PC.

- **🧠 Stable memory footprint** — ~17.6 MB of RDRAM after the heap audit and
  capped caches; the worst stress case (world tour + dialogue + combat) stays
  comfortably inside the PS2's 32 MB.
- **💾 Native Memory Card saves** — saves live on `mc0:` / `mc1:` with slot
  icons, like a real PS2 title; if there's no card, it falls back to your USB
  drive so you never lose progress.
- **🎮 Full DualShock 2 support** — analog cursor with acceleration, combat
  controls, and hotkeys for inventory, character sheet, Pip-Boy and automap.
- **⌨️ On-screen keyboard** — pops up automatically when the game asks for a
  name (new character, save slots). Type with the pad; no cable, no adapter.
- **📀 Real-hardware media support** — boots from USB mass storage, MX4SIO SD
  cards, internal HDD and CD/DVD. The USB/BDM IOP drivers are embedded in the
  ELF, so a plain OPL / uLaunchELF boot works with zero extra configuration.
- **🧩 Mods, total conversions and fan translations** — loose files in `data/`
  override the `.dat` archives exactly like on PC: Spanish text patches,
  custom scripts, full conversions. A boot menu toggles detected mods with
  the pad.
- **Verified on PCSX2** — the port runs an automated in-engine smoke suite
  (frontend, save/load, world tour, dialogue, combat, pad hotkeys) that has to
  pass green before every change. rc1 ships with 0 crashes.

## 🎮 Bring your own game (BYOG)

You need a legitimate copy of **Fallout 1** (Steam, GOG, or the original CD —
any release works).

1. **Extract** `fallout1-ce-ps2-v1.0.0-rc1.zip` to the **root** of your USB
   drive (or MX4SIO SD card). This creates `mass:/FALLOUT/`.
2. **Copy** `MASTER.DAT` and `CRITTER.DAT` from your Fallout installation into
   the same `FALLOUT/` folder.
3. *(Optional, recommended)* Copy the game's `data/` folder there too — music,
   movies and sound effects. Without it the game runs, just quieter.
4. **Launch** `FALLOUT.ELF` from OPL or uLaunchELF (or boot the folder in
   PCSX2). The engine finds its data by itself; nothing to configure.

Installing on the internal HDD (`hdd0:`) or burning a disc (`SYSTEM.CNF` is
included in the bundle) works the same way — full details in `README_PS2.txt`
inside the zip.

## 📦 Downloads

| File | What it is |
| --- | --- |
| `fallout1-ce-ps2-v1.0.0-rc1.zip` | Complete bundle — extract to the root of your USB/SD |
| `FALLOUT.ELF` | Standalone binary, for those who already have everything else in place |

SHA-256:

```
c4d9789223dfadba2405f8a396f5c8f6f62187944d83770bda9b457388c19aba  fallout1-ce-ps2-v1.0.0-rc1.zip
3911a4922ca47ddb93d9d4681c731c2240aa4a29b5d0dfbd896f16aa376fb475  FALLOUT.ELF
```

## 🕹️ Controls (quick reference)

| Input | Action |
| --- | --- |
| Left stick / D-Pad | Mouse cursor / menus (D-Pad nudges with arrows) |
| Right stick | Scroll (lists, world map) |
| Cross | Left click / confirm / type on the keyboard |
| Circle | Right click / interact / cancel |
| Square | Inventory (backspace on the keyboard) |
| Triangle | Character sheet |
| Select | Pip-Boy |
| Start | Game menu (Esc) / confirm |
| R1 | Attack / enter combat; end-combat attempt in battle |
| R2 | End turn / skip dialogue |
| L1 / L2 | Swap weapon hand / cycle weapon mode |
| R3 / L3 | Automap / summon the on-screen keyboard |

## 🙏 Credits

This port stands entirely on the shoulders of **alexbatalov and the
[fallout1-ce](https://github.com/alexbatalov/fallout1-ce) team**. Their
monumental work reverse-engineering and reimplementing the original Fallout
engine in clean modern C++ is what makes this possible — this PS2 version is
their codebase, ported and tuned for the Emotion Engine. Thank you, Alexey
(@alexbatalov).

Huge thanks as well to:

- The **PS2SDK** maintainers and contributors (gsKit, libpad, audsrv, fileXio,
  usbd/usbhdfsd/BDM, and everyone who keeps the IOP alive).
- The **PCSX2** team — the entire port was developed and tested against it.
- The retro homebrew community, for the endless documentation, tools and
  patience.

## ☕ Support the port

If you enjoy playing Fallout on the PlayStation 2 and want to buy me a coffee
for the time spent optimizing and porting the engine:

- **International (USD / PayPal):** [Ko-fi](https://ko-fi.com/nicolasalucin)
- **Argentina (Mercado Pago):** [Cafecito](https://cafecito.app/nicolasalucin)

This is a release candidate: if you find a bug, or you test it on real
hardware, please open an issue with your setup. See you in the wasteland.
