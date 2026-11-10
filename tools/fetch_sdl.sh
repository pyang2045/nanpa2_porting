#!/bin/sh
# Build ios/third_party/SDL2.xcframework (gitignored), which ios/nanpa2.xcodeproj links: SDL2 and
# SDL2main, static, for iOS devices (arm64) and the simulator (arm64 + x86_64).
#
#   tools/fetch_sdl.sh
#
# Run it once before opening the project (tools/build_ios.sh runs it when the framework is missing).
# The official release tarball is downloaded and checked against a pinned SHA-256, then built with
# CMake and Ninja (brew install cmake ninja) and packed by xcodebuild -create-xcframework. SDL's own
# Xcode project is not used: Xcode 26 compiles its Metal shader and then needs the separate
# Metal Toolchain download, which this game does not need. Nothing is installed, no sudo.
set -eu

ver=${SDL2_VERSION:-2.32.10}
sha=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165 # SDL2-2.32.10.tar.gz
[ "$ver" = 2.32.10 ] || sha=${SDL2_SHA256:?SDL2_VERSION other than 2.32.10 needs SDL2_SHA256}

here=$(cd "$(dirname "$0")/.." && pwd)
dest=$here/ios/third_party/SDL2.xcframework
if [ -f "$dest/.version" ] && [ "$(cat "$dest/.version")" = "$ver" ]; then
	echo "fetch_sdl: SDL2 $ver xcframework already built"
	exit 0
fi
for t in xcrun xcodebuild cmake ninja curl tar shasum libtool; do
	command -v "$t" >/dev/null 2>&1 || { echo "fetch_sdl: $t not found (install Xcode, then: brew install cmake ninja)" >&2; exit 1; }
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "fetch_sdl: downloading SDL2 $ver"
curl -fL -s -o "$tmp/sdl.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$ver/SDL2-$ver.tar.gz"
got=$(shasum -a 256 "$tmp/sdl.tar.gz" | cut -d' ' -f1)
if [ "$got" != "$sha" ]; then
	echo "fetch_sdl: SHA-256 mismatch for SDL2-$ver.tar.gz: got $got, expected $sha" >&2
	exit 1
fi
mkdir "$tmp/src"
tar -xzf "$tmp/sdl.tar.gz" -C "$tmp/src" --strip-components=1

# one slice per SDK: a static library (SDL2 + SDL2main merged) and the headers
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
		-DCMAKE_INSTALL_PREFIX="$i" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF >/dev/null
	cmake --build "$b" >/dev/null
	cmake --install "$b" >/dev/null
	libtool -static -o "$i/lib/libSDL2all.a" "$i/lib/libSDL2.a" "$i/lib/libSDL2main.a" 2>/dev/null
	args="$args -library $i/lib/libSDL2all.a -headers $i/include/SDL2"
done

rm -rf "$dest"
mkdir -p "$(dirname "$dest")"
# shellcheck disable=SC2086
xcodebuild -create-xcframework $args -output "$dest" >/dev/null
echo "$ver" >"$dest/.version"
echo "fetch_sdl: ios/third_party/SDL2.xcframework ready"
