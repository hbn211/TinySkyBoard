#!/bin/sh
# Installs or removes the TinySkyBoard top-bar extension for the current user (GNOME Shell 50).
#
#   ./install.sh [CODE]              install, or update; asks for the pairing code if not given
#   ./install.sh --uninstall         remove the extension (keeps the pairing code)
#   ./install.sh --uninstall --purge remove the extension and the pairing code
set -eu

UUID=tinyskyboard@hbn211.github.io
SRC=$(cd "$(dirname "$0")" && pwd)/$UUID
EXT_DIR=${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions
DEST=$EXT_DIR/$UUID
CONF_DIR=${XDG_CONFIG_HOME:-$HOME/.config}/tinyskyboard
CODE_FILE=$CONF_DIR/code

say() { printf '%s\n' "$*"; }
die() { printf 'Error: %s\n' "$*" >&2; exit 1; }

# the extension in the list of enabled ones (read at login); on = 1 adds it, 0 removes it
set_enabled_setting() {
    command -v gsettings >/dev/null || return 0
    cur=$(gsettings get org.gnome.shell enabled-extensions 2>/dev/null) || return 0
    rest=$(printf '%s' "$cur" | sed -e "s/^@as //" -e "s/[][]//g" -e "s/'$UUID'//" -e "s/, *,/,/g" \
        -e "s/^ *, *//" -e "s/ *, *$//")
    if [ "$1" = 1 ]; then
        new="[${rest:+$rest, }'$UUID']"
    else
        new="[$rest]"
    fi
    gsettings set org.gnome.shell enabled-extensions "$new"
}

# the setup page shows XXXX-XXXX with letters and digits, no 0/O/1/I/L
normalize_code() {
    c=$(printf '%s' "$1" | tr 'a-z' 'A-Z' | tr -cd '0-9A-Z')
    printf '%s' "$c" | grep -Eq '^[2-9A-HJKMNP-Z]{8}$' || return 1
    printf '%s-%s' "$(printf '%s' "$c" | cut -c1-4)" "$(printf '%s' "$c" | cut -c5-8)"
}

uninstall() {
    gnome-extensions disable "$UUID" 2>/dev/null || true
    set_enabled_setting 0
    if [ -L "$DEST" ]; then
        rm -- "$DEST"          # a link (a developer's install): only the link, never what it points to
    elif [ -d "$DEST" ]; then
        rm -r -- "$DEST"
    fi
    say "Removed the extension."
    if [ "${1:-}" = --purge ]; then
        rm -f -- "$CODE_FILE"
        rmdir -- "$CONF_DIR" 2>/dev/null || true
        say "Removed the pairing code."
    elif [ -f "$CODE_FILE" ]; then
        say "The pairing code stays in $CODE_FILE (--uninstall --purge removes it)."
    fi
}

install() {
    command -v gnome-shell >/dev/null || die "GNOME Shell not found: this extension is for the GNOME desktop."
    [ -f "$SRC/metadata.json" ] || die "extension files not found next to this script ($SRC)."
    ver=$(gnome-shell --version 2>/dev/null | sed -E 's/[^0-9]*([0-9]+).*/\1/')
    grep -q "\"$ver\"" "$SRC/metadata.json" ||
        say "Warning: made for GNOME Shell 50, this is $ver; it may not load."

    code=${1:-}
    if [ -z "$code" ] && [ -f "$CODE_FILE" ]; then
        code=$(cat "$CODE_FILE")
        say "Using the pairing code already saved."
    fi
    while [ -z "$code" ]; do
        [ -t 0 ] || die "no pairing code: ./install.sh XXXX-XXXX"
        printf 'Pairing code (setup page of the panel, Integrations): '
        read -r code
    done
    norm=$(normalize_code "$code") || die "\"$code\" isn't a pairing code (8 letters and digits, like K7PM-3QXD)."

    mkdir -p "$CONF_DIR"
    (umask 077 && printf '%s\n' "$norm" > "$CODE_FILE")
    chmod 600 "$CODE_FILE"

    mkdir -p "$EXT_DIR"
    if [ -L "$DEST" ]; then
        rm -- "$DEST"
    elif [ -d "$DEST" ]; then
        rm -r -- "$DEST"
    fi
    mkdir -p "$DEST"
    cp "$SRC"/metadata.json "$SRC"/extension.js "$SRC"/lan.js "$SRC"/stylesheet.css "$DEST"/

    set_enabled_setting 1
    if gnome-extensions enable "$UUID" 2>/dev/null; then
        say "Installed and on. If the top bar doesn't show it (or shows an old version), log out and back in."
    else
        say "Installed. Log out and back in to see it in the top bar."
    fi
}

case "${1:-}" in
--uninstall) uninstall "${2:-}" ;;
-h | --help) sed -n '2,6s/^# \{0,1\}//p' "$0" ;;
-*) die "unknown option $1 (--help)" ;;
*) install "${1:-}" ;;
esac
