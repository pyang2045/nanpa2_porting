#!/bin/sh
# Build the iPad / iPhone app (iOS 13+, universal, landscape only).
#
#   tools/build_ios.sh [--device] [--build-dir DIR] [--debug]
#
# Default: the iOS Simulator (the host architecture), ad-hoc signed: build-ios/sim/nanpa2.app (display name 同級生２).
#   xcrun simctl boot "iPad Pro 11-inch (M4)"      # once
#   xcrun simctl install booted build-ios/sim/nanpa2.app
#   xcrun simctl launch booted local.nanpa2
#
# --device builds for a real iPad with the Xcode generator and signs with your team:
#   DEVELOPMENT_TEAM=ABCDE12345 tools/build_ios.sh --device     (team ID: developer.apple.com > Membership)
# then build-ios/device/Release-iphoneos/nanpa2.app is installed with Xcode's Devices window, or
#   xcrun devicectl device install app --device <name> build-ios/device/Release-iphoneos/nanpa2.app
# A free personal team works; set NANPA2_APP_ID to a bundle identifier unique to you (default
# local.nanpa2). Needs a connected, trusted device and Developer Mode on it (untested here).
#
# SDL2 is not bundled: the official release source is downloaded once into build-ios/ (gitignored)
# and built as a static library for the same SDK. Needs Xcode, CMake and Ninja (no sudo, nothing
# installed). The app holds none of the game: copy your game folder into the app's Documents
# (README "iPad"). Re-running reuses the build folder.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
root=$here/build-ios
device=0
type=Release
sdl_ver=${SDL2_VERSION:-2.32.10}

while [ $# -gt 0 ]; do
	case $1 in
	--device) device=1 ;;
	--build-dir) root=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); shift ;;
	--debug) type=Debug ;;
	-h | --help) sed -n '2,22p' "$0"; exit 0 ;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done

for t in xcrun cmake ninja curl tar; do
	command -v "$t" >/dev/null 2>&1 || { echo "build_ios: $t not found (install Xcode, then: brew install cmake ninja)" >&2; exit 1; }
done

if [ "$device" = 1 ]; then
	sdk=iphoneos
	out=$root/device
	arch=arm64
else
	sdk=iphonesimulator
	out=$root/sim
	arch=$(uname -m) # the simulator runs the host's architecture
	[ "$arch" = arm64 ] || arch=x86_64
fi
sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)
common="-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sysroot -DCMAKE_OSX_ARCHITECTURES=$arch -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0"

# ---- SDL2: official release source, static library for this SDK ----
src=$root/SDL2-$sdl_ver
sdl=$out/sdl-install
if [ ! -f "$src/CMakeLists.txt" ]; then
	mkdir -p "$root"
	echo "build_ios: downloading SDL2 $sdl_ver"
	curl -fL -o "$root/SDL2-$sdl_ver.tar.gz" \
		"https://github.com/libsdl-org/SDL/releases/download/release-$sdl_ver/SDL2-$sdl_ver.tar.gz"
	tar -xzf "$root/SDL2-$sdl_ver.tar.gz" -C "$root"
fi
if [ ! -f "$sdl/lib/libSDL2.a" ]; then
	# shellcheck disable=SC2086
	cmake -S "$src" -B "$out/sdl-build" -G Ninja $common -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$sdl" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF
	cmake --build "$out/sdl-build"
	cmake --install "$out/sdl-build"
fi

# ---- the game ----
if [ "$device" = 1 ]; then
	: "${DEVELOPMENT_TEAM:?set DEVELOPMENT_TEAM to your Apple team ID}"
	# shellcheck disable=SC2086
	cmake -S "$here" -B "$out/build" -G Xcode $common -DSDL2_DIR="$sdl/lib/cmake/SDL2" \
		-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="$DEVELOPMENT_TEAM" \
		-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic \
		${NANPA2_APP_ID:+-DNANPA2_APP_ID="$NANPA2_APP_ID"}
	cmake --build "$out/build" --config "$type" -- -allowProvisioningUpdates
	echo "build_ios: $out/build/$type-iphoneos/nanpa2.app"
else
	# shellcheck disable=SC2086
	cmake -S "$here" -B "$out/build" -G Ninja $common -DCMAKE_BUILD_TYPE="$type" \
		-DSDL2_DIR="$sdl/lib/cmake/SDL2" ${NANPA2_APP_ID:+-DNANPA2_APP_ID="$NANPA2_APP_ID"}
	cmake --build "$out/build" --target nanpa2
	rm -rf "$out/nanpa2.app"
	cp -R "$out/build/nanpa2.app" "$out/nanpa2.app"
	codesign --force --sign - "$out/nanpa2.app"
	echo "build_ios: $out/nanpa2.app (simulator, $arch, ad-hoc signed)"
fi
