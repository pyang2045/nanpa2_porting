cmake_minimum_required(VERSION 3.16)
project(nanpa2 C)

# iOS / iPadOS (tools/build_ios.sh): CMAKE_SYSTEM_NAME=iOS. APPLE is true there too, so the macOS-only parts
# (AppKit, the folder picker, bundle_macos.sh) test NANPA2_MACOS.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(NANPA2_IOS ON)
elseif(APPLE)
  set(NANPA2_MACOS ON)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(SDL2 REQUIRED)

# The engine is plain C: the game logic (src/game) runs on the game state G (state.h) and calls the
# platform API (src/platform: files, surfaces, text, window, sound; docs/refactor.md). Nothing of the exe's
# code is used, so the build needs neither the exe nor any tool beyond a C11 compiler and SDL2, and the
# binary holds nothing of the game.
# Python: for the tests.
find_package(Python3 COMPONENTS Interpreter)
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/.venv/bin/python")
  set(_py "${CMAKE_SOURCE_DIR}/tools/.venv/bin/python")
else()
  set(_py "${Python3_EXECUTABLE}")
endif()
set(NANPA2_PYTHON "${_py}" CACHE FILEPATH "Python for the tests")

# Game logic reimplemented in C (docs/reimpl.md), without SDL or HLE
# dependencies, so the differential replay links it alone.
set(GAME_SOURCES
  src/game/ctl.c
  src/game/expr.c
  src/game/vm.c
  src/game/ops.c
  src/game/ops_sys.c
  src/game/ops_vm.c
  src/game/loop.c
  src/game/text.c
  src/game/anim.c
  src/game/file.c
  src/game/startup.c
  src/game/map.c
  src/game/map_draw.c
  src/game/map_move.c
  src/game/map_path.c
  src/game/map_sprite.c
  src/game/map_pump.c
  src/game/screen.c
  src/game/wipe.c
  src/game/state_view.c
  src/game/state.c
  src/game/timer.c
)

set(SOURCES
  src/main.c
  src/launch.c
  src/lzh.c
  src/gp4.c
  src/fsys.c
  src/trap.c
  src/hooks.c
  src/hash.c
    src/platform/files.c
  src/platform/window.c
  src/platform/clock.c
  src/platform/surface.c
  src/platform/text.c
  src/platform/textlayer.c
  src/platform/logo.c
  src/cinepak.c
  src/platform/sound.c
  src/dev/dev.c
  src/dev/replay.c
  src/dev/explore.c
  src/dev/status.c
  ${GAME_SOURCES}
)

option(NANPA2_SDL_RENDERER "Use the SDL_Renderer presenter on macOS too" OFF)
if(APPLE)
  enable_language(OBJC)
endif()
if(APPLE AND NOT NANPA2_SDL_RENDERER)
  list(APPEND SOURCES src/platform/present_metal.m)
else()
  list(APPEND SOURCES src/platform/present_sdl.c)
endif()
if(NANPA2_MACOS)
  list(APPEND SOURCES src/platform/desktop_macos.c src/platform/dialog_macos.m)
else()
  list(APPEND SOURCES src/platform/dialog_none.c)
endif()
if(NANPA2_IOS)
  list(APPEND SOURCES src/platform/docs_ios.m)
endif()

if(NANPA2_IOS)
  # the .app: Info.plist from cmake/Info.ios.plist.in (landscape, full screen, file sharing)
  set(NANPA2_APP_NAME "同級生２")
  set(NANPA2_APP_ID "local.nanpa2" CACHE STRING "Bundle identifier of the .app")
  set(NANPA2_IOS_MIN "13.0" CACHE STRING "Minimum iOS version")
  configure_file(cmake/Info.ios.plist.in "${CMAKE_BINARY_DIR}/Info.ios.plist" @ONLY)
  add_executable(nanpa2 MACOSX_BUNDLE ${SOURCES})
  set_target_properties(nanpa2 PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/Info.ios.plist"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${NANPA2_APP_ID}")
else()
  add_executable(nanpa2 ${SOURCES})
