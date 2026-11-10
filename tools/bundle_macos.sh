#!/bin/sh
# Assemble the macOS app bundle (CMake target nanpa2_app):
#   bundle_macos.sh OUT.app BINARY Info.plist
#
# Copies the binary and every dylib it needs outside /usr/lib and /System
# (Homebrew's libSDL3) into Contents/Frameworks, pointing the load commands at
# @executable_path/../Frameworks (the binary) and @loader_path (the dylibs),
# so the app runs from Finder without Homebrew's paths. The files are then
# signed ad hoc (install_name_tool breaks the signatures), and
# LSMinimumSystemVersion is set to the highest minos of the files.
#
# No game data goes in; the game folder is found at run time (src/launch.c). The binary holds nothing of the
# game (ctest no_game_bytes).
set -eu

app=$1
bin=$2
plist=$3
contents="$app/Contents"
fw="$contents/Frameworks"

rm -rf "$app"
mkdir -p "$contents/MacOS" "$fw" "$contents/Resources"
cp "$bin" "$contents/MacOS/nanpa2"
cp "$plist" "$contents/Info.plist"
printf 'APPL????' >"$contents/PkgInfo"

# non-system load commands of a Mach-O file
deps() {
	otool -L "$1" | tail -n +2 | awk '{print $1}' |
		grep -v -e '^/usr/lib/' -e '^/System/' -e '^@' || true
}

# fix FILE: copy what it needs into Frameworks, rewrite its load commands
# with PREFIX (@executable_path/../Frameworks or @loader_path)
fix() {
	file=$1
	prefix=$2
	for dep in $(deps "$file"); do
		name=$(basename "$dep")
		# a dylib's own id is listed too
		[ "$(otool -D "$file" | tail -n +2)" = "$dep" ] && continue
		if [ ! -f "$fw/$name" ]; then
			src=$dep
			[ -f "$src" ] || { echo "bundle: $dep (needed by $file) not found" >&2; exit 1; }
			cp -L "$src" "$fw/$name"
			chmod u+w "$fw/$name"
			install_name_tool -id "@executable_path/../Frameworks/$name" "$fw/$name" 2>/dev/null
			echo "$fw/$name" >>"$queue"
		fi
		install_name_tool -change "$dep" "$prefix/$name" "$file" 2>/dev/null
	done
}

queue=$(mktemp)
trap 'rm -f "$queue"' EXIT
fix "$contents/MacOS/nanpa2" "@executable_path/../Frameworks"

# dependencies of the copied dylibs, until none are new
done_n=0
while :; do
	total=$(wc -l <"$queue" | tr -d ' ')
	[ "$done_n" -lt "$total" ] || break
	done_n=$((done_n + 1))
	fix "$(sed -n "${done_n}p" "$queue")" "@loader_path"
done

# nothing may point outside the bundle and the system
for f in "$contents/MacOS/nanpa2" "$fw"/*.dylib; do
	left=$(deps "$f" | grep -v "^$(otool -D "$f" | tail -n +2)\$" || true)
	if [ -n "$left" ]; then
		echo "bundle: $f still needs $left" >&2
		exit 1
	fi
done

# LSMinimumSystemVersion: the highest minos (LC_BUILD_VERSION) or version
# (LC_VERSION_MIN_MACOSX) of the binaries
minos=$(for f in "$contents/MacOS/nanpa2" "$fw"/*.dylib; do
	otool -l "$f" | awk '/LC_BUILD_VERSION/{b=1} /LC_VERSION_MIN_MACOSX/{v=1}
		b && $1=="minos" {print $2; b=0} v && $1=="version" {print $2; v=0}'
done | sort -t. -k1,1n -k2,2n -k3,3n | tail -n 1)
[ -n "$minos" ] && plutil -replace LSMinimumSystemVersion -string "$minos" "$contents/Info.plist"

for f in "$fw"/*.dylib; do
	codesign --force --sign - "$f" 2>/dev/null
done
codesign --force --sign - "$app"
echo "bundle: $app (macOS $minos or later)"
