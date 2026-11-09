#!/bin/sh
# Build the port on Windows in an MSYS2 MINGW64 shell. UNTESTED: written to
# the MSYS2 package names (the route xsystem4 builds its Windows release on)
# but never run; the port has only been built and run on macOS.
#
#   tools/build_msys2.sh [--game-dir DIR] [--build-dir DIR] [--release | --debug]
#                        [--test] [--no-dist]
#
#   --game-dir DIR   game folder for the tests (default: game/; needed only
#                    with --test)
#   --build-dir DIR  build folder (default: build/)
#   --release        CMAKE_BUILD_TYPE=Release (default)
#   --debug          CMAKE_BUILD_TYPE=Debug
#   --test           run ctest afterwards
#   --no-dist        skip BUILD_DIR/dist
#
# Packages (once, from the MSYS2 MINGW64 shell):
#   pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
#     mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-SDL2 \
#     mingw-w64-x86_64-python
#
# Why MSYS2 and not MSVC: CMakeLists.txt passes GCC/Clang flags, and the
# sources use GNU extensions (__typeof__, __builtin_expect, POSIX getline).
# MinGW-w64 GCC is the nearest compiler, and pacman has SDL2. The engine
# assumes a 64-bit little-endian host (an LLP64 Windows build is fine: no code
# depends on the width of long, and the layout of the game state and of the
# FLAG files is fixed-width with static asserts).
#
# The program is built as nanpa2-port.exe, a GUI program (no console window;
# set NANPA2_LOG_FILE to keep its log). BUILD_DIR/dist gets it with the DLLs
# it needs from /mingw64/bin (SDL2.dll and the MinGW runtime). It holds no
# game code or data: run it with --game PATH.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
game=
dir=$here/build
type=Release
test=0
dist=1

usage() { sed -n '2,29p' "$0" >&2; exit 2; }
while [ $# -gt 0 ]; do
	case $1 in
	--game-dir) [ $# -ge 2 ] || usage; game=$2; shift ;;
	--build-dir) [ $# -ge 2 ] || usage; dir=$2; shift ;;
	--release) type=Release ;;
	--debug) type=Debug ;;
	--test) test=1 ;;
	--no-dist) dist=0 ;;
	-h | --help) usage ;;
	*) echo "build_msys2.sh: unknown option $1" >&2; usage ;;
	esac
	shift
done

if [ "${MSYSTEM:-}" != MINGW64 ]; then
	echo "build_msys2.sh: run this from the MSYS2 MINGW64 shell (MSYSTEM=${MSYSTEM:-unset})" >&2
	exit 1
fi
prefix=${MINGW_PREFIX:-/mingw64}

missing=
for t in gcc cmake ninja python objdump; do
	command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
pkg-config --exists sdl2 2>/dev/null || missing="$missing SDL2"
if [ -n "$missing" ]; then
	echo "build_msys2.sh: missing:$missing. Install with:" >&2
	sed -n '15,18p' "$0" | sed 's/^#//' >&2
	exit 1
fi
py=$(command -v python)

[ -n "$game" ] || game=$here/game
# the game folder is needed only for the tests
if [ -d "$game" ]; then
	game=$(cd "$game" && pwd)
elif [ "$test" = 1 ]; then
	echo "build_msys2.sh: game folder $game not found" >&2
	exit 1
fi

set -- -DCMAKE_BUILD_TYPE="$type" -DNANPA2_PYTHON="$py"
[ ! -d "$game" ] || set -- "$@" -DNANPA2_GAME_DIR="$game"
cmake -S "$here" -B "$dir" -G Ninja "$@"
ninja -C "$dir"
if [ "$test" = 1 ]; then
	ctest --test-dir "$dir" --output-on-failure
fi
[ "$dist" = 1 ] || exit 0

# --- dist: the program and the DLLs it imports from /mingw64/bin, followed
# to a fixed point. The imports are read with objdump; nothing is run.
out=$dir/dist
rm -rf "$out"
mkdir -p "$out"
cp "$dir/nanpa2-port.exe" "$out/"
queue="$out/nanpa2-port.exe"
while [ -n "$queue" ]; do
	next=
	for f in $queue; do
		for dll in $(objdump -p "$f" | awk '/DLL Name:/ {print $3}'); do
			[ -f "$prefix/bin/$dll" ] || continue # a Windows system DLL
			[ -f "$out/$dll" ] && continue
			cp "$prefix/bin/$dll" "$out/"
			next="$next $out/$dll"
		done
	done
	queue=$next
done
note="No game code or data is included: the port reads the game files from the game folder
(the CD contents, plus FLAG0..FLAG6 from data1.cab)."
cat >"$out/README.txt" <<EOF
同級生２ native port for Windows, built $(date -u +%Y-%m-%d) from $(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo 'a source tree').
$note

Run: nanpa2-port.exe --game C:\\path\\to\\your\\game\\folder
Log: set NANPA2_LOG_FILE=C:\\path\\nanpa2.log (no console window opens).
EOF
echo "build_msys2.sh: $out:"
ls "$out"
