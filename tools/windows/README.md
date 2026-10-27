# TinySkyBoard in the Windows notification area (experimental)

A mirror of a TinySkyBoard panel on the same Wi-Fi network: the temperature as the tray icon, the
panel in its menu. Install and use: see *On the computer* in the main [README](../../README.md). The
release workflow builds `TinySkyBoard-<version>-windows.exe` from this folder.

## Files

- `lan.go`: the receiver (signature, decryption, replays, the state lines); no Windows code
- `texts.go`: the menu lines in English, Portuguese and Spanish
- `icon.go`: the temperature drawn as an `.ico` (16 to 32 px, white or black for the taskbar theme)
- `main_windows.go`: the tray icon and menu (fyne.io/systray), the code box (PowerShell's
  `InputBox`), *Start with Windows* (the `HKCU\…\Run` key) and the log, all in `%APPDATA%\TinySkyBoard`
- `main_other.go`: the same receiver on a console, for testing on Linux or macOS

## Build and test (on any OS)

```sh
go test ./...
go run . -code XXXX-XXXX -secs 60     # prints the menu lines for each state the panel sends
go run . -icons /tmp/icons            # writes the icon in every size as a PNG preview
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -trimpath \
  -ldflags "-H windowsgui -s -w -X main.version=dev" -o TinySkyBoard.exe .
```
