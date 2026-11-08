#!/bin/sh
# Build the port on Windows in an MSYS2 MINGW64 shell. UNTESTED: written to
# the MSYS2 package names (the route xsystem4 builds its Windows release on)
# but never run; the port has only been built and run on macOS.
#
#   tools/build_msys2.sh [--backend interp|recomp] [--exe PATH] [--game-dir DIR]
#                        [--build-dir DIR] [--release | --debug] [--test] [--no-dist]
#
#   --backend B      passed on as -DNANPA2_BACKEND=B (default interp); as in
#                    tools/build.sh, only recomp needs --exe and capstone.
#   --exe PATH       recomp: your nanpa2.exe (default: game/nanpa2.exe); the
#                    game code is lifted from it at build time, read, never run.
#   --game-dir DIR   game folder for the tests (default: the folder of the
#                    exe, or game/ with --backend interp)
#   --build-dir DIR  build folder (default: build/)
#   --release        CMAKE_BUILD_TYPE=Release (default)
#   --debug          CMAKE_BUILD_TYPE=Debug
#   --test           run ctest afterwards
#   --no-dist        skip BUILD_DIR/dist
#
# Packages (once, from the MSYS2 MINGW64 shell):
#   pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
#     mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-SDL2 \
#     mingw-w64-x86_64-python mingw-w64-x86_64-python-capstone
# (no capstone package: mingw-w64-x86_64-python-pip, then
#  python -m pip install "capstone>=5.0.1,<6")
#
# Why MSYS2 and not MSVC: CMakeLists.txt passes GCC/Clang flags, and the
# generated recomp.c (one very large function) has only been compiled with
# Clang. MinGW-w64 GCC is the nearest compiler, and pacman has SDL2 and
# capstone (capstone for --backend recomp only).
#
# The program is built as nanpa2-port.exe, a GUI program (no console window;
# set NANPA2_LOG_FILE to keep its log). BUILD_DIR/dist gets it with the DLLs
# it needs from /mingw64/bin (SDL2.dll and the MinGW runtime). It holds no
# game data: run it with --game PATH. An interp dist holds nothing of the
# game; a recomp dist holds the game code lifted from your nanpa2.exe: for
# this machine only, never share that one.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
backend=
exe=$here/game/nanpa2.exe
game=
dir=$here/build
type=Release
test=0
dist=1

usage() { sed -n '2,37p' "$0" >&2; exit 2; }
while [ $# -gt 0 ]; do
	case $1 in
	--backend) [ $# -ge 2 ] || usage; backend=$2; shift ;;
	--exe) [ $# -ge 2 ] || usage; exe=$2; shift ;;
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

case $backend in
'' | recomp | interp) ;;
*) echo "build_msys2.sh: --backend is interp or recomp" >&2; exit 2 ;;
esac
recomp=0
[ "$backend" = recomp ] && recomp=1

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
	sed -n '22,26p' "$0" | sed 's/^#//' >&2
	exit 1
fi
py=$(command -v python)
if [ "$recomp" = 1 ]; then
	"$py" -c 'import capstone' 2>/dev/null || {
		echo "build_msys2.sh: $py has no capstone:" >&2
		echo "  pacman -S --needed mingw-w64-x86_64-python-capstone" >&2
		exit 1
	}
	echo "build_msys2.sh: Python $("$py" -c 'import sys, capstone; print(sys.version.split()[0], "capstone", capstone.__version__)')"

	if [ ! -f "$exe" ]; then
		echo "build_msys2.sh: $exe not found." >&2
		echo "  Copy your 同級生２ CD into game/ and extract nanpa2.exe and FLAG0..FLAG6 from" >&2
		echo "  data1.cab there (see game/README.md), or pass --exe PATH." >&2
		exit 1
	fi
	exe=$(cd "$(dirname "$exe")" && pwd)/$(basename "$exe")
	[ -n "$game" ] || game=$(dirname "$exe")
else
	[ -n "$game" ] || game=$here/game
fi
# interp builds need the game folder only for the tests
if [ -d "$game" ]; then
	game=$(cd "$game" && pwd)
elif [ "$recomp" = 1 ] || [ "$test" = 1 ]; then
	echo "build_msys2.sh: game folder $game not found" >&2
	exit 1
fi

set -- -DCMAKE_BUILD_TYPE="$type" -DNANPA2_PYTHON="$py" -DNANPA2_BACKEND="${backend:-interp}"
[ ! -d "$game" ] || set -- "$@" -DNANPA2_GAME_DIR="$game"
[ "$recomp" = 0 ] || set -- "$@" -DNANPA2_EXE="$exe"
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
if [ "$recomp" = 1 ]; then
	note="FOR THIS MACHINE ONLY, do not share or upload: nanpa2-port.exe holds the game
code lifted from your nanpa2.exe. No game data is included."
else
	note="No game code or data is included: the port runs the game code of your own
nanpa2.exe from the game folder (extract it and FLAG0..FLAG6 from data1.cab on
the CD)."
fi
cat >"$out/README.txt" <<EOF
同級生２ native port for Windows, built $(date -u +%Y-%m-%d) from $(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo 'a source tree').
$note

Run: nanpa2-port.exe --game C:\\path\\to\\your\\game\\folder
Log: set NANPA2_LOG_FILE=C:\\path\\nanpa2.log (no console window opens).
EOF
echo "build_msys2.sh: $out:"
ls "$out"
[ "$recomp" = 0 ] || echo "build_msys2.sh: recomp: for this machine only, never share or upload it"
