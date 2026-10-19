cmake_minimum_required(VERSION 3.13)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

set(EXECUTABLE_NAME fallout-ce)

if (APPLE)
    if(IOS)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "12" CACHE STRING "")
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "")
    else()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
        set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "")
    endif()
endif()

project(${EXECUTABLE_NAME})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

option(ASAN "Enable address sanitizer" OFF)
option(UBSAN "Enable undefined behaviour sanitizer" OFF)

# ---------------------------------------------------------------------------
# PlayStation 2 port (native build against PS2SDK).
#
# Enabled automatically by cmake/toolchain-ps2.cmake (which sets PS2=TRUE) or
# manually with -DPS2_BUILD=ON for inspection. See README.md for the Docker
# build command.
# ---------------------------------------------------------------------------
option(PS2_BUILD "Build for the PlayStation 2 (PS2SDK cross toolchain)" OFF)
if(PS2_BUILD)
    set(PS2 TRUE)
endif()

# Dev-only: compile the automated smoke-test harness (src/game/ps2_smoke.cc).
# Driven by tools/run_smoke.sh: after engine init it takes over the game flow,
# boots straight into a map with a premade character and logs a
# SUITE_FINISHED_* verdict to the emulator console.
option(PS2_DEV_SMOKE_TEST "Compile the PS2 automated smoke-test harness (dev only)" OFF)

if (ANDROID)
    add_library(${EXECUTABLE_NAME} SHARED)
else()
    add_executable(${EXECUTABLE_NAME} WIN32 MACOSX_BUNDLE)
endif()

if(ASAN)
    target_compile_options(${EXECUTABLE_NAME} PUBLIC "-fsanitize=address;-fsanitize-recover=address")
    target_link_options(${EXECUTABLE_NAME} PUBLIC "-fsanitize=address;-fsanitize-recover=address")
endif()
if(UBSAN)
    target_compile_options(${EXECUTABLE_NAME} PUBLIC "-fsanitize=undefined")
    target_link_options(${EXECUTABLE_NAME} PUBLIC "-fsanitize=undefined")
endif()

target_include_directories(${EXECUTABLE_NAME} PUBLIC src)

