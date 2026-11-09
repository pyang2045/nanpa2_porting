cmake_minimum_required(VERSION 3.16)
project(nanpa2 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(SDL2 REQUIRED)

# Game code: the C engine (src/game) runs on the emulated address space of
# src/x86 (docs/refactor.md). The interpreter (src/x86/interp.c) still
# dispatches the units by native address until R2 is finished; it decodes no
# game code any more (the game code is all C), so the build needs neither the
# exe nor capstone and the binary holds nothing of the game.
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
  src/game/units.c
  src/game/state_view.c
  src/game/state.c
)

set(SOURCES
  src/main.c
  src/launch.c
  src/assets.c
  src/lzh.c
  src/gp4.c
  src/fsys.c
  src/x86/x86.c
  src/x86/asset.c
  src/x86/interp.c
  src/hle/hle.c
  src/hle/crt.c
  src/hle/win32.c
  src/hle/ddraw.c
  src/hle/text.c
  src/hle/logo.c
  src/cinepak.c
  src/hle/sound.c
  src/hle/dev.c
  src/hle/replay.c
  src/hle/explore.c
  src/hle/status.c
  ${GAME_SOURCES}
  src/game/game.c
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
if(APPLE)
  list(APPEND SOURCES src/platform/desktop_macos.c src/platform/dialog_macos.m)
else()
  list(APPEND SOURCES src/platform/dialog_none.c)
endif()

add_executable(nanpa2 ${SOURCES})
target_include_directories(nanpa2 PRIVATE src third_party)
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
  target_link_libraries(nanpa2 PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit" "-framework CoreGraphics" "-framework CoreFoundation" iconv)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(nanpa2 PRIVATE m) # stb_truetype, minimp3
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nanpa2 PRIVATE -Wall -Wextra -Wno-unused-parameter)
  # the interpreter runs the whole game: optimized in Debug builds too
  set_source_files_properties(src/x86/interp.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()
# macOS app bundle: build/同級生２.app with SDL inside (tools/bundle_macos.sh).
# It holds no game data (the game folder is found or asked for at run time,
# see src/launch.c). Built with NANPA2_BACKEND=interp it holds nothing of the
# game either (test no_game_bytes); a recomp .app holds the game code lifted
# from your nanpa2.exe, so never share that one.
#   ninja -C build nanpa2_app        or configure with -DNANPA2_APP_BUNDLE=ON
if(APPLE)
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

add_executable(decode_all tests/decode_all.c src/lzh.c src/fsys.c)
target_include_directories(decode_all PRIVATE src)

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

# Self-test (a few seconds, headless): a save round trip through the lifted
# save/load code and the message pump's ECX (script op 0x1f).
add_test(NAME selftest COMMAND ${CMAKE_COMMAND} -E env NANPA2_HEADLESS=1 NANPA2_SAVETEST=1
  "NANPA2_SAVE_DIR=${CMAKE_BINARY_DIR}/selftest-saves" $<TARGET_FILE:nanpa2> --game "${NANPA2_GAME_DIR}")
set_tests_properties(selftest PROPERTIES TIMEOUT 30
  FIXTURES_REQUIRED selftest_dir PASS_REGULAR_EXPRESSION "pump selftest: ok"
  FAIL_REGULAR_EXPRESSION "MISMATCH|ECX KEPT|x86 trap")
add_test(NAME selftest_dir COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/selftest-saves")
set_tests_properties(selftest_dir PROPERTIES FIXTURES_SETUP selftest_dir)

# Deterministic mode and replay (about 40 s, headless; tests/replay_det.py): a
# recording made at test time from the game data, twice (identical), then
# replayed against its checkpoints, and two tampered recordings must diverge.
add_test(NAME replay_det COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/replay_det.py"
  $<TARGET_FILE:nanpa2> "${NANPA2_GAME_DIR}" "${CMAKE_BINARY_DIR}/replay-det")
set_tests_properties(replay_det PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# WAV loader: decoded duration equals header length / byte rate for mono,
# stereo, 11 kHz and files with extra chunks before data.
add_test(NAME wavcheck COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/wavcheck.py"
  $<TARGET_FILE:nanpa2> "${NANPA2_GAME_DIR}" "${CMAKE_BINARY_DIR}/wavcheck")
set_tests_properties(wavcheck PROPERTIES TIMEOUT 30)

  # No bytes of nanpa2.exe in the binary or the .app (if built): every 64-byte
  # run of the exe's sections is looked for (tests/no_game_bytes.py). Needs
  # the exe in the game folder to compare with; skipped without it.
  set(_scan $<TARGET_FILE:nanpa2>)
  if(APPLE)
    list(APPEND _scan "${NANPA2_APP_DIR}")
  endif()
  add_test(NAME no_game_bytes COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/no_game_bytes.py"
    "${NANPA2_GAME_DIR}/nanpa2.exe" ${_scan})
  set_tests_properties(no_game_bytes PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
  # nanpa2.dat: cut from the exe, holds no game code; the game runs from it
  # with the exe hidden and matches a run with the whole exe (about 2 min,
  # headless; tests/asset_only.py)
  add_test(NAME asset_only COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/asset_only.py"
    $<TARGET_FILE:nanpa2> "${NANPA2_GAME_DIR}" "${CMAKE_BINARY_DIR}/asset-only")
  set_tests_properties(asset_only PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)

