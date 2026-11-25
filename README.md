# Med Button

A one-button tabletop device that answers a single question at a glance: **did I take my meds today?**

It's firmware for an [M5Stack AtomS3 Lite](https://docs.m5stack.com/en/core/AtomS3%20Lite) (ESP32-S3, about 24 mm square) with one RGB LED under one button. Hold the button for a second when you take your dose. The light shows today's status, gets more urgent as the evening goes on, and works with your phone through push notifications, an iPhone Shortcut, and a local web page.

## Why

Remembering whether you've *already* taken a daily pill is surprisingly hard, and doubling up or skipping are both bad outcomes. Phone reminders get swiped away. This device keeps one piece of state physically on the nightstand, and it keeps working when Wi-Fi is down.

## Highlights

- **State machine on a 3am day boundary.** The device stores only the last day you took a dose, in flash (NVS). It derives "taken / not yet / due / late / missed yesterday" from that and the clock, so a reboot or power cut never loses state.
- **Escalating reminders.** The light goes blue, then amber at med time, then flashing red when late, then solid red the next morning. Each stage has its own push notification through [ntfy](https://ntfy.sh) with **Took it** / **Mute** action buttons.
- **Two-way phone control with no server or app to maintain.** Notification buttons and an iPhone Shortcut post to an ntfy command topic. The device polls it, applies the command, and confirms.
- **The button never stops responding.** Button and LED handling run in their own FreeRTOS task, separate from networking. A mutex guards shared state, so slow HTTPS calls can't block a press.
- **Local web UI** at `http://medbutton.local` (mDNS). It has a status page, a JSON API (`/api/status`), and a settings page for colors, brightness, flash patterns and every reminder time. Settings are stored versioned in NVS, with migration from older versions.
- **Graceful degradation.** Wi-Fi setup happens through a captive portal (WiFiManager). If home Wi-Fi isn't around, the device falls back to a phone hotspot. When it's offline, it shows a distinct "offline" blink while the light and button keep working locally.

## Stack

Iterated with Claude

C++ (Arduino framework on ESP-IDF/FreeRTOS) · PlatformIO · Adafruit NeoPixel · WiFiManager · ESP32 `WebServer`, `HTTPClient`, `Preferences`, mDNS · ntfy.sh · iOS Shortcuts

```
src/main.cpp              firmware: state, LED/button task, notifications, web UI
include/config.example.h  defaults & secrets template (copy to config.h)
include/icon.h            home-screen icon for the status page
flash.command             one-click build + upload on macOS
```

---

## Light

| Light | Meaning |
|---|---|
| Green, flash every 30 seconds | Taken today (10 quick flashes right after you press) |
| Blue, flash every 60 seconds | Not yet today, nothing wrong |
| Amber, flash every 3 seconds | Med time (from 8:30pm), not taken yet |
| Red, flash every 3 seconds | Late (from 9:45pm), not taken yet |
| Red, solid | Missed yesterday (from 3am until you mute it or 8:30pm) |
| 10 quick flashes | When amber or red starts, then every 15 minutes |
| White, breathing | Starting up / waiting for the clock (needs Wi-Fi) |
| Two magenta blips every 10 seconds | No Wi-Fi for over a minute. Light and button still work; phone notifications and the Shortcut pause |
| Purple, breathing | Wi-Fi setup mode |

## Button

| Action | What it does |
|---|---|
| Hold ~1 second | Mark today as taken |
| Single tap | Nothing |
| Double-tap while green | Undo today's press |
| Hold while plugging in | Open Wi-Fi setup |

The day switches over at 3am.

## Phone notifications (ntfy)

- **8:30pm, med time:** if you haven't pressed today. Has a **Took it** button.
- **9:45pm and 11:30pm:** if you still haven't pressed. Each has a **Took it** button.
- **8:00am:** if yesterday was missed, then every hour until you tap **Mute** or take today's dose (stops at 8:30pm). Has a **Mute** button.
- After a "Took it" from your phone, a quiet **Marked as taken** notification confirms the device got it.

## Status page

On your home Wi-Fi, open **http://medbutton.local** on your phone. It shows the current state, last dose, today's reminders, and all the rules, with a Mark taken button. Raw data is at **http://medbutton.local/api/status**. Tip: Share > Add to Home Screen gives it a pill icon.

## Settings

Open **http://medbutton.local/settings** (or the Settings button at the bottom of the status page) to change med time, late time, reminder notifications, missed-day alerts, flash intervals, brightness, and when the day starts. Changes save on the device and apply right away. The values in `include/config.h` are only the defaults, used on first start or after **Reset to defaults**.

**Solid or flashing:** in Settings > Light, green/blue (together) and late red can each be set to **Flashing** or **Solid**. Amber always flashes, and missed-yesterday red is always solid. The 10 quick flashes still repeat in either mode.

## Setup

1. Install **VS Code**, then the **PlatformIO IDE** extension.
2. Open this folder in VS Code.
3. Copy `include/config.example.h` to `include/config.h`. Set `NTFY_TOPIC` to a long random name, like `ryan-meds-7f3k9x2q`. Adjust times if you like.
4. Plug the Atom into your computer with a USB-C cable that carries data, and click PlatformIO's **Upload** arrow. The first build downloads tools and takes a few minutes.
   - If it can't find the device: hold the small reset button on the Atom's side for about 2 seconds, then try again.
5. Plug the Atom into a phone charger. It breathes **purple**.
6. On your iPhone, go to Settings > Wi-Fi and join **MedButton-Setup**. A setup page opens; pick your home Wi-Fi (2.4 GHz) and enter the password.
7. Install the **ntfy** app and subscribe to your topic name (just `NTFY_TOPIC`, not the `-cmd` one).
8. Make the Shortcut below.

It then breathes white for a few seconds and shows **red** the first time (no history yet). Hold for a second when you take your meds.

To change Wi-Fi later, hold the button while plugging it in.

## Traveling (phone hotspot)

Your iPhone hotspot (`HOTSPOT_SSID` in `config.h`) is saved as a backup network. When home Wi-Fi isn't around, the device alternates between the two every 20 seconds and connects to whichever it finds. Turn on Personal Hotspot with **Maximize Compatibility** on (the Atom needs 2.4 GHz).

The clock resets when the Atom loses power, so after plugging it in somewhere new, turn on the hotspot briefly so it can get the time. Times stay on Pacific time.

## iPhone Shortcut: "Took meds"

Works from anywhere. The light updates within about 30 seconds.

1. Open **Shortcuts**, tap **+**, and name it **Took meds**.
2. Add the action **Get Contents of URL**.
3. URL: `https://ntfy.sh/` (just that).
4. Tap **Show More**:
   - Method: **POST**
   - Request Body: **JSON**
   - Add a **Text** field: key `topic`, value your topic name plus `-cmd` (e.g. `ryan-meds-7f3k9x2q-cmd`)
   - Add a **Text** field: key `message`, value `taken`
5. Tap the shortcut's icon and pick the **pill** glyph, so it doesn't look generic on your home screen.
6. Done. Run it from the home screen, a widget, the Action button, or "Hey Siri, took meds."

A second shortcut with `message` = `mute` mutes the missed-dose alerts.

**Careful:** marking taken counts for *today*. If you take a missed dose the next morning, use Mute, not Took meds, or tonight's light will already be green.
