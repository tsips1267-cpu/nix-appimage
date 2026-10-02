#!/bin/sh
set -eu

program=$1
mkdir extras

# get the derivation's root directory

IFS=/ read -r _ nix store hash rest <<EOF
$program
EOF

if [ "$nix" != nix ] || [ "$store" != store ]; then
	echo "entrypoint '${program}' is not in the nix store" >&2
	exit 1
fi

drv="/nix/store/$hash"

# prints the values of a key in the main group of a .desktop file (and not e.g.
# in [Desktop Action ...] groups)
desktop_key() {
	sed -ne "/^\[Desktop Entry\]/,/^\[/{ s/^$1 *= *//p }" "$2"
}

# find desktop file

desktop=

# try find one whose Exec= matches $program
for d in "$drv/share/applications/"*.desktop; do
	if ! [ -e "$d" ]; then
		# no .desktop files, or a dangling symlink
		continue
	fi

	# the basename of the executable, which may be quoted
	if desktop_key Exec "$d" | sed -e 's/^"\([^"]*\)".*/\1/; t done' -e 's/ .*//' -e ':done' -e 's#.*/##' |
		grep --fixed-strings --line-regexp --quiet "$(basename "$program")"
	then
		if [ -z "$desktop" ]; then
			desktop=$d
		else
			echo "multiple .desktop entries found; giving up" >&2
			exit
		fi
	fi
done

if [ -z "$desktop" ]; then
	echo "no .desktop found; giving up" >&2
	exit
fi

# copy desktop file and icons

cp -L --no-preserve=all "$desktop" extras/
if [ -e "$drv/share/icons/hicolor" ]; then
	mkdir -p extras/usr/share/icons
	cp -Lr --no-preserve=all "$drv/share/icons/hicolor" extras/usr/share/icons ||
		echo "couldn't copy some icons; continuing anyway" >&2
fi

# create DirIcon, and an icon named after Icon= next to the .desktop file

icon=$(desktop_key Icon "$desktop" | head -n 1)
iconfile=
case "$icon" in
"") ;;
/*)
	# an absolute path, which tools like appimaged and AppImageLauncher don't
	# support, so we use a name instead
	if [ -f "$icon" ]; then
		iconfile=$icon
		icon=$(basename "$icon")
		icon=${icon%.*}
		sed -i -e "/^\[Desktop Entry\]/,/^\[/s|^Icon *=.*|Icon=$icon|" "extras/$(basename "$desktop")"
	fi
	;;
*)
	for dir in 256x256 512x512 1024x1024 192x192 128x128 96x96 64x64 48x48 scalable; do
		for ext in png svg xpm; do
			if [ -f "$drv/share/icons/hicolor/$dir/apps/$icon.$ext" ]; then
				iconfile="$drv/share/icons/hicolor/$dir/apps/$icon.$ext"
				break 2
			fi
		done
	done
	for ext in png svg xpm; do
		if [ -z "$iconfile" ] && [ -f "$drv/share/pixmaps/$icon.$ext" ]; then
			iconfile="$drv/share/pixmaps/$icon.$ext"
		fi
	done
	;;
esac

if [ -n "$iconfile" ]; then
	# .DirIcon SHOULD be a 256x256 PNG, and the app could have non-PNG
	# icons, so ideally we'd use a tool like imagemagick to create it. But that's a big dependency, and this is close enough?
	cp -L --no-preserve=all "$iconfile" extras/.DirIcon
	case "$iconfile" in
	*.png | *.svg | *.xpm) cp -L --no-preserve=all "$iconfile" "extras/$icon.${iconfile##*.}" ;;
	esac
fi
