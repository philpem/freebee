#!/bin/sh

set -eu

usage() {
	cat <<'EOF'
Usage: scripts/create-macos-app.sh [options]

Create a macOS FreeBee.app wrapper around the FreeBee executable.

Options:
  --executable PATH  Executable to bundle (default: project-root/freebee)
  --data-dir PATH    Directory containing .freebee.toml, ROMs, and disks
                     (default: current directory)
  --disk-image PATH  Default writable hard-disk seed (.img or .zip)
                     (default: project-root/3b1-hd.zip, then hd.img)
  --no-disk-image    Build without a bundled hard-disk seed
  --rom-dir PATH     Directory containing 14c.bin and 15c.bin
                     (default: project-root/roms)
  --no-roms          Build without bundled ROM files
  --output PATH      App bundle to create (default: project-root/FreeBee.app)
  --force            Compatibility option; existing bundles are replaced
  --no-sign          Do not ad-hoc sign the finished bundle
  -h, --help         Show this help
EOF
}

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(dirname -- "$script_dir")
executable="$project_dir/freebee"
data_dir=$(pwd)
output="$project_dir/FreeBee.app"
rom_dir="$project_dir/roms"
if [ -f "$project_dir/3b1-hd.zip" ]; then
	disk_image="$project_dir/3b1-hd.zip"
else
	disk_image="$project_dir/hd.img"
fi
force=0
sign=1

while [ "$#" -gt 0 ]; do
	case "$1" in
		--executable)
			[ "$#" -ge 2 ] || { echo "Missing value for --executable" >&2; exit 2; }
			executable=$2
			shift 2
			;;
		--data-dir)
			[ "$#" -ge 2 ] || { echo "Missing value for --data-dir" >&2; exit 2; }
			data_dir=$2
			shift 2
			;;
		--output)
			[ "$#" -ge 2 ] || { echo "Missing value for --output" >&2; exit 2; }
			output=$2
			shift 2
			;;
		--disk-image)
			[ "$#" -ge 2 ] || { echo "Missing value for --disk-image" >&2; exit 2; }
			disk_image=$2
			shift 2
			;;
		--no-disk-image)
			disk_image=
			shift
			;;
		--rom-dir)
			[ "$#" -ge 2 ] || { echo "Missing value for --rom-dir" >&2; exit 2; }
			rom_dir=$2
			shift 2
			;;
		--no-roms)
			rom_dir=
			shift
			;;
		--force)
			force=1
			shift
			;;
		--no-sign)
			sign=0
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Unknown option: $1" >&2
			usage >&2
			exit 2
			;;
	esac
done

[ "$(uname -s)" = "Darwin" ] || { echo "This script must be run on macOS." >&2; exit 1; }
[ -f "$executable" ] && [ -x "$executable" ] || {
	echo "FreeBee executable not found or not executable: $executable" >&2
	echo "Build it first with 'make'." >&2
	exit 1
}
[ -d "$data_dir" ] || { echo "Data directory not found: $data_dir" >&2; exit 1; }
if [ -n "$disk_image" ] && [ ! -f "$disk_image" ]; then
	echo "Hard-disk seed not found: $disk_image" >&2
	exit 1
fi
if [ -n "$rom_dir" ] && { [ ! -f "$rom_dir/14c.bin" ] || [ ! -f "$rom_dir/15c.bin" ]; }; then
	echo "ROM directory must contain 14c.bin and 15c.bin: $rom_dir" >&2
	exit 1
fi

executable=$(CDPATH= cd -- "$(dirname -- "$executable")" && printf '%s/%s\n' "$(pwd)" "$(basename -- "$executable")")
data_dir=$(CDPATH= cd -- "$data_dir" && pwd)
output_parent=$(dirname -- "$output")
mkdir -p "$output_parent"
output_parent=$(CDPATH= cd -- "$output_parent" && pwd)
output="$output_parent/$(basename -- "$output")"