target_sources(${EXECUTABLE_NAME} PUBLIC
    "src/game/actions.cc"
    "src/game/actions.h"
    "src/game/amutex.cc"
    "src/game/amutex.h"
    "src/game/anim.cc"
    "src/game/anim.h"
    "src/game/art.cc"
    "src/game/art.h"
    "src/game/automap.cc"
    "src/game/automap.h"
    "src/game/bmpdlog.cc"
    "src/game/bmpdlog.h"
    "src/game/cache.cc"
    "src/game/cache.h"
    "src/game/combat_defs.h"
    "src/game/combat.cc"
    "src/game/combat.h"
    "src/game/combatai.cc"
    "src/game/combatai.h"
    "src/game/config.cc"
    "src/game/config.h"
    "src/game/credits.cc"
    "src/game/credits.h"
    "src/game/critter.cc"
    "src/game/critter.h"
    "src/game/cycle.cc"
    "src/game/cycle.h"
    "src/game/display.cc"
    "src/game/display.h"
    "src/game/editor.cc"
    "src/game/editor.h"
    "src/game/elevator.cc"
    "src/game/elevator.h"
    "src/game/endgame.cc"
    "src/game/endgame.h"
    "src/game/fontmgr.cc"
    "src/game/fontmgr.h"
    "src/game/game_vars.h"
    "src/game/game.cc"
    "src/game/game.h"
    "src/game/gconfig.cc"
    "src/game/gconfig.h"
    "src/game/gdebug.cc"
    "src/game/gdebug.h"
    "src/game/gdialog.cc"
    "src/game/gdialog.h"
    "src/game/gmemory.cc"
    "src/game/gmemory.h"
    "src/game/gmouse.cc"
    "src/game/gmouse.h"
    "src/game/gmovie.cc"
    "src/game/gmovie.h"
    "src/game/graphlib.cc"
    "src/game/graphlib.h"
    "src/game/gsound.cc"
    "src/game/gsound.h"
    "src/game/heap.cc"
    "src/game/heap.h"
    "src/game/intface.cc"
    "src/game/intface.h"
    "src/game/inventry.cc"
    "src/game/inventry.h"
    "src/game/item.cc"
    "src/game/item.h"
    "src/game/light.cc"
    "src/game/light.h"
    "src/game/lip_sync.cc"
    "src/game/lip_sync.h"
    "src/game/loadsave.cc"
    "src/game/loadsave.h"
    "src/game/main.cc"
    "src/game/main.h"
    "src/game/mainmenu.cc"
    "src/game/mainmenu.h"
    "src/game/map_defs.h"
    "src/game/map.cc"
    "src/game/map.h"
    "src/game/message.cc"
    "src/game/message.h"
    "src/game/moviefx.cc"
    "src/game/moviefx.h"
    "src/game/object_types.h"
    "src/game/object.cc"
    "src/game/object.h"
    "src/game/options.cc"
    "src/game/options.h"
    "src/game/palette.cc"
    "src/game/palette.h"
    "src/game/party.cc"
    "src/game/party.h"
    "src/game/perk_defs.h"
    "src/game/perk.cc"
    "src/game/perk.h"
    "src/game/pipboy.cc"
    "src/game/pipboy.h"
    "src/game/protinst.cc"
    "src/game/protinst.h"
    "src/game/proto_types.h"
    "src/game/proto.cc"
    "src/game/proto.h"
    "src/game/queue.cc"
    "src/game/queue.h"
    "src/game/reaction.cc"
    "src/game/reaction.h"
    "src/game/roll.cc"
    "src/game/roll.h"
    "src/game/scripts.cc"
    "src/game/scripts.h"
    "src/game/select.cc"
    "src/game/select.h"
    "src/game/selfrun.cc"
    "src/game/selfrun.h"
    "src/game/sfxcache.cc"
    "src/game/sfxcache.h"
    "src/game/sfxlist.cc"
    "src/game/sfxlist.h"
    "src/game/skill_defs.h"
    "src/game/skill.cc"
    "src/game/skill.h"
    "src/game/skilldex.cc"
    "src/game/skilldex.h"
    "src/game/stat_defs.h"
    "src/game/stat.cc"
    "src/game/stat.h"
    "src/game/textobj.cc"
    "src/game/textobj.h"
    "src/game/tile.cc"
    "src/game/tile.h"
    "src/game/trait.cc"
    "src/game/trait.h"
    "src/game/version.cc"
    "src/game/version.h"
    "src/game/wordwrap.cc"
    "src/game/wordwrap.h"
    "src/game/worldmap_walkmask.cc"
    "src/game/worldmap_walkmask.h"
    "src/game/worldmap.cc"
    "src/game/worldmap.h"
    "src/int/audio.cc"
    "src/int/audio.h"
    "src/int/audiof.cc"
    "src/int/audiof.h"
    "src/int/datafile.cc"
    "src/int/datafile.h"
    "src/int/dialog.cc"
    "src/int/dialog.h"
    "src/int/export.cc"
    "src/int/export.h"
    "src/int/intlib.cc"
    "src/int/intlib.h"
    "src/int/intrpret.cc"
    "src/int/intrpret.h"
    "src/int/memdbg.cc"
    "src/int/memdbg.h"
    "src/int/mousemgr.cc"
    "src/int/mousemgr.h"
    "src/int/movie.cc"
    "src/int/movie.h"
    "src/int/nevs.cc"
    "src/int/nevs.h"
    "src/int/pcx.cc"
    "src/int/pcx.h"
    "src/int/region.cc"
    "src/int/region.h"
    "src/int/share1.cc"
    "src/int/share1.h"
    "src/int/sound.cc"
    "src/int/sound.h"
    "src/int/support/intextra.cc"
    "src/int/support/intextra.h"
    "src/int/widget.cc"
    "src/int/widget.h"
    "src/int/window.cc"
    "src/int/window.h"
    "src/plib/assoc/assoc.cc"
    "src/plib/assoc/assoc.h"
    "src/plib/color/color.cc"
    "src/plib/color/color.h"
    "src/plib/db/db.cc"
    "src/plib/db/db.h"
    "src/plib/db/lzss.cc"
    "src/plib/db/lzss.h"
    "src/plib/gnw/button.cc"
    "src/plib/gnw/button.h"
    "src/plib/gnw/debug.cc"
    "src/plib/gnw/debug.h"
    "src/plib/gnw/dxinput.cc"
    "src/plib/gnw/dxinput.h"
    "src/plib/gnw/grbuf.cc"
    "src/plib/gnw/grbuf.h"
    "src/plib/gnw/input.cc"
    "src/plib/gnw/input.h"
    "src/plib/gnw/gnw_types.h"
    "src/plib/gnw/gnw.cc"
    "src/plib/gnw/gnw.h"
    "src/plib/gnw/intrface.cc"
    "src/plib/gnw/intrface.h"
    "src/plib/gnw/kb.cc"
    "src/plib/gnw/kb.h"
    "src/plib/gnw/memory.cc"
    "src/plib/gnw/memory.h"
    "src/plib/gnw/mouse.cc"
    "src/plib/gnw/mouse.h"
    "src/plib/gnw/rect.cc"
    "src/plib/gnw/rect.h"
    "src/plib/gnw/svga_types.h"
    "src/plib/gnw/svga.cc"
    "src/plib/gnw/svga.h"
    "src/plib/gnw/text.cc"
    "src/plib/gnw/text.h"
    "src/plib/gnw/vcr.cc"
    "src/plib/gnw/vcr.h"
    "src/plib/gnw/winmain.h"
    "src/movie_lib.cc"
    "src/movie_lib.h"
)

