# TinySkyBoard on the GNOME top bar (experimental)

A mirror of a TinySkyBoard panel on the same Wi-Fi network, in the top bar of GNOME Shell 50.
Install, set up and uninstall: see *On the computer (GNOME top bar)* in the main
[README](../../README.md).

## How it works

Each panel broadcasts a state packet on UDP port 47474 (`main/lan.c`): text lines with the weather,
the sensors, the airing-out hint, the unread count, the latest two messages and the next schedule.
It goes out within 5 s of any change and at least every 30 s. The packet is signed with
HMAC-SHA256, keyed by SHA-256 of the pairing code; `lan.js` checks the signature and drops anything
else. It never sends a packet.

`test_lan.js` runs the receiver without GNOME Shell and prints each state:

```sh
gjs -m test_lan.js XXXX-XXXX 40
```

For development, link the folder instead of installing a copy, so edits are picked up:
`ln -s "$PWD/tinyskyboard@hbn211.github.io" ~/.local/share/gnome-shell/extensions/`
(`install.sh --uninstall` removes only the link). After changing the extension, log out and back in
to load it (on Wayland GNOME Shell can't be
restarted in place). Errors show in `journalctl --user -b | grep -i tinyskyboard`.
