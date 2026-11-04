cmake_minimum_required(VERSION 3.16)
project(nanpa2 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(SDL2 REQUIRED)

# The game code is lifted from your nanpa2.exe at build time; the generated
# C stays in the build folder.
set(NANPA2_EXE "${CMAKE_SOURCE_DIR}/game/nanpa2.exe" CACHE FILEPATH "nanpa2.exe to lift")
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/.venv/bin/python")
  set(_py "${CMAKE_SOURCE_DIR}/tools/.venv/bin/python")
else()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(_py "${Python3_EXECUTABLE}")
endif()
set(NANPA2_PYTHON "${_py}" CACHE FILEPATH "Python with capstone")

set(GEN_C "${CMAKE_BINARY_DIR}/gen/recomp.c")
add_custom_command(
  OUTPUT "${GEN_C}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/gen"
  COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/recomp.py" "${NANPA2_EXE}" "${GEN_C}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/recomp.py" "${NANPA2_EXE}"
  COMMENT "Lifting nanpa2.exe to C"
  VERBATIM)

set(SOURCES
  src/main.c
  src/launch.c
  src/lzh.c
  src/gp4.c
  src/fsys.c
  src/x86/x86.c
  src/hle/hle.c
  src/hle/crt.c
  src/hle/win32.c
  src/hle/ddraw.c
  src/hle/text.c
  src/hle/logo.c
  src/cinepak.c
  src/hle/sound.c
  src/hle/dev.c
  src/hle/explore.c
  src/hle/status.c
  "${GEN_C}"
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
if(APPLE)
  target_compile_options(nanpa2 PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
  target_link_libraries(nanpa2 PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit" "-framework CoreGraphics" "-framework CoreFoundation" iconv)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(nanpa2 PRIVATE m) # stb_truetype, minimp3
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nanpa2 PRIVATE -Wall -Wextra -Wno-unused-parameter)
  # generated code: labels and comparisons it never uses
  set_source_files_properties("${GEN_C}" PROPERTIES COMPILE_OPTIONS
    "-Wno-unused-label;-Wno-unused-value;-Wno-unused-variable;-Wno-parentheses-equality;-O1")
endif()

# macOS app bundle for this machine only: build/同級生２.app with SDL inside
# (tools/bundle_macos.sh). It holds the game code lifted from your
# nanpa2.exe, so never share it; it holds no game data (the game folder is
# found or asked for at run time, see src/launch.c).
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

# WAV loader: decoded duration equals header length / byte rate for mono,
# stereo, 11 kHz and files with extra chunks before data.
add_test(NAME wavcheck COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/wavcheck.py"
  $<TARGET_FILE:nanpa2> "${NANPA2_GAME_DIR}" "${CMAKE_BINARY_DIR}/wavcheck")
set_tests_properties(wavcheck PROPERTIES TIMEOUT 30)
