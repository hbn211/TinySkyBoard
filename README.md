# TinySkyBoard

A desk clock, weather station and message board for the
[Waveshare ESP32-C6-LCD-1.47](https://www.waveshare.com/esp32-c6-lcd-1.47.htm).
It shows the time, current weather and a 5-day forecast, and displays messages you send
from your phone over Telegram — right away or on a schedule.

Everything is configured from your phone through a setup page served by the board itself:
no reflashing when you move or change networks.

**Version 1.0.0** — see the [changelog](CHANGELOG.md). The version also shows at the bottom of
the setup page and in the bot's `/status` reply.

![TinySkyBoard running on a Waveshare ESP32-C6-LCD-1.47](docs/images/panel.jpg)

*Landscape layout, with the interface in Portuguese (English is the default).*

| Setup page | Built-in help |
|---|---|
| ![Setup page](docs/images/setup_en.png) | ![Help page](docs/images/help_en.png) |

## Features

- **Clock and date** synced over NTP; the time zone (including daylight saving) is detected
  from the city.
- **Weather now + 5-day forecast** from [Open-Meteo](https://open-meteo.com/) — no API key.
  Just type the city name; coordinates are looked up automatically.
- **Telegram messages** on the screen. Up to 5 messages take turns with the dashboard.
  The bot warns you when a message is too long to fit.
- **Scheduled messages** (`/schedule`): a step-by-step assistant with quick-reply buttons —
  time, duration and repeat (once, every day, Mon–Fri, weekends).
- **Uptime notices**: the bot tells you when the panel powers on (and why — e.g. a power cut,
  with how long it was off) and when the internet comes back after an outage. `/status` answers
  whether it's alive.
- **Day/night dimmer**: screen and LED brightness follow local sunrise and sunset, with a
  30-minute transition.
- **RGB LED** in the color of the current weather, blinking when a message arrives.
- **Setup portal**: the board creates its own Wi-Fi with a QR code on screen. Pick the network,
  city, language, unit, board orientation and brightness (with live preview) from your phone.
- **Three languages** — English, Portuguese and Spanish — on the screen, the setup page and
  the bot.

## Hardware

**Waveshare ESP32-C6-LCD-1.47**: ESP32-C6 (Wi-Fi 6, BLE 5), 4 MB flash, 1.47" ST7789 LCD
(172×320), WS2812 RGB LED, BOOT and RESET buttons. Nothing else is needed — a USB-C cable
powers it.

| Function | GPIO |
|---|---|
| LCD MOSI / SCLK / CS / DC / RST | 6 / 7 / 14 / 15 / 21 |
| LCD backlight (PWM) | 22 |
| RGB LED (WS2812) | 8 |
| BOOT button | 9 |

With the screen facing you and the USB port down, the two buttons sit on the USB edge:
**left = control (BOOT)**, **right = reset**.

## Building and flashing

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v6.0** (other 5.4+
versions will probably work). Dependencies (LVGL, esp_lvgl_port, led_strip, cJSON) are
downloaded by the IDF Component Manager on the first build.

```bash
git clone https://github.com/hbn211/TinySkyBoard.git
cd TinySkyBoard
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # the port name may differ
```

You don't need to set anything in `menuconfig`. With an empty Wi-Fi network (the default), the
board boots straight into setup mode.

## First setup

1. Power the board. The screen shows **SETUP MODE** and a QR code; the LED is orange.
2. Scan the QR code (or join the open **TinySkyBoard** Wi-Fi). The setup page opens by itself —
   if not, open `http://192.168.4.1`.
3. Pick your network, type the password and the city, then tap **Save and restart**.

To change settings later, **hold the control button for 5 seconds**. The panel also opens the
setup page by itself if it can't connect to Wi-Fi within 45 seconds (for example, after you
move). Without any change, setup mode restarts after 15 minutes and tries the Wi-Fi again.

## Using it

### Control button

| Action | What it does |
|---|---|
| Quick press | Shows active message #1; each press goes to the next, then back to the dashboard. 10 s without pressing returns to normal. |
| Hold 2 s on a message | Deletes it (a red bar fills while you hold; let go earlier to cancel). |
| Hold 5 s on the dashboard | Enters setup mode (and leaves it, without saving). |

### LED

| LED | Meaning |
|---|---|
| Solid blue | Connecting to Wi-Fi |
| Solid green | Connected, fetching the weather |
| Weather color | All good: yellow clear sky, gray cloudy, blue rain, purple storm… |
| 3 white blinks | A message arrived |
| Solid red | Wi-Fi is down (reconnects by itself) |
| Solid orange | Setup mode |

### Telegram bot

1. In Telegram, talk to **@BotFather**, send `/newbot` and copy the token.
2. Paste the token on the setup page.
3. Send any message to your bot. **The first chat to talk to it becomes the owner**; everyone
   else gets "this panel is private".

Commands work in any of the three languages; the bot's `/` menu shows them in the panel's
language.

| English | Português | Español | |
|---|---|---|---|
| *(any text)* | | | Shows it on the panel |
| `/schedule` | `/agendar` | `/programar` | Schedule a message |
| `/list` | `/agenda` | `/lista` | List what is scheduled |
| `/cancel N` | `/cancelar N` | `/cancelar N` | Delete schedule N |
| `/clear` | `/limpar` | `/borrar` | Remove all messages from the screen |
| `/status`, `/ping` | `/status` | `/estado` | Is it on? Uptime, Wi-Fi, weather |
| `/help` | `/ajuda` | `/ayuda` | List the commands |

The scheduling assistant understands times like `18:30`, `18h`, `tomorrow 08:00`, `12/25 09:00`
(day/month in Portuguese and Spanish) and `in 30 min`, and durations like `30 min`, `2 h`,
`1h30` or `1 day`.

## Privacy and security

- **Wi-Fi password and Telegram token** entered on the setup page are stored **only on the
  board** (NVS flash). They never touch your computer or this repository.
- Anything you type in `menuconfig` ends up in `sdkconfig` (ignored by git) **and inside the
  compiled firmware**. Don't share a `.bin` built with your credentials in `menuconfig` —
  the setup page is the safer place for them.
- NVS is **not encrypted**: someone with physical access to the board could read the
  credentials.
- The setup Wi-Fi **TinySkyBoard** is open while setup mode runs (at most 15 minutes).
- The bot only obeys its owner. To hand it over, enter `0` as the token on the setup page and
  then the token again: the next chat to talk to it becomes the owner.

## Project layout

```
main/
  main.c          boot, control button
  board.c/.h      LCD (ST7789 + LVGL), backlight, RGB LED, pins
  ui.c            dashboard, message screen, setup-mode screen
  net.c           Wi-Fi, NTP, Open-Meteo weather and geocoding, HTTPS helper
  telegram.c      bot: messages, commands, /schedule assistant, uptime notices
  schedule.c/.h   active messages and schedules
  dimmer.c        day/night brightness from sunrise/sunset
  led.c           RGB LED
  portal.c        setup mode: access point, captive DNS, setup page
  portal_help.c   help page
  config.c/.h     settings in NVS
  i18n.c/.h       all user-facing texts (en, pt, es)
  fonts/          Montserrat with Latin-1 accents + LVGL symbols
partitions.csv    single 3.9 MB app partition
sdkconfig.defaults
```

The fonts in `main/fonts/` were generated with
[lv_font_conv](https://github.com/lvgl/lv_font_conv) from Montserrat Medium and Font Awesome
(the exact command is in the header of each file). Emoji aren't included, so the bot drops them.

## Known limitations

- Emoji don't show on the screen (accents do).
- One owner per bot.
- The board-position picker assumes rotation 0 is "USB at the bottom". If "left" and "right"
  come out swapped on your board, swap the 1 and the 3 in `BOARD_USB_TO_ROT` (`main/board.h`).

## Credits and licenses

This project is released under the [MIT License](LICENSE). It builds on:

| Component | License |
|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) | Apache-2.0 |
| [LVGL](https://github.com/lvgl/lvgl) | MIT |
| [esp_lvgl_port](https://components.espressif.com/components/espressif/esp_lvgl_port), [led_strip](https://components.espressif.com/components/espressif/led_strip) | Apache-2.0 |
| [cJSON](https://github.com/DaveGamble/cJSON) | MIT |
| [Montserrat](https://github.com/JulietaUla/Montserrat) font | SIL Open Font License 1.1 |
| [Font Awesome Free](https://fontawesome.com/) icons (via LVGL) | SIL OFL 1.1 (font), CC BY 4.0 (icons) |

Weather data by [Open-Meteo.com](https://open-meteo.com/), licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The free Open-Meteo API is for
non-commercial use.