staging=$(mktemp -d "${TMPDIR:-/tmp}/freebee-app.XXXXXX")
trap 'rm -rf -- "$staging"' EXIT HUP INT TERM
app="$staging/FreeBee.app"
contents="$app/Contents"
mkdir -p "$contents/MacOS" "$contents/Resources" "$contents/Frameworks"

cp "$executable" "$contents/Resources/freebee"
chmod 755 "$contents/Resources/freebee"
cp "$project_dir/assets/macos/freebee.icns" "$contents/Resources/freebee.icns"
printf '%s\n' "$data_dir" > "$contents/Resources/data-directory"
if [ -n "$rom_dir" ]; then
	mkdir -p "$contents/Resources/roms"
	cp "$rom_dir/14c.bin" "$contents/Resources/roms/14c.bin"
	cp "$rom_dir/15c.bin" "$contents/Resources/roms/15c.bin"
fi
if [ -n "$disk_image" ]; then
	case "$disk_image" in
		*.zip) cp "$disk_image" "$contents/Resources/default-hd.zip" ;;
		*) cp "$disk_image" "$contents/Resources/default-hd.img" ;;
	esac
fi

# Bundle SDL and replace its Homebrew/MacPorts load path. System libraries and
# frameworks remain provided by macOS.
sdl_library=$(otool -L "$executable" | awk '/libSDL2.*dylib/ { print $1; exit }')
sdl3_library=
if [ -n "$sdl_library" ]; then
	[ -f "$sdl_library" ] || { echo "Linked SDL library not found: $sdl_library" >&2; exit 1; }
	sdl_name=$(basename -- "$sdl_library")
	cp -L "$sdl_library" "$contents/Frameworks/$sdl_name"
	install_name_tool -id "@rpath/$sdl_name" "$contents/Frameworks/$sdl_name"
	install_name_tool -change "$sdl_library" "@executable_path/../Frameworks/$sdl_name" "$contents/Resources/freebee"

	# Homebrew may provide SDL2 through sdl2-compat. That shim loads SDL3 with
	# dlopen() rather than declaring it in the Mach-O dependency list, so otool
	# alone cannot discover it. Place the expected name beside the shim.
	if strings "$sdl_library" | grep -q '@loader_path/libSDL3.dylib'; then
		sdl3_libdir=$(pkg-config --variable=libdir sdl3 2>/dev/null || true)
		if [ -n "$sdl3_libdir" ] && [ -f "$sdl3_libdir/libSDL3.0.dylib" ]; then
			sdl3_library="$sdl3_libdir/libSDL3.0.dylib"
		elif [ -x /opt/homebrew/bin/brew ]; then
			sdl3_prefix=$(/opt/homebrew/bin/brew --prefix sdl3 2>/dev/null || true)
			if [ -n "$sdl3_prefix" ] && [ -f "$sdl3_prefix/lib/libSDL3.0.dylib" ]; then
				sdl3_library="$sdl3_prefix/lib/libSDL3.0.dylib"
			fi
		fi
		[ -n "$sdl3_library" ] || {
			echo "SDL2 compatibility library requires SDL3, but libSDL3.0.dylib was not found" >&2
			exit 1
		}
		cp -L "$sdl3_library" "$contents/Frameworks/libSDL3.dylib"
	fi
fi

minimum_os=$(otool -l "$executable" | awk '$1 == "minos" { print $2; exit }')
minimum_os=${minimum_os:-10.13}
if [ -n "$sdl_library" ]; then
	sdl_minimum_os=$(otool -l "$sdl_library" | awk '$1 == "minos" { print $2; exit }')
	if [ -n "$sdl_minimum_os" ]; then
		minimum_os=$(awk -v app="$minimum_os" -v sdl="$sdl_minimum_os" 'BEGIN { print (app + 0 >= sdl + 0) ? app : sdl }')
	fi