target_sources(${EXECUTABLE_NAME} PUBLIC
    "src/audio_engine.cc"
    "src/audio_engine.h"
    "src/fps_limiter.cc"
    "src/fps_limiter.h"
    "src/platform_compat.h"
    "src/pointer_registry.cc"
    "src/pointer_registry.h"
    "src/plib/gnw/touch.cc"
    "src/plib/gnw/touch.h"
)

# winmain.cc declares the desktop SDL entry point; on PS2 the platform layer
# provides its own main() (src/platform/ps2/main.cc) so the platform can be
# brought up before the engine starts. platform_compat.cc is likewise replaced
# on PS2 by src/platform/ps2/platform_compat_ps2.cc (device-aware paths).
if(NOT PS2)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "src/plib/gnw/winmain.cc"
        "src/platform_compat.cc"
    )
endif()

if(PS2)
    # Native PlayStation 2 platform layer (C99) + two C++ shims (entry point
    # and the SDL2-subset implementation). The F5 audio worker streams mixed
    # PCM to the SPU2 through audsrv.
    target_sources(${EXECUTABLE_NAME} PRIVATE
        "src/platform/ps2/main.cc"
        "src/platform/ps2/boot.c"
        "src/platform/ps2/ps2_debug.c"
        "src/platform/ps2/ps2_time.c"
        "src/platform/ps2/ps2_mem.c"
        "src/platform/ps2/input_pad.c"
        "src/platform/ps2/video_gs.c"
        "src/platform/ps2/ps2_osk.c"
        "src/platform/ps2/fs.c"
        "src/platform/ps2/ps2_save.c"
        "src/platform/ps2/ps2_storage.c"
        "src/platform/ps2/audio_audsrv.c"
        "src/platform/ps2/platform_compat_ps2.cc"
        "src/platform/ps2/sdl/sdl_shim.c"
    )

    # Automated smoke-test harness (dev only, see tools/run_smoke.sh).
    if(PS2_DEV_SMOKE_TEST)
        message(STATUS "PS2 smoke-test harness enabled (PS2_DEV_SMOKE_TEST=ON)")
        target_sources(${EXECUTABLE_NAME} PRIVATE
            "src/game/ps2_smoke.cc"
            "src/game/ps2_smoke.h"
        )
        target_compile_definitions(${EXECUTABLE_NAME} PRIVATE PS2_DEV_SMOKE_TEST=1)
    endif()

    # audsrv.irx (ps2sdk, BSD/AFL-2.0) is embedded into the ELF; on a real
    # PS2 that embedded copy is the module that actually gets used, so no
    # loose files need to ship next to the game data.
    file(READ "${PS2SDK}/iop/irx/audsrv.irx" _ps2_audsrv_irx_hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," _ps2_audsrv_irx_bytes "${_ps2_audsrv_irx_hex}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/audsrv_irx_data.c"
        "/* Generated from ps2sdk audsrv.irx - do not edit. */\n"
        "/* aligned(16): SifExecModuleBuffer DMAs from this pointer and the EE\n"
        " * DMAC silently ignores the low four address bits. */\n"
        "const unsigned char g_audsrv_irx[] __attribute__((aligned(16))) = { ${_ps2_audsrv_irx_bytes} 0x00 };\n"
        "const unsigned int g_audsrv_irx_size = sizeof(g_audsrv_irx) - 1;\n")
    target_sources(${EXECUTABLE_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/audsrv_irx_data.c")

    # F9: IOP drivers for USB mass storage are embedded the same way, so a
    # raw-ELF boot (uLaunchELF "Launch Elf", no preloaded IOP stack) can read
    # game data from mass: without shipping loose .irx files. The tiered
    # loading in ps2_storage.c only touches them when the device is not
    # already provided by the loader (OPL/uLaunchELF/ps2link/PCSX2).
    set(_ps2_storage_irx_modules usbd usbhdfsd bdm usbmass_bd bdmfs_fatfs sio2man mx4sio_bd)
    set(_ps2_storage_irx_sources "")
    foreach(_ps2_irx ${_ps2_storage_irx_modules})
        file(READ "${PS2SDK}/iop/irx/${_ps2_irx}.irx" _ps2_irx_hex HEX)
        string(REGEX REPLACE "(..)" "0x\\1," _ps2_irx_bytes "${_ps2_irx_hex}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ps2_${_ps2_irx}_irx_data.c"
            "/* Generated from ps2sdk ${_ps2_irx}.irx - do not edit. */\n"
            "/* aligned(16): required by the SifExecModuleBuffer EE->IOP DMA. */\n"
            "const unsigned char g_ps2_${_ps2_irx}_irx[] __attribute__((aligned(16))) = { ${_ps2_irx_bytes} 0x00 };\n"
            "const unsigned int g_ps2_${_ps2_irx}_irx_size = sizeof(g_ps2_${_ps2_irx}_irx) - 1;\n")
        list(APPEND _ps2_storage_irx_sources "${CMAKE_CURRENT_BINARY_DIR}/ps2_${_ps2_irx}_irx_data.c")
    endforeach()
    target_sources(${EXECUTABLE_NAME} PRIVATE ${_ps2_storage_irx_sources})

    # The same module is also dropped next to the ELF. Under PCSX2 the
    # EE->IOP DMA route of SifExecModuleBuffer is broken by the IOP HLE (the
    # module loads on paper but never runs), and SifLoadModule of this file
    # is the only working route there; PCSX2 resolves "host:" to the ELF's
    # directory. See src/platform/ps2/audio_audsrv.c.
    add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PS2SDK}/iop/irx/audsrv.irx"
                "${CMAKE_CURRENT_BINARY_DIR}/audsrv.irx"
        COMMENT "Copying audsrv.irx next to the game ELF (PCSX2 host: root)"
    )

    # The SDL2-subset shim (src/platform/ps2/sdl/SDL.h) must win over any
    # other SDL.h on the include path.
    target_include_directories(${EXECUTABLE_NAME} BEFORE PRIVATE "src/platform/ps2/sdl")
    target_include_directories(${EXECUTABLE_NAME} PRIVATE "src/platform/ps2")

    target_compile_options(${EXECUTABLE_NAME} PRIVATE
        -Wcast-align=strict
        -fno-strict-aliasing
    )

    # Requested optimization profile for the EE (R5900 has no hardware FP64).
    # NOTE: -fsingle-precision-constant is applied only to the PS2 platform
    # layer (see below), not to the upstream engine: turning every floating
    # literal into `float` breaks upstream code (e.g. std::clamp<double>
    # overload resolution in game/options.cc).
    target_compile_options(${EXECUTABLE_NAME} PRIVATE
        $<$<CONFIG:Release>:-ffast-math>
    )

    # PS2 platform layer: single-precision floating constants are fine here.
    # -Wno-cast-align: this layer deliberately casts between byte buffers and
    # typed DMA/audio buffers (all accesses are statically aligned in
    # practice); the strict cast-align audit stays enabled for the engine.
    set_source_files_properties(
        src/platform/ps2/boot.c
        src/platform/ps2/ps2_debug.c
        src/platform/ps2/ps2_time.c
        src/platform/ps2/ps2_mem.c
        src/platform/ps2/input_pad.c
        src/platform/ps2/video_gs.c
        src/platform/ps2/ps2_osk.c
        src/platform/ps2/fs.c
        src/platform/ps2/audio_audsrv.c
        src/platform/ps2/platform_compat_ps2.cc
        src/platform/ps2/sdl/sdl_shim.c
        PROPERTIES COMPILE_OPTIONS "-fsingle-precision-constant;-Wno-cast-align"
    )

    # Platform libraries: gsKit (+ DMA/kit utilities), GS graph primitives,
    # libpad for DualShock 2 input, libaudsrv for SPU2 streaming.
    target_link_libraries(${EXECUTABLE_NAME}
        gskit
        dmakit
        graph
        pad
        audsrv
        m
    )

    # Canonical PS2 artifact name: fallout1-ce.elf (PCSX2 needs the .elf
    # extension to boot the file directly).
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES
        OUTPUT_NAME "fallout1-ce"
        SUFFIX ".elf"
    )
