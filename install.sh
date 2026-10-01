#!/bin/bash
# frame-autopass installer and updater for the Steam Frame.
#
#   curl -fsSL https://github.com/bod09/frame-autopass/releases/latest/download/install.sh | bash
#
# or, from an unpacked release folder: ./install.sh
#
# Installs two programs into ~/.local/share/frame-autopass and a systemd
# user unit that starts them with SteamVR. Nothing outside your home
# directory is touched, so it survives SteamOS updates. Running it again
# updates to the latest release. Remove with `autopass uninstall`.
set -euo pipefail

REPO="bod09/frame-autopass"
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/frame-autopass"
TARBALL="frame-autopass-aarch64.tar.gz"

say() { printf '%s\n' "$*"; }
fail() { printf 'frame-autopass: %s\n' "$*" >&2; exit 1; }

[ "$(uname -m)" = "aarch64" ] || fail "this is for the Steam Frame (aarch64); this machine is $(uname -m)."
command -v systemctl >/dev/null || fail "systemd not found."

# Desktop Mode on the Frame runs Konsole in a nested session with its own
# XDG_RUNTIME_DIR and D-Bus, where `systemctl --user` cannot reach the real
# user service manager. Always talk to the login session's one.
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"

if ! grep -qs arcimx616 /sys/class/video4linux/*/name; then
    fail "the Arcturus Vision colour module was not found. frame-autopass switches between that module
and the built-in IR cameras, so it needs the module attached."
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Use the files next to this script when run from an unpacked release,
# otherwise download the latest release and check its checksum.
here="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)"
if [ -n "$here" ] && [ -x "$here/autopassd" ] && [ -x "$here/autopass" ]; then
    src="$here"
else
    base="https://github.com/$REPO/releases/latest/download"
    say "Downloading the latest frame-autopass release..."
    curl -fsSL -o "$work/$TARBALL" "$base/$TARBALL" || fail "download failed ($base/$TARBALL)."
    curl -fsSL -o "$work/$TARBALL.sha256" "$base/$TARBALL.sha256" || fail "checksum download failed."
    (cd "$work" && sha256sum -c --quiet "$TARBALL.sha256") || fail "checksum mismatch; not installing."
    tar -C "$work" -xzf "$work/$TARBALL"
    src="$work/frame-autopass"
fi

new_version="$("$src/autopass" version 2>/dev/null || echo unknown)"
old_version="$("$DEST/autopass" version 2>/dev/null || echo none)"

# Replace the programs while the service is stopped, then (re)install the
# unit, which starts it again if SteamVR is running.
systemctl --user stop frame-autopass.service 2>/dev/null || true
mkdir -p "$DEST"
for f in autopassd autopass; do
    install -m 0755 "$src/$f" "$DEST/$f.new"
    mv -f "$DEST/$f.new" "$DEST/$f"
done
"$DEST/autopass" install

if [ "$old_version" = "none" ]; then
    say "frame-autopass $new_version installed. It runs whenever SteamVR runs; nothing else to do."
else
    say "frame-autopass updated: $old_version -> $new_version."
fi
say "Check it any time with: $DEST/autopass status"
