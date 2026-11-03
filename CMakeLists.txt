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
  src/hle/status.c
  "${GEN_C}"
)

option(NANPA2_SDL_RENDERER "Use the SDL_Renderer presenter on macOS too" OFF)
if(APPLE AND NOT NANPA2_SDL_RENDERER)
  enable_language(OBJC)
  list(APPEND SOURCES src/platform/present_metal.m)
else()
  list(APPEND SOURCES src/platform/present_sdl.c)
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
  target_link_libraries(nanpa2 PRIVATE "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit" iconv)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(nanpa2 PRIVATE -Wall -Wextra -Wno-unused-parameter)
  # generated code: labels and comparisons it never uses
  set_source_files_properties("${GEN_C}" PROPERTIES COMPILE_OPTIONS
    "-Wno-unused-label;-Wno-unused-value;-Wno-unused-variable;-Wno-parentheses-equality;-O1")
endif()

add_executable(decode_all tests/decode_all.c src/lzh.c src/fsys.c)
target_include_directories(decode_all PRIVATE src)

# Smoke test of a plain interactive launch (a real window on this desktop,
# about 45 s): ctest --test-dir build -R smoke --output-on-failure
set(NANPA2_GAME_DIR "${CMAKE_SOURCE_DIR}/game" CACHE PATH "Game folder for the tests")
enable_testing()
add_test(NAME smoke COMMAND "${NANPA2_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/smoke.py"
  $<TARGET_FILE:nanpa2> "${NANPA2_GAME_DIR}")
set_tests_properties(smoke PROPERTIES TIMEOUT 120)
