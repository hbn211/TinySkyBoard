# Changelog

## 1.1.0 — 2026-10-16

- **Govee Wi-Fi thermometers** through the Govee OpenAPI: temperature, humidity, today's low and
  high, and the trend over the last hour. One sensor fills the screen, two share it. Polling
  follows each sensor's uploads, "updated" shows when the reading changed, and a sensor that
  stops reporting leaves the screen. API key and upload interval on the setup page.
- `/status` lists the Govee sensors, including the ones that stopped reporting.
- Screen rotation times (clock, sensor pages, messages) on the setup page; 5 s by default
  (was 10 s for the clock and 6 s per message).
- The LED blinks white until new messages are opened with the button; its brightness is a
  separate setting (0% = no blink). Unread messages have an orange header.
- **Set up over Wi-Fi** card after the last message: hold 2 s to open the setup page on the home
  network, with a QR code to its address. Closes with a click or after 10 minutes.
- `tools/govee_probe.py` to check the Govee API from a computer.

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