endif()

if(IOS)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "src/platform/ios/paths.h"
        "src/platform/ios/paths.mm"
    )
endif()

if(WIN32)
    target_compile_definitions(${EXECUTABLE_NAME} PUBLIC
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

if(WIN32)
    target_link_libraries(${EXECUTABLE_NAME}
        winmm
    )
endif()

if (WIN32)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "os/windows/fallout-ce.ico"
        "os/windows/fallout-ce.rc"
    )
endif()

if(APPLE)
    if(IOS)
        set(RESOURCES
            "os/ios/AppIcon.xcassets"
            "os/ios/LaunchScreen.storyboard"
        )

        target_sources(${EXECUTABLE_NAME} PUBLIC ${RESOURCES})
        set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/os/ios/Info.plist"
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.alexbatalov.fallout-ce"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        )

        set(MACOSX_BUNDLE_BUNDLE_NAME "${EXECUTABLE_NAME}")
        set(MACOSX_BUNDLE_DISPLAY_NAME "Fallout")
    else()
        set(RESOURCES
            "os/macos/fallout-ce.icns"
        )

        target_sources(${EXECUTABLE_NAME} PUBLIC ${RESOURCES})
        set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            OUTPUT_NAME "Fallout Community Edition"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/os/macos/Info.plist"
            XCODE_ATTRIBUTE_EXECUTABLE_NAME "${EXECUTABLE_NAME}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.alexbatalov.fallout-ce"
        )

        set(MACOSX_BUNDLE_ICON_FILE "fallout-ce.icns")
        set(MACOSX_BUNDLE_BUNDLE_NAME "Fallout: Community Edition")
        set(MACOSX_BUNDLE_DISPLAY_NAME "Fallout")
    endif()

    set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.alexbatalov.fallout-ce")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0")
