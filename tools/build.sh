#!/bin/sh
# Build the port on macOS or Linux (for Windows see tools/build_msys2.sh).
#
#   tools/build.sh [--backend interp|recomp] [--exe PATH] [--game-dir DIR]
#                  [--build-dir DIR] [--release | --debug] [--test] [--app]
#
#   --backend B      passed on as -DNANPA2_BACKEND=B. recomp lifts the game
#                    code from your nanpa2.exe at build time (tools/recomp.py)
#                    and needs --exe and Python with capstone; interp needs
#                    neither. Without --backend nothing is passed and the
#                    recomp checks run (the build as it is today).
#   --exe PATH       recomp: your nanpa2.exe (default: game/nanpa2.exe); it
#                    is read, never run.
#   --game-dir DIR   game folder for the tests (default: the folder of the
#                    exe, or game/ with --backend interp)
#   --build-dir DIR  build folder (default: build/)
#   --release        CMAKE_BUILD_TYPE=Release (default)
#   --debug          CMAKE_BUILD_TYPE=Debug
#   --test           run ctest afterwards (the smoke test opens a window for
#                    about a minute; "Skipped" means passed, screen not seen)
#   --app            macOS only: also build the app bundle (target nanpa2_app,
#                    BUILD_DIR/同級生２.app; for this machine only, never share it)
#
# Needs a C compiler, CMake, Ninja, SDL2 and Python 3. Missing tools are
# named with the install command for this system; nothing is installed with
# sudo. Python's capstone goes into tools/.venv (created on first run, reused
# after). Re-running reuses the venv and the build folder.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
backend=
exe=$here/game/nanpa2.exe
game=
dir=$here/build
type=Release
test=0
app=0

usage() { sed -n '2,27p' "$0" >&2; exit 2; }
while [ $# -gt 0 ]; do
	case $1 in
	--backend) [ $# -ge 2 ] || usage; backend=$2; shift ;;
	--exe) [ $# -ge 2 ] || usage; exe=$2; shift ;;
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

case $backend in
'' | recomp | interp) ;;
*) echo "build.sh: --backend is interp or recomp" >&2; exit 2 ;;
esac
recomp=1
[ "$backend" = interp ] && recomp=0

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
		echo "  brew install sdl2 cmake ninja python   (and xcode-select --install for the compiler)"
	else
		echo "  Debian/Ubuntu: sudo apt install build-essential cmake ninja-build pkg-config libsdl2-dev python3 python3-venv"
		echo "  Fedora:        sudo dnf install gcc cmake ninja-build pkgconf-pkg-config SDL2-devel python3"
		echo "  Arch:          sudo pacman -S --needed base-devel cmake ninja sdl2 python"
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
if pkg-config --exists sdl2 2>/dev/null || sdl2-config --version >/dev/null 2>&1; then
	sdl=1
elif [ "$os" = Darwin ] && command -v brew >/dev/null 2>&1 && brew list --versions sdl2 >/dev/null 2>&1; then
	sdl=1
fi
if [ "$sdl" = 0 ]; then
	# CMake may still find it (SDL2_DIR, CMAKE_PREFIX_PATH); its error follows if not
	echo "build.sh: SDL2 development files not found by pkg-config or sdl2-config. Install with:" >&2
	hint >&2
fi

# --- recomp only: the exe the game code is lifted from, and Python with
# capstone (tools/recomp.py)
if [ "$recomp" = 1 ]; then
	if [ ! -f "$exe" ]; then
		echo "build.sh: $exe not found." >&2
		echo "  Copy your 同級生２ CD into game/ and extract nanpa2.exe and FLAG0..FLAG6 from" >&2
		echo "  data1.cab there (unshield x data1.cab; see game/README.md), or pass --exe PATH." >&2
		exit 1
	fi
	exe=$(cd "$(dirname "$exe")" && pwd)/$(basename "$exe")
	[ -n "$game" ] || game=$(dirname "$exe")

	# --- Python with capstone (tools/recomp.py)
	venv=$here/tools/.venv
	py=$venv/bin/python
	capstone='capstone>=5.0.1,<6'
	if ! "$py" -c 'import capstone' >/dev/null 2>&1; then
		echo "build.sh: setting up $venv with $capstone"
		if [ ! -x "$py" ]; then
			python3 -m venv "$venv" || {
				echo "build.sh: python3 -m venv failed (Debian/Ubuntu: sudo apt install python3-venv)" >&2
				exit 1
			}
		fi
		"$py" -m pip install --quiet --disable-pip-version-check "$capstone" || {
			echo "build.sh: pip could not install $capstone into $venv" >&2
			exit 1
		}
	fi
	echo "build.sh: Python $("$py" -c 'import sys, capstone; print(sys.version.split()[0], "capstone", capstone.__version__)')"
else
	[ -n "$game" ] || game=$here/game
fi
[ -d "$game" ] || { echo "build.sh: game folder $game not found" >&2; exit 1; }
game=$(cd "$game" && pwd)

# --- configure and build (both no-ops when nothing changed). The cache
# variables are passed every time so a reused build folder never keeps stale
# values.
set -- -DCMAKE_BUILD_TYPE="$type" -DNANPA2_GAME_DIR="$game"
[ -z "$backend" ] || set -- "$@" -DNANPA2_BACKEND="$backend"
[ "$recomp" = 0 ] || set -- "$@" -DNANPA2_EXE="$exe" -DNANPA2_PYTHON="$py"
cmake -S "$here" -B "$dir" -G Ninja "$@"
ninja -C "$dir"
if [ "$app" = 1 ]; then
	ninja -C "$dir" nanpa2_app
fi
if [ "$test" = 1 ]; then
	ctest --test-dir "$dir" --output-on-failure
fi

echo "build.sh: built $dir/nanpa2 ($type)"
[ "$app" = 1 ] && echo "build.sh: app $dir/同級生２.app (this machine only, never share it)"
echo "build.sh: play with: $dir/nanpa2 --game \"$game\""
