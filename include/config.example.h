// Copy this file to config.h (same folder) and fill in your values.
// Times and light settings here are only DEFAULTS: change them anytime at
// http://medbutton.local/settings without re-uploading.
// Wi-Fi is NOT set here: you pick your network from your phone on first start.
#pragma once

// Backup network: your phone hotspot. Used when home Wi-Fi isn't available.
// On iPhone, turn on Personal Hotspot > Maximize Compatibility (2.4 GHz).
// Leave HOTSPOT_SSID as "" to turn off.
#define HOTSPOT_SSID     "Your Hotspot Name"
#define HOTSPOT_PASSWORD "your-hotspot-password"

// Time zone (Pacific, with daylight saving handled automatically)
#define TZ_STRING "PST8PDT,M3.2.0,M11.1.0"

// The day switches over at this hour (3 = 3am)
#define DAY_START_HOUR 3

// Med time: if not taken yet, the light turns amber (flash every 3 s) and
// your phone gets a notification. 24-hour clock. Set MED_ALERT_HOUR to -1 to turn off.
#define MED_ALERT_HOUR   20
#define MED_ALERT_MINUTE 30

// Late: if still not taken, the light turns red (flash every 3 s).
#define LATE_HOUR   21
#define LATE_MINUTE 45

// Amber, red and solid red also give 10 quick flashes every this many minutes.
#define BURST_EVERY_MIN 15

// Night reminders, {hour, minute} on a 24-hour clock, earliest first (up to 4).
// Each sends one notification if today isn't taken yet.
#define NIGHT_REMINDERS { {21, 45}, {23, 30} }

// Morning alert: if yesterday was missed, notify at this time, then repeat
// every FOLLOWUP_MINUTES until you tap the button (mute) or take today's dose.
// Follow-ups stop at med time. Set MORNING_HOUR to -1 to turn off.
#define MORNING_HOUR     8
#define MORNING_MINUTE   0
#define FOLLOWUP_MINUTES 60

// ntfy channel name. Make it long and hard to guess, since anyone who knows
// it can read your notifications. Subscribe to the same name in the ntfy app.
#define NTFY_TOPIC "ryan-meds-change-this-to-something-random"

// Status page address on your home network: http://medbutton.local
#define HOSTNAME "medbutton"

// Light brightness, 0-255. Red is brighter on purpose.
#define BRIGHTNESS     40
#define RED_BRIGHTNESS 255
