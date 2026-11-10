#!/bin/sh
# Build the port on macOS or Linux (for Windows see tools/build_msys2.sh).
#
#   tools/build.sh [--game-dir DIR] [--build-dir DIR] [--release | --debug] [--test] [--app]
#
#   --game-dir DIR   game folder for the tests (default: game/; needed only
#                    with --test)
#   --build-dir DIR  build folder (default: build/)
#   --release        CMAKE_BUILD_TYPE=Release (default)
#   --debug          CMAKE_BUILD_TYPE=Debug
#   --test           run ctest afterwards (the smoke test opens a window for
#                    about a minute; "Skipped" means passed, screen not seen)
#   --app            macOS only: also build the app bundle (target nanpa2_app,
#                    BUILD_DIR/同級生２.app)
#
# The engine is plain C on SDL3: it needs a C11 compiler, CMake, Ninja and
# SDL3 (and Python 3 for the tests), nothing else. It runs on 64-bit
# little-endian hosts (state.h refuses to build otherwise). The binary and the
# .app hold nothing of the game (ctest no_game_bytes): the player supplies the
# game folder (no nanpa2.exe needed). Missing tools are named with the install command for this system;
# nothing is installed with sudo. Re-running reuses the build folder.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
game=
dir=$here/build
type=Release
test=0
app=0

usage() { sed -n '2,19p' "$0" >&2; exit 2; }
while [ $# -gt 0 ]; do
	case $1 in
	--game-dir) [ $# -ge 2 ] || usage; game=$2; shift ;;
	--build-dir) [ $# -ge 2 ] || usage; dir=$2; shift ;;
	--release) type=Release ;;
	--debug) type=Debug ;;
	--test) test=1 ;;
	--app) app=1 ;;
	-h | --help) usage ;;
	*) echo "build.sh: unknown option $1" >&2; usage ;;
	esac
	shift
done

os=$(uname -s)
case $os in
Darwin | Linux) ;;
MINGW* | MSYS* | CYGWIN*)
	echo "build.sh: on Windows use tools/build_msys2.sh in an MSYS2 MINGW64 shell" >&2
	exit 1 ;;
*) echo "build.sh: $os is not supported (macOS and Linux; Windows: tools/build_msys2.sh)" >&2
	exit 1 ;;
esac
if [ "$app" = 1 ] && [ "$os" != Darwin ]; then
	echo "build.sh: --app is macOS only" >&2
	exit 2
fi

# --- dependencies: check, and name the packages when something is missing
hint() {
	if [ "$os" = Darwin ]; then
		echo "  brew install sdl3 cmake ninja python   (and xcode-select --install for the compiler)"
	else
		echo "  Debian/Ubuntu: sudo apt install build-essential cmake ninja-build pkg-config libsdl3-dev python3 (Debian 13 / Ubuntu 25.04 or later; else build SDL3 from source, README Linux)"
		echo "  Fedora:        sudo dnf install gcc cmake ninja-build pkgconf-pkg-config SDL3-devel python3"
		echo "  Arch:          sudo pacman -S --needed base-devel cmake ninja sdl3 python"
	fi
}
missing=
for t in cmake ninja cc python3; do
	command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
if [ -n "$missing" ]; then
	echo "build.sh: missing:$missing. Install them with:" >&2
	hint >&2
	exit 1
fi
sdl=0
if pkg-config --exists sdl3 2>/dev/null; then
	sdl=1
elif [ "$os" = Darwin ] && command -v brew >/dev/null 2>&1 && brew list --versions sdl3 >/dev/null 2>&1; then
	sdl=1
fi
if [ "$sdl" = 0 ]; then
	# CMake may still find it (SDL3_DIR, CMAKE_PREFIX_PATH); its error follows if not
	echo "build.sh: SDL3 development files not found by pkg-config. Install with:" >&2
	hint >&2
fi

[ -n "$game" ] || game=$here/game
# the game folder is needed only for the tests
if [ -d "$game" ]; then
	game=$(cd "$game" && pwd)
elif [ "$test" = 1 ]; then
	echo "build.sh: game folder $game not found" >&2
	exit 1
fi

# --- configure and build (both no-ops when nothing changed). The cache
# variables are passed every time so a reused build folder never keeps stale
# values.
set -- -DCMAKE_BUILD_TYPE="$type" -DNANPA2_PYTHON="$(command -v python3)"
[ ! -d "$game" ] || set -- "$@" -DNANPA2_GAME_DIR="$game"
cmake -S "$here" -B "$dir" -G Ninja "$@"
ninja -C "$dir"
if [ "$app" = 1 ]; then
	ninja -C "$dir" nanpa2_app
fi
if [ "$test" = 1 ]; then
	ctest --test-dir "$dir" --output-on-failure
fi

echo "build.sh: built $dir/nanpa2 ($type)"
if [ "$app" = 1 ]; then
	echo "build.sh: app $dir/同級生２.app (no game code or data inside; the player supplies the game folder)"
fi
echo "build.sh: play with: $dir/nanpa2 --game \"$game\""