fi
if [ -n "$sdl3_library" ]; then
	sdl3_minimum_os=$(otool -l "$sdl3_library" | awk '$1 == "minos" { print $2; exit }')
	if [ -n "$sdl3_minimum_os" ]; then
		minimum_os=$(awk -v app="$minimum_os" -v sdl="$sdl3_minimum_os" 'BEGIN { print (app + 0 >= sdl + 0) ? app : sdl }')
	fi
fi

cat > "$contents/MacOS/FreeBee" <<'EOF'
#!/bin/sh
set -eu
contents=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
data_dir=$(sed -n '1p' "$contents/Resources/data-directory")
support_dir="$HOME/Library/Application Support/FreeBee"
working_disk="$support_dir/hd.img"
mkdir -p "$support_dir"

# Never run against the signed, read-only seed in the app bundle. Install a
# private writable copy once, using a temporary path so interruption cannot
# leave a partial root disk behind.
if [ ! -f "$working_disk" ]; then
	tmp_dir=$(mktemp -d "$support_dir/.install.XXXXXX")
	trap 'rm -rf -- "$tmp_dir"' EXIT HUP INT TERM
	if [ -f "$contents/Resources/default-hd.zip" ]; then
		/usr/bin/ditto -x -k "$contents/Resources/default-hd.zip" "$tmp_dir"
		seed=$(find "$tmp_dir" -type f -name '*.img' -print -quit)
		[ -n "$seed" ] || { echo "Bundled disk archive contains no .img file" >&2; exit 1; }
		mv "$seed" "$working_disk"
	elif [ -f "$contents/Resources/default-hd.img" ]; then
		cp "$contents/Resources/default-hd.img" "$tmp_dir/hd.img"
		mv "$tmp_dir/hd.img" "$working_disk"
	fi
	rm -rf -- "$tmp_dir"
	trap - EXIT HUP INT TERM
fi

if [ -f "$working_disk" ]; then
	export FREEBEE_DEFAULT_HD="$working_disk"
fi
if [ -f "$contents/Resources/roms/14c.bin" ] && [ -f "$contents/Resources/roms/15c.bin" ]; then
	export FREEBEE_ROM14C="$contents/Resources/roms/14c.bin"
	export FREEBEE_ROM15C="$contents/Resources/roms/15c.bin"
fi
if [ ! -d "$data_dir" ]; then
	data_dir="$support_dir"
fi
cd "$data_dir"
exec "$contents/Resources/freebee" "$@"
EOF
chmod 755 "$contents/MacOS/FreeBee"

cat > "$contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>en</string>
	<key>CFBundleDisplayName</key><string>FreeBee</string>
	<key>CFBundleExecutable</key><string>FreeBee</string>
	<key>CFBundleIconFile</key><string>freebee</string>
	<key>CFBundleIdentifier</key><string>org.philpem.freebee</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>FreeBee</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>0.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSMinimumSystemVersion</key><string>$minimum_os</string>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF

if command -v plutil >/dev/null 2>&1; then
	plutil -lint "$contents/Info.plist" >/dev/null
fi
if [ "$sign" -eq 1 ] && command -v codesign >/dev/null 2>&1; then
	codesign --force --deep --sign - "$app" >/dev/null
fi

# Replace an older generated app only after the new bundle has been fully
# assembled, validated, and signed. This keeps the previous app available if
# any earlier build step fails.
if [ -e "$output" ]; then
	echo "Removing old app bundle: $output"
	rm -rf -- "$output"
fi
mv "$app" "$output"
echo "Created $output"
echo "FreeBee data directory: $data_dir"
if [ -n "$disk_image" ]; then
	echo "Bundled hard-disk seed: $disk_image"
fi
if [ -n "$rom_dir" ]; then
	echo "Bundled ROMs: $rom_dir/14c.bin, $rom_dir/15c.bin"
fi
echo "Target architectures: $(lipo -archs "$executable")"
echo "Minimum macOS version: $minimum_os"