endif()
target_include_directories(nanpa2 PRIVATE src third_party)
# The default build is unoptimised (Debug); the glyph rasteriser (stb_truetype in text.c, used by the text
# layer for every new glyph at the display's resolution, textlayer.c) is not fast enough that way: optimise
# these two files whatever the build type. Neither is part of the game's own computation.
if(NOT MSVC)
  set_source_files_properties(src/platform/text.c src/platform/textlayer.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
if(TARGET SDL2::SDL2)
  target_link_libraries(nanpa2 PRIVATE SDL2::SDL2)
else()
  target_include_directories(nanpa2 PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(nanpa2 PRIVATE ${SDL2_LIBRARIES})
endif()
if(TARGET SDL2::SDL2main)
  target_link_libraries(nanpa2 PRIVATE SDL2::SDL2main)
endif()
# Windows (untested, tools/build_msys2.sh): a GUI program, so no console
# window opens next to the game (set NANPA2_LOG_FILE for the log); SDL2main
# supplies WinMain and calls main (src/main.c includes <SDL.h>). Named
# nanpa2-port.exe so it can never overwrite the game's own nanpa2.exe, which
# the port reads at run time.
if(WIN32)
  if(NOT TARGET SDL2::SDL2main AND NOT SDL2_LIBRARIES MATCHES "SDL2main")
    message(FATAL_ERROR "SDL2main not found: the GUI program needs it for WinMain")
  endif()
  set_target_properties(nanpa2 PROPERTIES WIN32_EXECUTABLE ON OUTPUT_NAME nanpa2-port)
endif()
if(APPLE)
  target_compile_options(nanpa2 PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
  target_link_libraries(nanpa2 PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework CoreGraphics" "-framework CoreFoundation" iconv)
endif()
if(NANPA2_MACOS)
  target_link_libraries(nanpa2 PRIVATE "-framework AppKit")
elseif(NANPA2_IOS)
  target_link_libraries(nanpa2 PRIVATE "-framework UIKit")
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(nanpa2 PRIVATE m) # stb_truetype, minimp3
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nanpa2 PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
# macOS app bundle: build/同級生２.app with SDL inside (tools/bundle_macos.sh).
# It holds no game data (the game folder is found or asked for at run time,
# see src/launch.c). It holds nothing of the game either (test no_game_bytes).
#   ninja -C build nanpa2_app        or configure with -DNANPA2_APP_BUNDLE=ON
if(NANPA2_MACOS)
  option(NANPA2_APP_BUNDLE "Build build/同級生２.app with the default target" OFF)
  set(NANPA2_APP_NAME "同級生２")
  set(NANPA2_APP_ID "local.nanpa2" CACHE STRING "Bundle identifier of the .app")
  set(NANPA2_APP_DIR "${CMAKE_BINARY_DIR}/${NANPA2_APP_NAME}.app")
  configure_file(cmake/Info.plist.in "${CMAKE_BINARY_DIR}/Info.plist" @ONLY)
  if(NANPA2_APP_BUNDLE)
    set(_app_all ALL)
  endif()
  add_custom_target(nanpa2_app ${_app_all}
    COMMAND /bin/sh "${CMAKE_SOURCE_DIR}/tools/bundle_macos.sh"
      "${NANPA2_APP_DIR}" $<TARGET_FILE:nanpa2> "${CMAKE_BINARY_DIR}/Info.plist"
    DEPENDS nanpa2 "${CMAKE_SOURCE_DIR}/tools/bundle_macos.sh" "${CMAKE_BINARY_DIR}/Info.plist"
    COMMENT "Bundling ${NANPA2_APP_NAME}.app"
    VERBATIM)
endif()

if(NOT NANPA2_IOS)
  add_executable(decode_all tests/decode_all.c src/lzh.c src/fsys.c)
  target_include_directories(decode_all PRIVATE src)
endif()

# The text layer's tracking and validation, with a stub rasteriser (tests/textlayer_test.c, no game data).
add_executable(textlayer_test tests/textlayer_test.c src/platform/textlayer.c)
target_include_directories(textlayer_test PRIVATE src third_party)
if(TARGET SDL2::SDL2)
  target_link_libraries(textlayer_test PRIVATE SDL2::SDL2)
else()
  target_include_directories(textlayer_test PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(textlayer_test PRIVATE ${SDL2_LIBRARIES})
endif()
if(TARGET SDL2::SDL2main)
  target_link_libraries(textlayer_test PRIVATE SDL2::SDL2main)
endif()
if(APPLE)
  target_link_libraries(textlayer_test PRIVATE "-framework Foundation" "-framework CoreFoundation")
endif()
set_source_files_properties(src/platform/textlayer.c PROPERTIES COMPILE_OPTIONS "-O2")

# Smoke test (about 60 s): a plain interactive launch (a real window on this
# desktop) and a headless one: ctest --test-dir build -R smoke --output-on-failure
# Exit 77 = skipped: the game checks passed but nobody could see the screen
# (locked, display asleep, window covered), so presentation was checked
# offscreen only.
set(NANPA2_GAME_DIR "${CMAKE_SOURCE_DIR}/game" CACHE PATH "Game folder for the tests")
enable_testing()
add_test(NAME smoke COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/smoke.py"
  $<TARGET_FILE:nanpa2> "${NANPA2_GAME_DIR}")
set_tests_properties(smoke PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

add_test(NAME textlayer COMMAND textlayer_test)
set_tests_properties(textlayer PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "textlayer test: ok")

# Self-test (a few seconds, headless): a save round trip through the lifted
# save/load code.
add_test(NAME selftest COMMAND ${CMAKE_COMMAND} -E env NANPA2_HEADLESS=1 NANPA2_SAVETEST=1
  "NANPA2_SAVE_DIR=${CMAKE_BINARY_DIR}/selftest-saves" $<TARGET_FILE:nanpa2> --game "${NANPA2_GAME_DIR}")
set_tests_properties(selftest PROPERTIES TIMEOUT 30
  FIXTURES_REQUIRED selftest_dir PASS_REGULAR_EXPRESSION "save selftest: ok"
  FAIL_REGULAR_EXPRESSION "MISMATCH|game trap")
add_test(NAME selftest_dir COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/selftest-saves")
set_tests_properties(selftest_dir PROPERTIES FIXTURES_SETUP selftest_dir)

# Deterministic mode and replay (about 40 s, headless; tests/replay_det.py): a
# recording made at test time from the game data, twice (identical), then
# replayed against its checkpoints, and two tampered recordings must diverge.
add_test(NAME replay_det COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/replay_det.py"
  $<TARGET_FILE:nanpa2> "${NANPA2_GAME_DIR}" "${CMAKE_BINARY_DIR}/replay-det")
set_tests_properties(replay_det PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# Header hygiene: every header in src/ compiles alone, nothing but src/dev and main.c includes a dev
# header, game code includes no SDL (tests/headers.py).
set(_hflags "")
if(CMAKE_OSX_SYSROOT)
  list(APPEND _hflags -isysroot "${CMAKE_OSX_SYSROOT}")
endif()
foreach(_d ${SDL2_INCLUDE_DIRS})
  list(APPEND _hflags -isystem ${_d})
endforeach()
if(TARGET SDL2::SDL2)
  get_target_property(_sdl_inc SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
  foreach(_d ${_sdl_inc})
    list(APPEND _hflags -isystem ${_d})
  endforeach()
endif()
add_test(NAME headers COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/headers.py"
  "${CMAKE_SOURCE_DIR}/src" "${CMAKE_C_COMPILER}" ${_hflags})
set_tests_properties(headers PROPERTIES TIMEOUT 120)

# WAV loader: decoded duration equals header length / byte rate for mono,
# stereo, 11 kHz and files with extra chunks before data.
add_test(NAME wavcheck COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/wavcheck.py"
  $<TARGET_FILE:nanpa2> "${NANPA2_GAME_DIR}" "${CMAKE_BINARY_DIR}/wavcheck")
set_tests_properties(wavcheck PROPERTIES TIMEOUT 30)

  # No bytes of nanpa2.exe in the binary or the .app (if built): every 64-byte
  # run of the exe's sections is looked for (tests/no_game_bytes.py). Needs
  # the exe in the game folder to compare with; skipped without it.
  set(_scan $<TARGET_FILE:nanpa2>)
  if(NANPA2_MACOS)
    list(APPEND _scan "${NANPA2_APP_DIR}")
  endif()
  add_test(NAME no_game_bytes COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/no_game_bytes.py"
    "${NANPA2_GAME_DIR}/nanpa2.exe" ${_scan})
  set_tests_properties(no_game_bytes PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
  # the game folder without nanpa2.exe and nanpa2.dat: the same recording as with them, and a
  # replay of it (about 1 min, headless; tests/standalone.py)
  add_test(NAME standalone COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/standalone.py"
    $<TARGET_FILE:nanpa2> "${NANPA2_GAME_DIR}" "${CMAKE_BINARY_DIR}/standalone")
  set_tests_properties(standalone PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)

