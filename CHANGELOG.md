# Changelog

## Unreleased

### Fixes

- The RGB LED showed red as green and green as red (this board's LED takes RGB order, not the usual
  WS2812 GRB): the reminder's red breathing looked green, and so did "Wi-Fi down".
- Free memory could drop to about 1 KB when several HTTPS connections overlapped, so the weather
  failed to load; TLS buffers are now allocated only while data moves (lowest free memory ~49 KB).
  `/status` and the log show free / lowest / largest block.
- Turning night mode on from its card started the clock only ~20 s later.
- `/list` said a schedule was "on the panel now" after today's occurrence had been deleted with the
  button; it now says "deleted from the screen today".
- At night a new message shows alone and a reminder takes turns only with the night clock (no weather
  pages unless you click).

- **Telegram alerts for the Govee sensors**: min/max temperature and humidity per sensor on the
  setup page (saved by sensor ID); one message when a reading leaves the range and one when it's
  back, with a small margin so it doesn't repeat. Optional message when a sensor stops sending.
- **Rain alert**, turned on/off with `/rain` (`/chuva`, `/lluvia`): 60 % chance or more within the
  next 2 hours, at most every 6 hours.
- Portrait: the message header shows the date/time on a second line; long titles (message,
  weather details, other cities, status list) scroll instead of being cut.
- **Night mode** (optional, setup page, 00:00–06:00 by default): only a large amber clock that fills
  the screen and the LED off; a new message or a click wakes the panel for a few seconds, and an
  unread scheduled message (a reminder) keeps it awake, with the LED breathing red, until it's read.
  A **Night mode** card after the setup card turns it on now (a nap) or off for tonight with a 2 s hold.
- **Weather details page** after the dashboard: feels-like temperature, UV index (today's max at
  night), European air quality index and the chance of rain for each of the next 6 hours, with
  the usual color scales. On by default; option and screen time on the setup page.

## 1.2.0 — 2026-10-17

- **Other cities** (optional, up to 4, on the setup page): a page per city with its local time,
  date, time difference and weather.
- **12-hour clock** (AM/PM) option, used on the screen and in the bot's replies.
- Wi-Fi signal as 1–4 bars instead of dBm, sweeping while connecting (no "Connecting…" text); lighter gray for secondary text.
- Animated weather icon on the dashboard instead of the weather's name (sun with a glow, moon at
  night, drifting clouds, falling rain and snow, fog, lightning), and weather icons in the forecast.
  Larger text for the status bar, the city and humidity/wind.
- Color emoji in messages (Twemoji, about 370 of the most used). Emoji the panel doesn't have are
  left out and the bot says so.
- `/messages` (`/mensagens`, `/mensajes`): which messages are on the screen and whether they were read.
- `/clear` only deletes the sent messages; scheduled ones stay (delete them with `/cancel N`).
- The **Set up over Wi-Fi** card also shows a status list: IP address, last weather update,
  Telegram and Govee connection (invalid key, sensors on screen) and uptime.
- A deleted occurrence of a repeating schedule stays deleted after a restart, and an unread one
  stays unread.
- A Govee sensor only shows once the panel has seen its reading change, so a value of unknown age
  is never shown.
- Forecast columns spaced evenly; other cities' low/high without decimals.
- Typing several cities in the main city field ("Munich, Tokyo") keeps the first one and moves the
  rest to **Other cities**.

### Fixes

- The setup page opened over the home network could stop halfway: it's now sent in 4 KB chunks,
  and Wi-Fi power saving is off (the board runs on USB).
- Heap corruption (crash loop) when a one-line text cut with "…" changed: LVGL 9.3's
  `lv_label_set_text_fmt` doesn't undo the dots; formatted texts now go through `lv_label_set_text`.
- Single-line texts (status bar, city, status list) wrapped onto a second line instead of ending
  in "…".
- The gear on the setup card could land on top of the status list after a message was shown.

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
