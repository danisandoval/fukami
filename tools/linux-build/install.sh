#!/bin/sh
# Install Fukami for the current user (Steam Deck / Linux x86-64) and add it to Steam.
#
# Run it from the extracted Fukami folder:   ./install.sh
# It copies the app to ~/Applications/Fukami (override with FUKAMI_INSTALL_DIR), adds an application-menu
# entry and, on SteamOS, adds Fukami to Steam as a non-Steam game. It never touches your game data: put your
# Ridge Racer V (USA) .chd in ~/.local/share/Fukami/ or choose it on first launch.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
dest=${FUKAMI_INSTALL_DIR:-$HOME/Applications/Fukami}
data=${XDG_DATA_HOME:-$HOME/.local/share}
apps="$data/applications"

[ -x "$here/bin/Fukami" ] || { echo "install.sh: run it from the extracted Fukami folder (bin/Fukami not found)" >&2; exit 1; }

if [ "$here" != "$dest" ]; then
    mkdir -p "$dest"
    cp -a "$here"/. "$dest"/
fi
mkdir -p "$apps" "$data/Fukami"

cat > "$apps/fukami.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Fukami
Comment=Ridge Racer V (USA), native port
Exec="$dest/Fukami"
Icon=$dest/share/fukami.png
Terminal=false
Categories=Game;
DESKTOP
chmod 644 "$apps/fukami.desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" >/dev/null 2>&1 || true

echo "Fukami installed in $dest"
if command -v steamos-add-to-steam >/dev/null 2>&1; then
    steamos-add-to-steam "$apps/fukami.desktop" && echo "Added to Steam: restart Steam or switch to Game Mode to see it." \
        || echo "Could not add it to Steam automatically: in Steam choose Games > Add a Non-Steam Game and pick Fukami."
else
    echo "To play from Steam: Games > Add a Non-Steam Game to My Library > Fukami."
fi
echo "Put your Ridge Racer V (USA) .chd in $data/Fukami/ (or pick it on the first launch)."
