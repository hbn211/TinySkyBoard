# Changelog

## 1.0.0 — 2026-10-14

First public release.

- Clock and date over NTP, with the time zone (and daylight saving) detected from the city.
- Current weather and 5-day forecast from Open-Meteo; city name lookup in English, Portuguese
  and Spanish.
- Telegram bot: up to 5 messages taking turns on screen, a warning when a message is too long,
  `/schedule` assistant with quick-reply buttons (once, every day, Mon–Fri, weekends),
  `/list`, `/cancel`, `/clear`, `/status`, `/help`.
- Notices when the panel powers on (with the reason and how long it was off) and when the
  internet comes back.
- Control button: browse active messages, hold 2 s to delete one, hold 5 s for setup mode.
- Setup portal with captive DNS and QR code: Wi-Fi, city, language, unit, board position with
  live preview, day/night brightness for screen and LED, Telegram token, built-in help.
- Day/night dimmer following local sunrise and sunset.
- RGB LED in the current weather color; status colors and message blink.
- English, Portuguese and Spanish everywhere.
