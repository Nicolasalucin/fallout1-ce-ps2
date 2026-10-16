# Fallout for PlayStation 2 — Community Edition (native port)

**English** | [Español](#español)

A **native PlayStation 2 port** of the Fallout 1 engine, based on
[**fallout1-ce**](https://github.com/alexbatalov/fallout1-ce) by
[Alexander Batalov (`alexbatalov`)](https://github.com/alexbatalov) and its
contributors. The engine is compiled for the Emotion Engine (MIPS R5900)
against **PS2SDK** (gsKit, libpad, audsrv, fileXio, libmc) and runs on real
hardware (FMCB/OPL) and on PCSX2.

A Fallout 2 CE port is planned as a second target reusing the same platform
layer.

> **Disclaimer:** This is an unofficial, non-commercial fan project. It is not
> affiliated with, endorsed by, or supported by Bethesda Softworks, Interplay,
> or the creators of the original Fallout. "Fallout" is a trademark of its
> respective owners.

---

## ⚠️ Modified work notice

This repository contains a **modified version** of
[fallout1-ce](https://github.com/alexbatalov/fallout1-ce) (Sustainable Use
License v1.0). Modifications made here:

- Added a native PlayStation 2 platform layer (`src/platform/ps2/`, in C99):
  video (gsKit, 8-bit indexed framebuffer `GS_PSM_T8` + `GS_PSM_CT32` CLUT),
  input (libpad / DualShock 2), audio (audsrv), file I/O (fileXio), Memory
  Card saving (mcman/mcserv + `icon.sys` generation), static arena allocator
  and a mod loader with boot menu.
- Replaced the SDL2 backend with PS2-specific implementations of the engine's
  existing platform interfaces (`svga`, `audio_engine`, input, timers).
- Build system changes for cross-compilation with the ps2dev toolchain.

The original license text is kept unmodified in [LICENSE.md](LICENSE.md).

## Credits

Huge thanks to:

- **[Alexander Batalov (`alexbatalov`)]** — author of
  [fallout1-ce](https://github.com/alexbatalov/fallout1-ce) and
  [fallout2-ce](https://github.com/alexbatalov/fallout2-ce), the clean
  re-implementation of the Fallout engines this port is built upon. All engine
  work, bugfixes and the Reverse-Engineering foundation are his and his
  contributors' work.
- **fallout1-ce / fallout2-ce contributors** — see the upstream repositories.
- **PS2SDK / ps2dev / gsKit authors and community** — for the toolchain,
  drivers and libraries that make PS2 homebrew possible.

## Legal / no game assets

This repository contains **only source code**. It does **not** contain any
copyrighted game material: no `MASTER.DAT`, no `CRITTER.DAT`, no graphics,
sound, music, movies, text or data of any kind from Fallout. You must own a
legal copy of Fallout to use this software (see
[Bring Your Own Game](#bring-your-own-game-byo)).

## Status

Work in progress (native port under construction):

- [x] F0 — Repository setup, legal hygiene, cross-build pipeline (Docker)
- [x] F1 — Full engine compiles for MIPS / platform stubs
- [ ] F2 — Video: 8-bit framebuffer via gsKit (T8 + CLUT), 60 Hz present
- [ ] F3 — Input: DualShock 2, cursor, on-screen keyboard, timers
- [ ] F4 — I/O: fileXio, block cache, `host:` / `mass:` / `hdd0:` probing
- [ ] F5 — Audio: audsrv streaming (ACM music / voices / SFX)
- [ ] F6 — Memory Card saves (`mc0:/FALLOUT1_Sxx`) + icons
- [ ] F7 — Memory: arena allocator, caches capped to fit 32 MB RDRAM
- [ ] F8 — Mod loader + boot menu
- [ ] F9 — Hardware hardening (USB / MX4SIO / HDD)

---

## Bring Your Own Game (BYO)

You need the original game data files. **Legal ways to get them:**

### From Steam

1. Install Fallout (1997) from Steam (App ID 38400).
2. Right click the game in your library → **Manage → Browse local files**.
3. Copy these files/folders from the game directory:
   - `MASTER.DAT`
   - `CRITTER.DAT`
   - the `data` folder (contains `sound/`, `music/`, etc.)

### From GOG

1. Install Fallout using GOG Galaxy, or extract the offline installer:

   ```console
   $ sudo apt install innoextract
   $ innoextract setup_fallout_2.1.0.18.exe -I app
   ```

2. Use the contents of the extracted `app` folder. Same files as above.

You can also prepare the data on Linux directly from your Steam installation
(`~/.steam/steam/steamapps/common/Fallout` or the "compatdata" prefix).

### Where to put the files

**On a real PS2 (USB stick / MX4SIO / HDD):**

```
mass:/FALLOUT/MASTER.DAT
mass:/FALLOUT/CRITTER.DAT
mass:/FALLOUT/data/...
mass:/FALLOUT/mods/...        (optional)
mass:/FALLOUT/saves/...       (fallback save location)
```

The USB stick must be FAT32. MX4SIO exposes the SD card as `mass:` as well.

**On PCSX2 (recommended for development/fast testing):**

- Put the same layout in this repository's `local_assets/` folder
  (ignored by git).
- In PCSX2: **Settings → Emulation → Enable Host Filesystem**.
- Boot `fallout1-ce.elf` from the repository root with
  **System → Boot ELF**; the `host:` device then resolves to the repository
  folder, so the game reads `host:local_assets/`.

## Building

Cross-compilation uses the official ps2dev toolchain in Docker (no local SDK
needed). The upstream `ps2dev/ps2dev` image ships only the bare toolchain, so
this repository provides a tiny derived image that adds `cmake`, `make` and
`ninja`, plus a vendored, modernized toolchain file:

```console
docker build -t fallout-ce-ps2-build os/ps2/build
docker run --rm -v "$(pwd):/src" -w /src fallout-ce-ps2-build sh -c \
  "cmake -B build_ps2 -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-ps2.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build_ps2 -j\$(nproc)"
```

The result is `fallout1-ce.elf` in the repository root. Copy it to your
`mass:/FALLOUT/` folder (console) or boot it with PCSX2 (emulator).

## Controls (DualShock 2)

| Input | Action |
| --- | --- |
| Left stick | Mouse cursor (acceleration curve) |
| Right stick | Scroll map / worldmap |
| ✕ | Left click |
| ○ | Right click / cycle cursor mode |
| L1 | Cycle active weapon |
| R1 | Pip-Boy |
| L2 | Inventory |
| R2 | End turn |
| D-Pad | Scroll view |
| START | Game menu (Esc) |
| SELECT | Character sheet (C) |

## Saving

Saves use flat folders on the Memory Card, one per slot (the PS2 browser
does not support nested save folders):

```
mc0:/FALLOUT1_S01/ … FALLOUT1_S10/   each with SAVE.DAT, icon.sys, icon.icn
```

If no Memory Card is present (or it is full), the game falls back to
`mass:/FALLOUT/saves/` and, on PCSX2, `host:local_assets/saves/`.

## Mods

Put mods in `mods/` next to the game data (loose files or `.dat` files).
Mods override `master.dat`/`critter.dat` content. A boot menu lets you
enable/disable detected mods with the controller before the game starts.

## License

The engine code is distributed under the [Sustainable Use
License](LICENSE.md) v1.0 (non-commercial use; keep all notices). The
PlayStation 2 platform layer added by this port is licensed under the same
terms.

---

# Español

Un **port nativo para PlayStation 2** del motor de Fallout 1, basado en
[**fallout1-ce**](https://github.com/alexbatalov/fallout1-ce) de
[Alexander Batalov (`alexbatalov`)](https://github.com/alexbatalov) y sus
colaboradores. El motor se compila para el Emotion Engine (MIPS R5900) con
**PS2SDK** (gsKit, libpad, audsrv, fileXio, libmc) y funciona en consola real
(FMCB/OPL) y en PCSX2.

Está previsto un segundo objetivo: el port de Fallout 2 CE reutilizando la
misma capa de plataforma.

> **Aviso:** Proyecto fan no oficial y sin ánimo de lucro. No está afiliado ni
> respaldado por Bethesda Softworks, Interplay ni los creadores del Fallout
> original. "Fallout" es una marca registrada de sus respectivos dueños.

## ⚠️ Aviso de obra modificada

Este repositorio contiene una **versión modificada** de
[fallout1-ce](https://github.com/alexbatalov/fallout1-ce) (Sustainable Use
License v1.0). Modificaciones realizadas: capa de plataforma nativa PS2
(`src/platform/ps2/`, C99) para vídeo (gsKit, framebuffer indexado 8 bits
`GS_PSM_T8` + paleta `GS_PSM_CT32`), entrada (libpad), audio (audsrv),
ficheros (fileXio), guardado en Memory Card (`icon.sys` generado), allocator
por arena y cargador de mods con menú de arranque; sustitución del backend
SDL2 por implementaciones PS2 de las interfaces existentes del motor; y
cambios de build para el toolchain ps2dev. El texto de licencia original se
mantiene intacto en [LICENSE.md](LICENSE.md).

## Créditos

Gracias infinitas a:

- **[Alexander Batalov (`alexbatalov`)]** — autor de
  [fallout1-ce](https://github.com/alexbatalov/fallout1-ce) y
  [fallout2-ce](https://github.com/alexbatalov/fallout2-ce), la
  reimplementación limpia del motor sobre la que se construye este port. Todo
  el trabajo de ingeniería del motor, bugfixes y la base de
  ingeniería inversa es suyo y de sus colaboradores.
- **Colaboradores de fallout1-ce / fallout2-ce** — ver repos upstream.
- **Comunidad ps2dev / PS2SDK / gsKit** — por el toolchain, drivers y
  librerías que hacen posible el homebrew de PS2.

## Legal / sin assets del juego

Este repositorio contiene **solo código fuente**. **No** contiene material con
copyright: ni `MASTER.DAT`, ni `CRITTER.DAT`, ni gráficos, sonido, música,
vídeos, textos o datos de Fallout. Debes poseer una copia legal de Fallout
para usar este software (ver [Cómo usar tus propios datos legales]).

## Bring Your Own Game (usa tus propios datos)

Necesitas los ficheros de datos originales. **Formas legales de obtenerlos:**

### Desde Steam

1. Instala Fallout (1997) desde Steam (App ID 38400).
2. Clic derecho en el juego → **Gestionar → Explorar archivos locales**.
3. Copia de la carpeta del juego:
   - `MASTER.DAT`
   - `CRITTER.DAT`
   - la carpeta `data` (contiene `sound/`, `music/`, etc.)

### Desde GOG

1. Instala Fallout con GOG Galaxy o extrae el instalador offline:

   ```console
   $ sudo apt install innoextract
   $ innoextract setup_fallout_2.1.0.18.exe -I app
   ```

2. Usa el contenido de la carpeta `app` extraída. Mismos ficheros.

### Dónde colocar los ficheros

**Consola real (USB / MX4SIO / HDD):**

```
mass:/FALLOUT/MASTER.DAT
mass:/FALLOUT/CRITTER.DAT
mass:/FALLOUT/data/...
mass:/FALLOUT/mods/...        (opcional)
mass:/FALLOUT/saves/...       (guardado alternativo)
```

El pendrive debe estar en FAT32. MX4SIO expone la SD como `mass:` también.

**PCSX2 (recomendado para desarrollo/pruebas rápidas):**

- Coloca el mismo contenido en la carpeta `local_assets/` del repositorio
  (ignorada por git).
- En PCSX2: **Settings → Emulation → Enable Host Filesystem**.
- Arranca `fallout1-ce.elf` desde la raíz del repositorio con
  **System → Boot ELF**; el dispositivo `host:` apunta entonces a la carpeta
  del repositorio y el juego lee `host:local_assets/`.

## Compilación

La compilación cruzada usa el toolchain oficial de ps2dev en Docker (sin
instalar nada en el sistema anfitrión). La imagen upstream `ps2dev/ps2dev`
solo trae el toolchain pelado, así que este repositorio incluye una imagen
derivada mínima que añade `cmake`, `make` y `ninja`, más un toolchain file
propio modernizado:

```console
docker build -t fallout-ce-ps2-build os/ps2/build
docker run --rm -v "$(pwd):/src" -w /src fallout-ce-ps2-build sh -c \
  "cmake -B build_ps2 -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-ps2.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build_ps2 -j\$(nproc)"
```

El resultado es `fallout1-ce.elf` en la raíz del repositorio. Cópialo a
`mass:/FALLOUT/` (consola) o arráncalo con PCSX2 (emulador).

## Controles (DualShock 2)

| Entrada | Acción |
| --- | --- |
| Stick izquierdo | Cursor del ratón (con curva de aceleración) |
| Stick derecho | Scroll de mapa / mapa del mundo |
| ✕ | Clic izquierdo |
| ○ | Clic derecho / cambiar modo de cursor |
| L1 | Cambiar arma activa |
| R1 | Pip-Boy |
| L2 | Inventario |
| R2 | Fin de turno |
| D-Pad | Scroll de vista |
| START | Menú del juego (Esc) |
| SELECT | Hoja de personaje (C) |

## Guardado

Los guardados usan carpetas planas en la Memory Card, una por ranura (el
navegador de PS2 no soporta carpetas anidadas):

```
mc0:/FALLOUT1_S01/ … FALLOUT1_S10/   cada una con SAVE.DAT, icon.sys, icon.icn
```

Si no hay Memory Card (o está llena), se guarda en `mass:/FALLOUT/saves/` y,
en PCSX2, en `host:local_assets/saves/`.

## Mods

Coloca los mods en `mods/` junto a los datos del juego (archivos sueltos o
`.dat`). Los mods tienen prioridad sobre `master.dat`/`critter.dat`. Un menú
de arranque permite activar/desactivar los mods detectados con el mando antes
de iniciar la partida.

## Licencia

El código del motor se distribuye bajo la [Sustainable Use
License](LICENSE.md) v1.0 (uso no comercial; conservar todos los avisos). La
capa de plataforma PS2 añadida por este port se licencia bajo los mismos
términos.
