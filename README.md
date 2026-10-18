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
  ### AI Assistance & Tooling
- **AI-Assisted Porting:** Developed and maintained by Nicolas Alucin using AI coding assistance ([OpenCode](https://github.com/opencode-ai) / LLMs) under human technical direction for low-level architecture, memory profiling, and platform abstraction.

## Legal / no game assets

This repository contains **only source code**. It does **not** contain any
copyrighted game material: no `MASTER.DAT`, no `CRITTER.DAT`, no graphics,
sound, music, movies, text or data of any kind from Fallout. You must own a
legal copy of Fallout to use this software (see
[Bring Your Own Game](#bring-your-own-game-byo)).

## Status

**v1.0.0-rc1** — release candidate. All planned phases (F0–F9) have landed and the full smoke suite is green on PCSX2 (frontend, save/load with the on-screen keyboard, world tour, dialogue, combat, pad hotkeys):

- [x] F0 — Repository setup, legal hygiene, cross-build pipeline (Docker)
- [x] F1 — Full engine compiles for MIPS / platform stubs
- [x] F2 — Video: 8-bit framebuffer via gsKit (T8 + CLUT), 60 Hz present
- [x] F3 — Input: DualShock 2 (libpad, analog mode, accelerated cursor; verified with a virtual pad)
- [x] F4 — I/O: device probing (`host:` / `mass:` / `hdd0:`), case-insensitive paths, block buffering for the big `.dat` files
- [x] F5 — Audio: audsrv streaming (ACM music / voices / SFX), mixed on a dedicated EE thread
- [x] F6 — Memory Card saves (`mc0:/FALLOUT1_Sxx`) + slot icons; auto-named saves (on-screen keyboard deferred to F8)
- [x] F7 — Memory: heap audit + capped caches (no arena allocator needed); world-tour / dialogue / combat stress with heap watermarks under 32 MB RDRAM
- [x] F8 — On-screen keyboard (save naming) + DualShock 2 ergonomic mapping; pad hotkey dispatch verified
- [x] F9 — Real-hardware storage: ELF-relative asset discovery (USB / MX4SIO / HDD / CD), embedded USB + BDM IOP drivers, case-insensitive path resolution

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

The game locates `MASTER.DAT`/`CRITTER.DAT` automatically: first next to the
ELF (the directory it was launched from), then `mass:`, `mass0:`, `cdrom0:`,
`hdd0:` and finally the PCSX2 `host:` layouts. Booted as a raw ELF without a
loader-provided IOP stack, the build brings up USB/MX4SIO storage using the
ps2sdk drivers embedded in the ELF; under OPL/uLaunchELF those drivers are
already resident and are detected and left alone. Saves are independent of the
asset device: they always go to the memory card (`mc0:`, then `mc1:`, then the
asset folder if there is no card).

**On PCSX2 (recommended for development/fast testing):**

- Put the same layout in this repository's `local_assets/` folder
  (ignored by git).
- In PCSX2: **Settings → Emulation → Enable Host Filesystem**.
- Boot `fallout1-ce.elf` with **System → Boot ELF**, keeping it in the folder
  that contains `local_assets/` (e.g. the repository root). PCSX2 resolves
  `host:` to the **folder of the booted ELF**, so the game reads
  `host:local_assets/`.
- Development tip (ELF inside `build_ps2/`): symlink the assets next to it
  once with `ln -s ../local_assets build_ps2/local_assets`.
- `audsrv.irx` must sit **next to the ELF** (the build copies it there
  automatically): audio is loaded from disk at startup, which also works in
  PCSX2 with a host filesystem.

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

For a release bundle, run `tools/make_dist.sh`: it builds the shipping ELF
(Release, smoke harness disabled), strips it with
`mips64r5900el-ps2-elf-strip` and assembles `dist/FALLOUT/` with
`FALLOUT.ELF`, `SYSTEM.CNF` and `README_PS2.txt` — copy that folder to your
device or burn it to CD/DVD.

## Controls (DualShock 2)

| Input | Action |
| --- | --- |
| Left stick | Mouse cursor (acceleration curve) |
| Right stick | Scroll wheel (lists / world map) |
| ✕ Cross | Left click / confirm |
| ○ Circle | Right click / interact |
| □ Square | Inventory (I) |
| △ Triangle | Character sheet (C) |
| SELECT | Pip-Boy (P) |
| START | Game menu (Esc) |
| L1 | Change active hand (B) |
| L2 | Cycle item / weapon mode (N) |
| R1 | Attack / enter combat (A); end-combat attempt (Return) in battle |
| R2 | End turn / skip dialog (Space) |
| R3 | Automap (Tab) |
| L3 | Toggle on-screen keyboard |
| D-Pad | Arrow keys (move / navigate) |

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

## Support the Port

If you enjoy playing Fallout on the PlayStation 2 and want to buy me a
coffee for the time spent optimizing and porting the engine:

- **International (USD / PayPal):** [Ko-fi](https://ko-fi.com/nicolasalucin)
- **Argentina (Mercado Pago):** [Cafecito](https://cafecito.app/nicolasalucin)

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
  ### Asistencia de IA y Herramientas
- **Desarrollo asistido por IA:** Port diseñado y mantenido por Nicolas Alucin con asistencia de herramientas de IA (OpenCode / LLMs) bajo dirección técnica humana para la implementación de bajo nivel, gestión de memoria RDRAM y abstracción de plataforma para PS2.

## Legal / sin assets del juego

Este repositorio contiene **solo código fuente**. **No** contiene material con
copyright: ni `MASTER.DAT`, ni `CRITTER.DAT`, ni gráficos, sonido, música,
vídeos, textos o datos de Fallout. Debes poseer una copia legal de Fallout
para usar este software (ver [Cómo usar tus propios datos legales]).

## Estado

**v1.0.0-rc1** — release candidate. Todas las fases previstas (F0–F9) están completadas y la suite de smoke completa da verde en PCSX2 (frontend, guardado/carga con teclado en pantalla, world tour, diálogos, combate, atajos del pad):

- [x] F0 — Preparación del repositorio, higiene legal, build cruzado (Docker)
- [x] F1 — El motor completo compila para MIPS / stubs de plataforma
- [x] F2 — Vídeo: framebuffer de 8 bits con gsKit (T8 + CLUT), presentado a 60 Hz
- [x] F3 — Entrada: DualShock 2 (libpad, modo analógico, cursor acelerado)
- [x] F4 — E/S: sondeo de `host:` / `mass:` / `hdd0:`, rutas sin distinguir
  mayúsculas, búfer por bloques para los `.dat` grandes
- [x] F5 — Audio: streaming por audsrv (música `.ACM`, voces y SFX), mezcla en un hilo propio del EE
- [x] F6 — Guardados en tarjeta de memoria (`mc0:/FALLOUT1_Sxx`) + iconos por
  ranura; nombre automático (teclado en pantalla para nombrar pospuesto a F8)
- [x] F7 — Memoria: auditoría de heap + cachés con tope (sin asignador de arena); stress world-tour / diálogo / combate con watermarks bajo los 32 MB de RDRAM
- [x] F8 — Teclado en pantalla (nombre de guardado) + mapeo ergonómico DualShock 2; despacho de atajos de pad verificado
- [x] F9 — Almacenamiento en hardware real: búsqueda de assets relativa al ELF (USB / MX4SIO / HDD / CD), drivers IOP USB + BDM embebidos, resolución de rutas sin distinguir mayúsculas

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

El juego localiza `MASTER.DAT`/`CRITTER.DAT` automáticamente: primero junto al
ELF (la carpeta desde la que se lanzó), luego `mass:`, `mass0:`, `cdrom0:`,
`hdd0:` y por último los layouts `host:` de PCSX2. Si arrancas el ELF "a pelo"
sin la pila IOP de un loader, el propio build monta el almacenamiento
USB/MX4SIO con los drivers de ps2sdk embebidos en el ELF; con OPL/uLaunchELF
esos drivers ya están cargados y se detectan sin tocarlos. Los guardados son
independientes del dispositivo de datos: van siempre a la tarjeta de memoria
(`mc0:`, si no `mc1:`, y si no hay tarjeta a la carpeta de assets).

**PCSX2 (recomendado para desarrollo/pruebas rápidas):**

- Coloca el mismo contenido en la carpeta `local_assets/` del repositorio
  (ignorada por git).
- En PCSX2: **Settings → Emulation → Enable Host Filesystem**.
- Arranca `fallout1-ce.elf` con **System → Boot ELF**, manteniéndolo en la
  carpeta que contiene `local_assets/` (p. ej. la raíz del repositorio). PCSX2
  resuelve `host:` a la **carpeta del ELF arrancado**, así que el juego lee
  `host:local_assets/`.
- Truco para desarrollo (ELF dentro de `build_ps2/`): enlaza los datos una vez
  con `ln -s ../local_assets build_ps2/local_assets`.
- `audsrv.irx` debe estar **junto al ELF** (el build lo copia automáticamente):
  el audio se carga de disco al arrancar, lo que también funciona en PCSX2
  con host filesystem.

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

Para generar el paquete de release, ejecutá `tools/make_dist.sh`: compila el
ELF definitivo (Release, sin el harness de smoke), lo strippea con
`mips64r5900el-ps2-elf-strip` y arma `dist/FALLOUT/` con `FALLOUT.ELF`,
`SYSTEM.CNF` y `README_PS2.txt`; copiá esa carpeta a tu dispositivo o
grabala en CD/DVD.

## Controles (DualShock 2)

| Entrada | Acción |
| --- | --- |
| Stick izquierdo | Cursor del ratón (con curva de aceleración) |
| Stick derecho | Rueda de scroll (listas / mapa del mundo) |
| ✕ Cruz | Clic izquierdo / confirmar |
| ○ Círculo | Clic derecho / interactuar |
| □ Cuadrado | Inventario (I) |
| △ Triángulo | Hoja de personaje (C) |
| SELECT | Pip-Boy (P) |
| START | Menú del juego (Esc) |
| L1 | Cambiar mano activa (B) |
| L2 | Ciclar modo de arma / objeto (N) |
| R1 | Atacar / entrar en combate (A); intentar fin de combate (Return) en batalla |
| R2 | Fin de turno / saltar diálogo (Espacio) |
| R3 | Automapa (Tab) |
| L3 | Alternar teclado en pantalla |
| D-Pad | Flechas (moverse / navegar) |

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

## Apoyá el port

Si disfrutás jugando Fallout en la PlayStation 2 y querés invitarme un café
por el tiempo dedicado a optimizar y portear el motor:

- **Internacional (USD / PayPal):** [Ko-fi](https://ko-fi.com/nicolasalucin)
- **Argentina (Mercado Pago):** [Cafecito](https://cafecito.app/nicolasalucin)

## Licencia

El código del motor se distribuye bajo la [Sustainable Use
License](LICENSE.md) v1.0 (uso no comercial; conservar todos los avisos). La
capa de plataforma PS2 añadida por este port se licencia bajo los mismos
términos.
