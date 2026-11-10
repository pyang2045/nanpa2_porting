#!/bin/sh
# Build ios/third_party/SDL3.xcframework (gitignored), which ios/nanpa2.xcodeproj links: SDL3,
# static, for iOS devices (arm64) and the simulator (arm64 + x86_64).
#
#   tools/fetch_sdl.sh
#
# Run it once before opening the project (tools/build_ios.sh runs it when the framework is missing).
# The official release tarball is downloaded and checked against a pinned SHA-256, then built with
# CMake and Ninja (brew install cmake ninja) and packed by xcodebuild -create-xcframework. SDL's own
# Xcode project is not used: Xcode 26 compiles its Metal shader and then needs the separate
# Metal Toolchain download, which this game does not need. Nothing is installed, no sudo.
set -eu

# The one place the SDL version lives (tools/build_msys2.sh and the README refer here).
ver=${SDL3_VERSION:-3.4.18}
sha=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 # SDL3-3.4.18.tar.gz
[ "$ver" = 3.4.18 ] || sha=${SDL3_SHA256:?SDL3_VERSION other than 3.4.18 needs SDL3_SHA256}

here=$(cd "$(dirname "$0")/.." && pwd)
dest=$here/ios/third_party/SDL3.xcframework
if [ -f "$dest/.version" ] && [ "$(cat "$dest/.version")" = "$ver" ]; then
	echo "fetch_sdl: SDL3 $ver xcframework already built"
	exit 0
fi
for t in xcrun xcodebuild cmake ninja curl tar shasum; do
	command -v "$t" >/dev/null 2>&1 || { echo "fetch_sdl: $t not found (install Xcode, then: brew install cmake ninja)" >&2; exit 1; }
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "fetch_sdl: downloading SDL3 $ver"
curl -fL -s -o "$tmp/sdl.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$ver/SDL3-$ver.tar.gz"
got=$(shasum -a 256 "$tmp/sdl.tar.gz" | cut -d' ' -f1)
if [ "$got" != "$sha" ]; then
	echo "fetch_sdl: SHA-256 mismatch for SDL3-$ver.tar.gz: got $got, expected $sha" >&2
	exit 1
fi
mkdir "$tmp/src"
tar -xzf "$tmp/sdl.tar.gz" -C "$tmp/src" --strip-components=1

# one slice per SDK: a static library and the headers
args=""
for sdk in iphoneos iphonesimulator; do
	archs="arm64"
	[ "$sdk" = iphonesimulator ] && archs="arm64;x86_64"
	sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)
	b=$tmp/build-$sdk
	i=$tmp/install-$sdk
	# no build paths in __FILE__ of the library
	cmake -S "$tmp/src" -B "$b" -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$sysroot" \
		-DCMAKE_OSX_ARCHITECTURES="$archs" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_FLAGS="-ffile-prefix-map=$tmp=sdl" -DCMAKE_OBJC_FLAGS="-ffile-prefix-map=$tmp=sdl" \
		-DCMAKE_INSTALL_PREFIX="$i" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_CAMERA=OFF >/dev/null
	cmake --build "$b" >/dev/null
	cmake --install "$b" >/dev/null
	args="$args -library $i/lib/libSDL3.a -headers $i/include" # include/SDL3/*.h: #include <SDL3/SDL.h>
done

rm -rf "$dest"
mkdir -p "$(dirname "$dest")"
# shellcheck disable=SC2086
xcodebuild -create-xcframework $args -output "$dest" >/dev/null
echo "$ver" >"$dest/.version"
echo "fetch_sdl: ios/third_party/SDL3.xcframework ready"