endif()

if(NOT PS2)
    if((NOT ${CMAKE_SYSTEM_NAME} MATCHES "Linux") AND (NOT ${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD") AND (NOT ${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD"))
        add_subdirectory("third_party/sdl2")
    else()
        find_package(SDL2)
    endif()

    target_link_libraries(${EXECUTABLE_NAME} ${SDL2_LIBRARIES})
    target_include_directories(${EXECUTABLE_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})
endif()

add_subdirectory("third_party/adecode")
target_link_libraries(${EXECUTABLE_NAME} adecode::adecode)

add_subdirectory("third_party/fpattern")
target_link_libraries(${EXECUTABLE_NAME} fpattern::fpattern)

if(APPLE)
    if(IOS)
        install(TARGETS ${EXECUTABLE_NAME} DESTINATION "Payload")

        set(CPACK_GENERATOR "ZIP")
        set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
        set(CPACK_PACKAGE_FILE_NAME "fallout-ce")
        set(CPACK_ARCHIVE_FILE_EXTENSION "ipa")
    else()
        install(TARGETS ${EXECUTABLE_NAME} DESTINATION .)

        set(CPACK_GENERATOR "DragNDrop")
        set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK ON)
        set(CPACK_PACKAGE_FILE_NAME "Fallout Community Edition")
    endif()

    include(CPack)
endif()
