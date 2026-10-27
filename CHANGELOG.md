# Changelog

## 1.8.0 — 2026-10-27

- **The panel in the computer's top bar** (experimental): a GNOME Shell extension that mirrors the
  panel on the same Wi-Fi network — weather, sensors, airing-out hint, the latest messages and the
  next schedule. It only listens. Install and uninstall in the README.
- **Share on the home network**, off by default, in a new section of the setup page (*Computer and
  other panels*). Ticking it shows the panel's **pairing code** right away: the computer and other
  panels need it to hear this panel. "Make a new code" shows a new one at once and, saved, cuts off
  the ones with the old code.
- Panels with the same code on the same network share messages from Telegram and new schedules.
- The help page has a section about it.
- `tools/lan_peer.py` acts as a panel from a computer, for testing.

## 1.7.0 — 2026-10-25

- Scheduled messages with ✅ Done in Telegram can go to **everyone on the bot**, not only to whoever
  scheduled it: `/schedule` asks "only me / everyone" when there are invited people, and the
  scheduled messages page has a choice (don't send / whoever scheduled it / everyone). Whoever taps
  Done clears it; the other copies say "✅ Done by Ana 14:05". `/list` marks these with 🔔👥.
- **Notification LED**: color and mode (blink, breathe, solid) for unread messages and for a reminder
  at night, on the setup page with a live preview on the LED. Defaults as before: white blink, red
  breathing.

## 1.6.2 — 2026-10-19

- Airing out: **"yes" only when the humidity inside is high** (60 % by default, adjustable on the
  setup page; holds until 5 % under it) besides the drier air outside, so the house isn't cooled
  whenever it's cold outside. The Telegram message gives the humidity inside and says to open the
  windows wide for 5–10 minutes and close them.

## 1.6.1 — 2026-10-19

### Fixes

- The airing-out hint never showed (and its alert never went out): the pair's sensors were looked up
  by an ID field that is only filled for the screen. They're matched by the stored ID now.

## 1.6.0 — 2026-10-19

- **Dew point** on every Govee sensor card: the water actually in the air, which relative humidity
  alone hides when temperatures differ.
- **Comparisons**: the temperature difference stays in the middle, now with the humidity and the dew
  point differences under it (cyan when A holds more water, sand when less) and each sensor's dew
  point on its card.
- **Airing out**: per comparison, "sensor B is outdoors" adds *Air out: yes / no / either* (dew point
  inside vs outside, 3 °C margin with hysteresis), and an optional Telegram alert when airing out
  helps and when you can close the windows, to everyone on the bot (at most every 3 h, never at
  power-on or at night). "Air out: yes" shows as a pulsing green band.
- The configuration backup carries the new options; an older file loads with them off.

## 1.5.2 — 2026-10-19

### Fixes

- Sensor comparisons were never saved: the Govee IDs ("AA:BB:…") arrive URL-encoded and longer
  than the field they were read into. Every setup-page field is now decoded before it's cut to size.
- Comparison page: the sensor cards use their space (name, large temperature, humidity), the text
  under the difference no longer hides under the cards and the humidity difference is larger.
- Govee trend arrow: it disappeared when the temperature stayed the same for over an hour (the
  history only had changes); now a value holds until the next change, the trend is recomputed every
  minute and shows "–" for a steady sensor from 30 min after power-on.
- Govee sensors show up right after power-on with their first reading, instead of waiting for the
  reading to change (up to one upload interval, longer indoors); "updated HH:MM" comes with the first change.
- Weather details: at night the UV tile says just "UV max" (it was cut as "UV · today's…"); tile
  titles that don't fit roll instead of being cut.

## 1.5.1 — 2026-10-18

### Fixes

- Govee: a sensor only counts as stopped (leaves the screen, "stopped" alert) after 12 hours without
  any change. Indoors the reading can stay the same for several uploads, which sent alert/back pairs
  every few minutes.

## 1.5.0 — 2026-10-18

- **Slide transitions**: a new page slides in from the right and the old one out to the left; the
  same page with new content (another sensor, another message) slides in a little as it appears.
  At night the pages just swap, so the dashboard never peeks out behind the night clock.
- **Time-left bar**: a thin line at the bottom shrinks until the next page while the pages take
  turns by themselves (hidden while browsing with the button and at night).
- README: the browser flasher is explained right at the top.

### Fixes

- Govee: a sensor no longer leaves the screen (and no "stopped sending" alert goes out) when the API's
  "online" flag flickers off for a moment; only 3 intervals without a new reading count.

## 1.4.0 — 2026-10-18

- **Browser flasher** on GitHub Pages (ESP Web Tools): flash or update the board from Chrome/Edge
  with illustrated steps, keeping the settings. Releases are built by GitHub Actions with the
  firmware attached.
- **More people on the bot**: `/invite` gives a link (and a code) valid for 10 minutes for someone at
  home; up to 4 people can send messages and schedules. `/members` lists and removes them. With
  more than one person, the panel and `/messages` show who sent each message; a ✅ Done reminder
  goes to whoever scheduled it. Alerts, `/rain`, `/invite` and `/members` stay with the owner, and
  the `/` menu shows those only in the owner's chat.
- **Scheduled messages page**: create, edit and delete scheduled messages from the browser (text,
  repeat, date and time, duration, ✅ Done in Telegram). Linked from the setup page when it's opened
  over the home network; using it keeps that page open.
- **Configuration backup** on the setup page: download the settings, sensor alerts and schedules
  (without the Wi-Fi password and tokens) and load them on another board.
- After a crash, the bot's power-on notice says where it happened (reason, task, address, version).

## 1.3.0 — 2026-10-18

- **Weather details page** after the dashboard, with widget-like tiles: feels-like (with the actual
  temperature), UV index (today's max at night), European air quality with a level bar, and the
  chance of rain for each of the next 6 hours. On by default; option and screen time on the setup page.
- **Night mode** (optional, 00:00–06:00 by default): only a large amber clock and the LED off; a new
  message or a click wakes the panel for a few seconds. An unread reminder keeps it awake, taking
  turns with the night clock, with the LED breathing red until it's read. A **Night mode** card
  turns it on now (a nap) or off for tonight with a 2 s hold.
- **Work forecast** (optional): the weather in the city where you work, on chosen weekdays and
  hours, with rain chance and temperature for the next 12 hours.
- **Schedule card**: browse the scheduled messages on the panel and delete today's occurrence or
  the whole series with a hold bar that acts on release (nothing / today / all / nothing).
- **✅ Done in Telegram**: `/schedule` asks whether the reminder should also arrive in Telegram when
  it starts, with a button that clears it from the screen. Deleting it on the panel relabels the
  button ("Done on the panel"); `/list` marks these schedules with 🔔.
- **Telegram alerts for the Govee sensors**: min/max temperature and humidity per sensor on the
  setup page (saved by sensor ID); one message when a reading leaves the range and one when it's
  back, with a small margin so it doesn't repeat. Optional message when a sensor stops sending.
- **Rain alert**, turned on/off with `/rain` (`/chuva`, `/lluvia`): 60 % chance or more within the
  next 2 hours, at most every 6 hours.
- **Sensor comparisons**: pick up to two pairs of Govee sensors; each gets a page with the
  difference (A − B, colored warmer/cooler) and both readings.
- Setup page grouped in titled sections, shown as two columns of cards on a computer screen. Work
  and night-mode times take minutes (e.g. 12:15–13:45) with the phone's time picker.
- Portrait: the message header shows the date/time on a second line; long titles and city names
  roll instead of being cut. Long scheduled texts roll slowly in a medium font and get more time
  to read (20 s plus 1 s per 4 characters, up to two minutes).
- `/list` shows "deleted from the screen today" / "next one skipped"; `/status` shows free / lowest /
  largest memory block. The serial log no longer shows the Wi-Fi name or Telegram chat ids.

### Fixes

- A click before the Wi-Fi connected restarted the panel (the message list wasn't loaded yet); saved
  messages now show from power-on. Clicks during network work are no longer lost.
- The RGB LED showed red as green and green as red (this board's LED takes RGB order, not the usual
  WS2812 GRB).
- Free memory could drop to about 1 KB when several HTTPS connections overlapped, so the weather
  failed to load; TLS buffers are now allocated only while data moves (lowest free memory ~45 KB).
- After a failed air-quality fetch the last value stays on screen for at most one hour.
- Turning night mode on from its card started the clock only ~20 s later.
- At night a new message shows alone and a reminder takes turns only with the night clock.

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
