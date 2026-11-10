#!/bin/sh
# Build the iPad / iPhone app (iOS 13+, universal, landscape only): a thin wrapper over xcodebuild on
# ios/nanpa2.xcodeproj (open that in Xcode for the normal workflow; README "iPad / iPhone").
#
#   tools/build_ios.sh [--device | --ipa] [--build-dir DIR] [--debug]
#
# Default: the iOS Simulator (the host architecture), ad-hoc signed: build-ios/sim/nanpa2.app (display name 同級生２).
#   xcrun simctl boot "iPad Pro 11-inch (M4)"      # once
#   xcrun simctl install booted build-ios/sim/nanpa2.app
#   xcrun simctl launch booted local.nanpa2
#
# --device builds for a real iPad and signs with your team (Xcode's automatic signing):
#   DEVELOPMENT_TEAM=ABCDE12345 tools/build_ios.sh --device     (team ID: developer.apple.com > Membership)
# then build-ios/device/Release-iphoneos/nanpa2.app is installed with Xcode's Devices window, or
#   xcrun devicectl device install app --device <name> build-ios/device/Release-iphoneos/nanpa2.app
# A free personal team works; set NANPA2_APP_ID to a bundle identifier unique to you (default
# local.nanpa2). Needs a connected, trusted device and Developer Mode on it (untested here).
#
# --ipa builds an UNSIGNED device app (arm64, no team, no profile) and packs build-ios/nanpa2.ipa
# for sideloading tools that re-sign it with your own Apple ID (Sideloadly, AltStore/SideStore, or
# Xcode's Devices window > + for a personal team): README "Sideloading". No Apple account needed here.
#
# SDL3 is not bundled: tools/fetch_sdl.sh (run here if needed) puts the official release source in
# ios/third_party/SDL3, which the project builds as a subproject. Needs Xcode only. The app holds none
# of the game: copy your game folder into the app's Documents (README "iPad").
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
root=$here/build-ios
device=0
ipa=0
config=Release

while [ $# -gt 0 ]; do
	case $1 in
	--device) device=1 ;;
	--ipa) device=1; ipa=1 ;;
	--build-dir) mkdir -p "$2"; root=$(cd "$2" && pwd); shift ;;
	--debug) config=Debug ;;
	-h | --help) sed -n '2,25p' "$0"; exit 0 ;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done

for t in xcodebuild xcrun; do
	command -v "$t" >/dev/null 2>&1 || { echo "build_ios: $t not found (install Xcode)" >&2; exit 1; }
done
[ -d "$here/ios/third_party/SDL3.xcframework" ] || "$here/tools/fetch_sdl.sh"

if [ "$device" = 1 ]; then
	sdk=iphoneos
	out=$root/device
	[ "$ipa" = 1 ] && out=$root/ipa
	arch=arm64
	dest="generic/platform=iOS"
else
	sdk=iphonesimulator
	out=$root/sim
	arch=$(uname -m) # the simulator runs the host's architecture
	[ "$arch" = arm64 ] || [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ] && arch=arm64 || arch=x86_64
	dest="generic/platform=iOS Simulator"
fi

# No build paths in the shipped binary: the game's own flags map the source tree (see the project);
# (the project already does it for its own flags; this adds the bare repo root).
map="-ffile-prefix-map=$here=."

set -- -project "$here/ios/nanpa2.xcodeproj" -scheme nanpa2 -configuration "$config" -sdk "$sdk" \
	-destination "$dest" SYMROOT="$out" OBJROOT="$out/obj" ARCHS="$arch" ONLY_ACTIVE_ARCH=NO \
	"OTHER_CFLAGS=\$(inherited) $map"
[ -z "${NANPA2_APP_ID:-}" ] || set -- "$@" PRODUCT_BUNDLE_IDENTIFIER="$NANPA2_APP_ID"

if [ "$ipa" = 1 ]; then
	# no signing at all; any signature is removed below
	xcodebuild "$@" CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= build
	app=$out/nanpa2.app
	rm -rf "$app" "$out/Payload"
	cp -R "$out/$config-iphoneos/nanpa2.app" "$app"
	rm -rf "$app/_CodeSignature" "$app/embedded.mobileprovision"
	codesign --remove-signature "$app/nanpa2" 2>/dev/null || true # the linker's ad-hoc signature
	strip -x "$app/nanpa2"
	mkdir "$out/Payload"
	cp -R "$app" "$out/Payload/"
	rm -f "$root/nanpa2.ipa"
	(cd "$out" && ditto -c -k --norsrc --noextattr --noqtn --keepParent Payload "$root/nanpa2.ipa")
	echo "build_ios: $root/nanpa2.ipa (unsigned, arm64; re-sign by sideloading)"
elif [ "$device" = 1 ]; then
	: "${DEVELOPMENT_TEAM:?set DEVELOPMENT_TEAM to your Apple team ID}"
	xcodebuild "$@" DEVELOPMENT_TEAM="$DEVELOPMENT_TEAM" -allowProvisioningUpdates build
	echo "build_ios: $out/$config-iphoneos/nanpa2.app"
else
	xcodebuild "$@" CODE_SIGNING_ALLOWED=NO build
	rm -rf "$out/nanpa2.app"
	cp -R "$out/$config-iphonesimulator/nanpa2.app" "$out/nanpa2.app"
	codesign --force --sign - "$out/nanpa2.app"
	echo "build_ios: $out/nanpa2.app (simulator, $arch, ad-hoc signed)"
fi
