// Med Button for M5Stack AtomS3 Lite.
//
// Light
//   Green, flash / 30 s     taken today (10 quick flashes when you press)
//   Blue, flash / 30 s      not yet today, nothing wrong
//   Amber, flash / 3 s      med time (from 8:30pm), not taken yet
//   Red, flash / 3 s        late (from 9:45pm), not taken yet
//   Red, solid              missed yesterday (from 3am until muted or 8:30pm)
//   Amber/red/solid red also give 10 quick flashes on entry and every 15 min.
//   White, breathing        starting up / waiting for the clock
//   Two magenta blips/10 s  no Wi-Fi for over a minute (light still works; phone features paused)
//   Purple, breathing       Wi-Fi setup mode: join "MedButton-Setup" from your phone
//
// Button
//   Hold ~1 s               mark today as taken
//   Single tap              nothing (mute alerts from your phone instead)
//   Double-tap (green)      undo today's press
//   Hold while plugging in  open Wi-Fi setup
//
// Status page: http://medbutton.local (JSON at /api/status)
// The day switches over at 3am by default. All times are editable at /settings.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include "icon.h"

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy include/config.example.h to include/config.h and fill it in."
#endif

// ---------------------------------------------------------------- hardware

static const int LED_PIN = 35;     // one WS2812C-2020 under the button
static const int BUTTON_PIN = 41;  // active low
static const char *SETUP_AP_NAME = "MedButton-Setup";

// ---------------------------------------------------------------- timing

static const uint32_t HOLD_MS = 1000;        // hold to confirm
static const uint32_t DOUBLE_TAP_MS = 500;   // max gap between taps for undo
static const uint32_t MIN_TAP_MS = 30;       // ignore contact bounce
static const uint32_t FLASH_ON = 250;
static const uint32_t OFFLINE_AFTER_MS = 60000;    // show offline after 1 min without Wi-Fi
static const uint32_t RED_BLINK_ON = 400;
static const uint32_t SEND_RETRY_MS = 60000;    // retry a failed notification after 1 min

// ---------------------------------------------------------------- settings
//
// Editable on the status page (/settings) and saved on the device. The values
// in config.h are only the defaults. Clock times are minutes after midnight
// (20:30 = 1230); -1 means off.

#ifndef LATE_HOUR
#define LATE_HOUR   21
#define LATE_MINUTE 45
#endif
#ifndef BURST_EVERY_MIN
#define BURST_EVERY_MIN 15
#endif

static const int NIGHT_MAX = 4;
static const uint32_t SETTINGS_VERSION = 3;

struct Settings {
  uint32_t version;
  int32_t dayStart;            // hour the day switches over (0-6)
  int32_t med;                 // med time: amber + notification
  int32_t late;                // late: red
  int32_t night[NIGHT_MAX];    // extra "still not taken" notifications
  int32_t morning;             // missed-yesterday alerts start
  int32_t followupMin;         // ...and repeat this often
  int32_t burstMin;            // 10 quick flashes this often while amber/red
  int32_t greenSec;            // green: one flash this often
  int32_t blueSec;             // blue: one flash this often
  int32_t amberSec;            // amber: one flash this often (amber always flashes)
  int32_t brightness;          // green/blue/white, 1-255
  int32_t redBrightness;       // amber/red, 1-255
  // added in version 3 (keep new fields at the end so old settings migrate)
  int32_t redSec;              // red (late): one flash this often, when flashing
  int32_t calmSolid;           // 1 = green/blue stay on solid, 0 = flash
  int32_t redSolid;            // 1 = late red stays on solid, 0 = flash
};
static const size_t SETTINGS_V2_SIZE = sizeof(Settings) - 3 * sizeof(int32_t);
static Settings cfg;

static int32_t clockOrOff(int h, int m) { return h < 0 ? -1 : h * 60 + m; }

static void defaultSettings(Settings &c) {
  struct { int h, m; } night[] = NIGHT_REMINDERS;
  c.version = SETTINGS_VERSION;
  c.dayStart = DAY_START_HOUR;
  c.med = clockOrOff(MED_ALERT_HOUR, MED_ALERT_MINUTE);
  c.late = clockOrOff(LATE_HOUR, LATE_MINUTE);
  for (int i = 0; i < NIGHT_MAX; i++) {
    c.night[i] = i < (int)(sizeof(night) / sizeof(night[0])) ? clockOrOff(night[i].h, night[i].m) : -1;
  }
  c.morning = clockOrOff(MORNING_HOUR, MORNING_MINUTE);
  c.followupMin = FOLLOWUP_MINUTES;
  c.burstMin = BURST_EVERY_MIN;
  c.greenSec = 30;
  c.blueSec = 60;
  c.amberSec = 3;
  c.redSec = 3;
  c.calmSolid = 0;
  c.redSolid = 0;
  c.brightness = BRIGHTNESS;
  c.redBrightness = RED_BRIGHTNESS;
}

Adafruit_NeoPixel led(1, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences prefs;
WiFiManager wm;
WebServer server(80);

// ---------------------------------------------------------------- saved state
//
// A "med day" is the calendar date of (now - day start hour), in days since
// 1970-01-01. Everything is saved to flash so unplugging loses nothing.

int32_t lastTakenDay = -1;   // last day the button was held
int32_t prevTakenDay = -1;   // value before that, for undo
int32_t ackDay = -1;         // day a missed-dose alert was acknowledged (muted)
int32_t nightSentDay[4] = {-1, -1, -1, -1};  // day each night reminder went out
int32_t medSentDay = -1;     // day the med-time notification went out

static const char *NIGHT_KEYS[4] = {"night0", "night1", "night2", "night3"};

static void loadState() {
  prefs.begin("medbutton", false);
  lastTakenDay = prefs.getInt("taken", -1);
  prevTakenDay = prefs.getInt("prev", -1);
  ackDay = prefs.getInt("ack", -1);
  for (int i = 0; i < NIGHT_MAX; i++) nightSentDay[i] = prefs.getInt(NIGHT_KEYS[i], -1);
  medSentDay = prefs.getInt("med", -1);

  defaultSettings(cfg);
  Settings saved;
  size_t n = prefs.getBytes("cfg", &saved, sizeof(saved));
  if (n == sizeof(saved) && saved.version == SETTINGS_VERSION) {
    cfg = saved;
  } else if (n == SETTINGS_V2_SIZE && saved.version == 2) {
    // Older settings: keep them, use defaults for the new fields.
    memcpy(&cfg, &saved, SETTINGS_V2_SIZE);
    cfg.version = SETTINGS_VERSION;
    cfg.redSec = cfg.amberSec;  // red used to share amber's interval
  }
}

static void saveSettings(const Settings &c);  // defined after the state lock

static void loadCommandCursor();  // defined with the phone commands below

// Guards saved state, which both the UI task and loop() change.
static SemaphoreHandle_t stateLock;
struct StateGuard {
  StateGuard() { xSemaphoreTakeRecursive(stateLock, portMAX_DELAY); }
  ~StateGuard() { xSemaphoreGiveRecursive(stateLock); }
};

static void saveInt(const char *key, int32_t &field, int32_t value) {
  StateGuard g;
  field = value;
  prefs.putInt(key, value);
}

static void saveString(const char *key, const String &value) {
  StateGuard g;
  prefs.putString(key, value);
}

static void saveSettings(const Settings &c) {
  StateGuard g;
  cfg = c;
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
}

// ---------------------------------------------------------------- time

// Days since 1970-01-01 <-> civil date (Howard Hinnant's algorithms).
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

static void civilFromDays(int32_t z, int &y, unsigned &m, unsigned &d) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
}

static bool clockValid() { return time(nullptr) > 1700000000; }

static bool shiftedNow(struct tm &t) {
  if (!clockValid()) return false;
  time_t shifted = time(nullptr) - cfg.dayStart * 3600;
  localtime_r(&shifted, &t);
  return true;
}

// Today's med day, or -1 if the clock isn't set yet.
static int32_t medDay() {
  struct tm t;
  if (!shiftedNow(t)) return -1;
  return daysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

// Minutes since the day switched over (0 at 3am).
static int minutesIntoDay() {
  struct tm t;
  if (!shiftedNow(t)) return -1;
  return t.tm_hour * 60 + t.tm_min;
}

// A clock time expressed as minutes since the day switched over.
static int dayOffset(int hour, int minute) {
  return ((hour - cfg.dayStart) * 60 + minute + 1440) % 1440;
}

// A settings clock time (minutes after midnight) as minutes since the day
// switched over; 1440 ("never today") if it's off.
static int clockOffset(int32_t clock) {
  return clock < 0 ? 1440 : dayOffset(clock / 60, clock % 60);
}

static int firstNightOffset() {
  int best = 1440;
  for (int i = 0; i < NIGHT_MAX; i++) {
    int o = clockOffset(cfg.night[i]);
    if (o < best) best = o;
  }
  return best;
}

static int medOffset() { return clockOffset(cfg.med); }

// When the evening starts: med time or the first night reminder, whichever is
// earlier. Morning missed-dose alerts stop here.
static int eveningOffset() {
  int m = medOffset(), n = firstNightOffset();
  return m < n ? m : n;
}

// When the dose becomes "late" (red), as minutes since the day switch.
static int lateOffset() { return clockOffset(cfg.late); }

// Seconds since the day switched over, or -1 if the clock isn't set.
static int secsIntoDay() {
  struct tm t;
  if (!shiftedNow(t)) return -1;
  return (t.tm_hour * 60 + t.tm_min) * 60 + t.tm_sec;
}

// ---------------------------------------------------------------- status

enum Status { WAITING, TAKEN, OK_YESTERDAY, MISSED };

static Status currentStatus() {
  int32_t today = medDay();
  if (today < 0) return WAITING;
  if (lastTakenDay == today) return TAKEN;
  if (lastTakenDay == today - 1) return OK_YESTERDAY;
  return MISSED;
}

static bool missAcknowledged() { return ackDay == medDay(); }

// ---------------------------------------------------------------- light

static void show(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
  led.setPixelColor(0, led.Color(r * brightness / 255, g * brightness / 255,
                                 b * brightness / 255));
  led.show();
}

static void off() { show(0, 0, 0, 0); }

static float breathe(uint32_t period) {
  float phase = (millis() % period) / (float)period;
  return 0.15f + 0.85f * (phase < 0.5f ? phase * 2 : (1 - phase) * 2);
}

// Updated by loop() whenever Wi-Fi is connected.
static volatile uint32_t lastOnlineMs = 0;

// Offline = no Wi-Fi for over a minute (ignores brief drops and startup).
static bool isOffline() {
  return millis() - lastOnlineMs > OFFLINE_AFTER_MS;
}

static uint8_t feedbackBrightness();

// What the light should be doing right now.
//   happy  green / blue, one flash every 30 s
//   due    from med time (8:30pm): amber flash every 3 s
//   late   from 9:45pm: red flash every 3 s
//   missed yesterday not taken (from 3am): solid red
// due, late and missed also give 10 quick flashes on entry and every 15 min.
enum Phase { PH_WAITING, PH_TAKEN, PH_PENDING, PH_DUE, PH_LATE, PH_MISSED };

static Phase currentPhase(int &anchorSecs) {
  anchorSecs = -1;
  Status s = currentStatus();
  int secs = secsIntoDay();
  if (s == WAITING || secs < 0) return PH_WAITING;
  if (s == TAKEN) return PH_TAKEN;
  if (secs >= lateOffset() * 60) { anchorSecs = lateOffset() * 60; return PH_LATE; }
  if (secs >= medOffset() * 60)  { anchorSecs = medOffset() * 60;  return PH_DUE; }
  if (s == MISSED && !missAcknowledged()) { anchorSecs = 0; return PH_MISSED; }
  return PH_PENDING;
}

static void drawStatus() {
  uint32_t ms = millis();

  if (wm.getConfigPortalActive()) {
    show(160, 0, 255, cfg.brightness * breathe(2000));
    return;
  }

  int anchor;
  Phase ph = currentPhase(anchor);
  if (ph == PH_WAITING) {
    show(255, 255, 255, cfg.brightness * breathe(2000));
    return;
  }

  uint8_t r = 0, g = 0, b = 0;
  switch (ph) {
    case PH_TAKEN:   g = 255; break;
    case PH_PENDING: g = 80; b = 255; break;
    case PH_DUE:     r = 255; g = 140; break;  // amber
    default:         r = 255; break;           // late / missed: red
  }

  // 10 quick flashes when a warning phase starts and every cfg.burstMin minutes after.
  if (anchor >= 0) {
    static int32_t lastKey = -1;
    static uint32_t burstStart = 0;
    int slot = (secsIntoDay() - anchor) / ((cfg.burstMin > 0 ? cfg.burstMin : 15) * 60);
    int32_t key = (medDay() % 1000) * 10000 + ph * 1000 + slot;
    if (key != lastKey) { lastKey = key; burstStart = ms; }
    uint32_t t = ms - burstStart;
    if (t < 10 * 140) {
      if (t % 140 < 70) show(r, g, b, cfg.redBrightness); else off();
      return;
    }
  }

  // Offline: two quick magenta blips every 10 s, on top of the normal pattern.
  if (isOffline()) {
    uint32_t p = ms % 10000;
    if ((p >= 5000 && p < 5100) || (p >= 5250 && p < 5350)) {
      show(255, 0, 140, feedbackBrightness());
      return;
    }
  }

  switch (ph) {
    case PH_TAKEN:
    case PH_PENDING: {
      uint32_t period = (uint32_t)(ph == PH_TAKEN ? cfg.greenSec : cfg.blueSec) * 1000;
      if (cfg.calmSolid || ms % period < FLASH_ON) show(r, g, b, cfg.brightness); else off();
      break;
    }
    case PH_DUE:  // amber always flashes
      if (ms % ((uint32_t)cfg.amberSec * 1000) < RED_BLINK_ON) show(r, g, b, cfg.redBrightness); else off();
      break;
    case PH_LATE:
      if (cfg.redSolid || ms % ((uint32_t)cfg.redSec * 1000) < RED_BLINK_ON) show(r, g, b, cfg.redBrightness);
      else off();
      break;
    case PH_MISSED:
      show(r, g, b, cfg.redBrightness);
      break;
    default:
      break;
  }
}

// The button and light run in their own task (see uiTask) so slow network
// calls in loop() can never freeze them. Other code asks for a confirmation
// flash through this variable instead of driving the LED itself.
enum Feedback : uint8_t { FB_NONE, FB_TAKEN, FB_UNDO, FB_MUTED };
static volatile uint8_t feedbackRequest = FB_NONE;
static void requestFeedback(Feedback f) { feedbackRequest = f; }

static void flash(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness, int times) {
  for (int i = 0; i < times; i++) {
    show(r, g, b, brightness);
    delay(120);
    off();
    delay(120);
  }
}

static uint8_t feedbackBrightness() { return cfg.brightness * 2 > 255 ? 255 : cfg.brightness * 2; }

// ---------------------------------------------------------------- actions

// The med day a given moment belongs to.
static int32_t medDayAt(time_t when) {
  time_t shifted = when - cfg.dayStart * 3600;
  struct tm t;
  localtime_r(&shifted, &t);
  return daysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

// Record a dose for `day`. Never moves the record backwards.
static bool recordTaken(int32_t day) {
  StateGuard g;
  if (day < 0 || day <= lastTakenDay) return false;
  saveInt("prev", prevTakenDay, lastTakenDay);
  saveInt("taken", lastTakenDay, day);
  Serial.printf("Taken for day %ld\n", (long)day);
  return true;
}

static void markTaken() {
  if (medDay() < 0) return;  // no clock yet, can't record a date
  recordTaken(medDay());
  requestFeedback(FB_TAKEN);
}

static void undoTaken() {
  {
    StateGuard g;
    saveInt("taken", lastTakenDay, prevTakenDay);
  }
  requestFeedback(FB_UNDO);
  Serial.println("Undo");
}

// Mute today's missed-dose alerts. Only from the phone (notification button,
// Shortcut, or status page), never from the device button.
static void acknowledgeMiss() {
  {
    StateGuard g;
    saveInt("ack", ackDay, medDay());
  }
  requestFeedback(FB_MUTED);
  Serial.println("Miss acknowledged, alerts muted");
}

static void playFeedback(uint8_t f) {
  switch (f) {
    case FB_TAKEN:  // 10 quick flashes
      for (int i = 0; i < 10; i++) {
        show(0, 255, 0, feedbackBrightness());
        delay(70);
        off();
        delay(70);
      }
      break;
    case FB_UNDO:  flash(255, 255, 255, feedbackBrightness(), 2); break;  // white
    case FB_MUTED: flash(255, 0, 0, cfg.redBrightness, 2); break;
  }
}

// ---------------------------------------------------------------- button

static bool ignoreUntilRelease = false;
static bool buttonBusy = false;  // true while the button is being handled

// A single tap does nothing. Double-tap while green undoes today's press.
static void onTap() {
  static uint32_t lastTapAt = 0;
  uint32_t now = millis();
  Status s = currentStatus();

  if (s == TAKEN) {
    if (lastTapAt != 0 && now - lastTapAt < DOUBLE_TAP_MS) {
      undoTaken();
      lastTapAt = 0;
    } else {
      lastTapAt = now;
    }
  } else {
    lastTapAt = 0;
  }
}

static void handleButton() {
  static uint32_t pressedAt = 0;
  static bool fired = false;
  bool down = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();

  if (ignoreUntilRelease) {
    if (!down) ignoreUntilRelease = false;
    buttonBusy = down;
    return;
  }

  if (down) {
    buttonBusy = true;
    if (pressedAt == 0) {
      pressedAt = now;
      fired = false;
    }
    uint32_t held = now - pressedAt;
    if (!fired && held >= HOLD_MS) {
      fired = true;
      markTaken();
    } else if (fired) {
      show(0, 255, 0, cfg.brightness);
    } else {
      // brighten while holding so you can see it registering
      show(255, 255, 255, 20 + (uint32_t)200 * held / HOLD_MS);
    }
    return;
  }

  buttonBusy = false;
  if (pressedAt != 0) {
    uint32_t held = now - pressedAt;
    pressedAt = 0;
    if (!fired && held >= MIN_TAP_MS) onTap();
  }
}

// ---------------------------------------------------------------- notifications

// Phone -> device commands arrive on this topic ("taken", "mute").
static String commandTopic() { return String(NTFY_TOPIC) + "-cmd"; }

// Tappable buttons shown on the notification itself.
// Night reminders get "Took it". The missed-dose alert gets only "Mute", so a
// morning tap can't accidentally mark today (tonight's dose) as taken.
static String actionHeader(bool tookIt, bool mute) {
  String url = String("https://ntfy.sh/") + commandTopic();
  String a = "";
  if (tookIt) a += "http, Took it, " + url + ", body=taken, clear=true";
  if (tookIt && mute) a += "; ";
  if (mute) a += "http, Mute, " + url + ", body=mute, clear=true";
  return a;
}

static bool sendNtfy(const char *title, const char *message, const char *priority,
                     const String &actions = String("")) {
  WiFiClientSecure client;
  client.setInsecure();  // skips certificate check; acceptable for reminders
  HTTPClient http;
  String url = String("https://ntfy.sh/") + NTFY_TOPIC;
  if (!http.begin(client, url)) return false;
  http.addHeader("Title", title);
  http.addHeader("Priority", priority);
  http.addHeader("Tags", "pill");
  if (actions.length()) http.addHeader("Actions", actions);
  int code = http.POST(message);
  http.end();
  Serial.printf("ntfy '%s' -> %d\n", title, code);
  return code >= 200 && code < 300;
}

static void handleNotifications() {
  static uint32_t retryAt = 0;          // after a failure, wait before retrying
  static int32_t missedSentDay = -1;    // day the first missed alert went out
  static uint32_t missedSentAt = 0;     // when the last missed alert went out
  static int missedCount = 0;

  if (WiFi.status() != WL_CONNECTED) return;
  if (retryAt != 0 && (int32_t)(millis() - retryAt) < 0) return;

  int32_t today = medDay();
  int mins = minutesIntoDay();
  if (today < 0 || mins < 0) return;

  Status s = currentStatus();

  // Med time: once, if today isn't taken yet. Skipped if we're already past the
  // first night reminder (e.g. after a power cut); that reminder covers it.
  if (cfg.med >= 0 && s != TAKEN && mins >= medOffset() && mins < firstNightOffset() && medSentDay != today) {
    if (sendNtfy("Med time", "Time to take your meds.", "high", actionHeader(true, false))) {
      saveInt("med", medSentDay, today);
      retryAt = 0;
    } else {
      retryAt = millis() + SEND_RETRY_MS;
    }
    return;
  }

  // Night reminders: each once, if today isn't taken yet. If several are due
  // (for example after a power cut), only the latest one is sent.
  if (s != TAKEN) {
    int due = -1;
    for (int i = 0; i < NIGHT_MAX; i++) {
      if (cfg.night[i] >= 0 && mins >= clockOffset(cfg.night[i]) && nightSentDay[i] != today) due = i;
    }
    if (due >= 0) {
      const char *title = "Still not taken: meds";
      if (sendNtfy(title, "You haven't pressed the button today.", "high", actionHeader(true, false))) {
        for (int i = 0; i < NIGHT_MAX; i++)
          if (cfg.night[i] >= 0 && clockOffset(cfg.night[i]) <= clockOffset(cfg.night[due]))
            saveInt(NIGHT_KEYS[i], nightSentDay[i], today);
        retryAt = 0;
      } else {
        retryAt = millis() + SEND_RETRY_MS;
      }
      return;
    }
  }

  // Missed-dose alert: from the morning time until med time,
  // repeating until acknowledged (tap) or today's dose is taken.
  int morning = clockOffset(cfg.morning);
  if (cfg.morning >= 0 && s == MISSED && !missAcknowledged() && mins >= morning && mins < eveningOffset()) {
    bool due = missedSentDay != today ||
               millis() - missedSentAt >= (uint32_t)cfg.followupMin * 60000UL;
    if (!due) return;
    if (missedSentDay != today) missedCount = 0;
    const char *title = missedCount == 0 ? "Missed yesterday's meds" : "Reminder: missed yesterday's meds";
    if (sendNtfy(title, "Tap Mute to stop these alerts until tomorrow.", "urgent",
                 actionHeader(false, true))) {
      missedSentDay = today;
      missedSentAt = millis();
      missedCount++;
      retryAt = 0;
    } else {
      retryAt = millis() + SEND_RETRY_MS;
    }
  }
}

// ---------------------------------------------------------------- phone commands
//
// The iPhone Shortcut and the notification buttons post "taken" or "mute" to
// ntfy.sh/<NTFY_TOPIC>-cmd. We poll that topic every 30 s, so it works from
// anywhere. ntfy keeps messages ~12 h, so commands sent while the device is
// offline are applied when it reconnects.

static String lastCmdId;  // ntfy message id (or a start timestamp) to poll after

static void loadCommandCursor() { lastCmdId = prefs.getString("cmdid", ""); }

static String jsonString(const String &line, const char *key) {
  String pat = String("\"") + key + "\":\"";
  int s = line.indexOf(pat);
  if (s < 0) return String("");
  s += pat.length();
  int e = line.indexOf('"', s);
  return e < 0 ? String("") : line.substring(s, e);
}

static long jsonNumber(const String &line, const char *key) {
  String pat = String("\"") + key + "\":";
  int s = line.indexOf(pat);
  if (s < 0) return 0;
  return strtol(line.c_str() + s + pat.length(), nullptr, 10);
}

static void applyCommand(const String &raw, time_t sentAt) {
  String cmd = raw;
  cmd.trim();
  cmd.toLowerCase();
  if (cmd == "taken") {
    if (recordTaken(medDayAt(sentAt))) {
      requestFeedback(FB_TAKEN);
      sendNtfy("Marked as taken", "The button got your message.", "low");
    }
  } else if (cmd == "mute") {
    if (currentStatus() == MISSED && !missAcknowledged()) acknowledgeMiss();
  }
}

static void handleCommands() {
  static uint32_t lastPoll = 0;
  if (WiFi.status() != WL_CONNECTED || !clockValid()) return;
  if (lastPoll != 0 && millis() - lastPoll < 30000) return;
  lastPoll = millis();

  if (lastCmdId.length() == 0) {  // first run ever: ignore anything older than now
    lastCmdId = String((unsigned long)time(nullptr));
    saveString("cmdid", lastCmdId);
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = String("https://ntfy.sh/") + commandTopic() + "/json?poll=1&since=" + lastCmdId;
  if (!http.begin(client, url)) return;
  int code = http.GET();
  String body = code == 200 ? http.getString() : String("");
  http.end();
  if (code != 200) return;

  int start = 0;
  while (start < (int)body.length()) {
    int end = body.indexOf('\n', start);
    if (end < 0) end = body.length();
    String line = body.substring(start, end);
    start = end + 1;

    String id = jsonString(line, "id");
    if (id.length() == 0 || jsonString(line, "event") != "message") continue;
    applyCommand(jsonString(line, "message"), (time_t)jsonNumber(line, "time"));
    lastCmdId = id;
    saveString("cmdid", id);
  }
}

// ---------------------------------------------------------------- status page

static String formatClock(int hour, int minute) {
  char buf[12];
  int h12 = hour % 12 == 0 ? 12 : hour % 12;
  snprintf(buf, sizeof(buf), "%d:%02d %s", h12, minute, hour < 12 ? "AM" : "PM");
  return String(buf);
}

// A settings clock time (minutes after midnight) for display, or "off".
static String clockText(int32_t clock) {
  return clock < 0 ? String("off") : formatClock(clock / 60, clock % 60);
}

static String formatDay(int32_t day) {
  if (day < 0) return String("never");
  static const char *WD[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *MO[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int y; unsigned m, d;
  civilFromDays(day, y, m, d);
  char buf[24];
  snprintf(buf, sizeof(buf), "%s %s %u, %d", WD[((day % 7) + 11) % 7], MO[m - 1], d, y);
  return String(buf);
}

static const char *statusKey() {
  int a;
  switch (currentPhase(a)) {
    case PH_TAKEN: return "taken";
    case PH_PENDING: return "pending";
    case PH_DUE: return "due";
    case PH_LATE: return "late";
    case PH_MISSED: return "missed";
    default: return "waiting_for_clock";
  }
}

static const char *statusText() {
  int a;
  switch (currentPhase(a)) {
    case PH_TAKEN: return "Green: taken today";
    case PH_PENDING: return "Blue: not yet today";
    case PH_DUE: return "Amber: med time, take your meds";
    case PH_LATE: return "Red: late, take your meds";
    case PH_MISSED: return "Solid red: missed yesterday (mute from your phone)";
    default: return "White: waiting for the clock";
  }
}

static String reminderState(int i) {
  int32_t today = medDay();
  if (today < 0) return String("unknown");
  if (nightSentDay[i] == today) return String("sent");
  if (lastTakenDay == today) return String("not needed");
  return String("pending");
}

static String nowText() {
  if (!clockValid()) return String("clock not set");
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%a %b %d, %I:%M %p", &t);
  return String(buf);
}

static void handleApiStatus() {
  String j = "{";
  j += "\"status\":\""; j += statusKey(); j += "\",";
  j += "\"light\":\""; j += statusText(); j += "\",";
  j += "\"now\":\""; j += nowText(); j += "\",";
  j += "\"today\":\""; j += formatDay(medDay()); j += "\",";
  j += "\"last_taken\":\""; j += formatDay(lastTakenDay); j += "\",";
  j += "\"day_starts\":\""; j += formatClock(cfg.dayStart, 0); j += "\",";
  j += "\"med_time\":\""; j += clockText(cfg.med); j += "\",";
  j += "\"late_at\":\""; j += clockText(cfg.late); j += "\",";
  j += "\"med_notified_today\":"; j += (medSentDay == medDay() && medDay() >= 0) ? "true" : "false"; j += ",";
  j += "\"night_reminders\":[";
  bool first = true;
  for (int i = 0; i < NIGHT_MAX; i++) {
    if (cfg.night[i] < 0) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"time\":\""; j += clockText(cfg.night[i]);
    j += "\",\"today\":\""; j += reminderState(i); j += "\"}";
  }
  j += "],";
  j += "\"missed_alert\":{\"starts\":\""; j += clockText(cfg.morning);
  j += "\",\"repeat_minutes\":"; j += String(cfg.followupMin);
  j += ",\"muted_today\":"; j += missAcknowledged() ? "true" : "false"; j += "},";
  j += "\"wifi_network\":\""; j += WiFi.SSID(); j += "\",";
  j += "\"wifi_rssi\":"; j += String(WiFi.RSSI()); j += ",";
  j += "\"uptime_minutes\":"; j += String((unsigned long)(millis() / 60000));
  j += "}";
  server.send(200, "application/json", j);
}

static void handleRoot() {
  String h;
  h.reserve(4000);
  h += "<!doctype html><html><head><meta charset='utf-8'>"
       "<meta name='viewport' content='width=device-width,initial-scale=1'>"
       "<meta http-equiv='refresh' content='60'><title>Meds</title><link rel='apple-touch-icon' href='/apple-touch-icon.png'><meta name='apple-mobile-web-app-title' content='Meds'><style>"
       "body{font:16px -apple-system,system-ui,sans-serif;margin:0 auto;max-width:520px;padding:16px;"
       "color:#1d1d1f;background:#fafafa}"
       "@media(prefers-color-scheme:dark){body{color:#eee;background:#111}.card{background:#1c1c1e!important}}"
       "h1{font-size:22px;margin:4px 0 12px}h2{font-size:16px;margin:20px 0 6px;opacity:.7}"
       ".card{background:#fff;border-radius:12px;padding:14px;margin:8px 0}"
       "table{width:100%;border-collapse:collapse}td{padding:6px 4px;vertical-align:top;"
       "border-top:1px solid rgba(128,128,128,.2)}tr:first-child td{border-top:0}"
       "td:first-child{white-space:nowrap;padding-right:12px;opacity:.75}"
       ".big{font-size:18px;font-weight:600}"
       ".btn{width:100%;margin-top:10px;padding:12px;border:0;border-radius:10px;font-size:16px;font-weight:600;"
       "background:#1f9d55;color:#fff}.alt{background:#8e8e93}"
       "</style></head><body><h1>Med Button</h1>";

  h += "<div class='card'><div class='big'>"; h += statusText(); h += "</div>";
  h += "<table><tr><td>Now</td><td>"; h += nowText(); h += "</td></tr>";
  h += "<tr><td>Last taken</td><td>"; h += formatDay(lastTakenDay); h += "</td></tr>";
  h += "<tr><td>Wi-Fi</td><td>"; h += WiFi.SSID(); h += "</td></tr></table>";
  if (currentStatus() != TAKEN && currentStatus() != WAITING)
    h += "<form method='post' action='/api/taken'><button class='btn'>Mark taken</button></form>";
  if (currentStatus() == MISSED && !missAcknowledged())
    h += "<form method='post' action='/api/mute'><button class='btn alt'>Mute missed-dose alerts</button></form>";
  h += "</div>";

  h += "<h2>Today's reminders</h2><div class='card'><table>";
  if (cfg.med >= 0) {
    int32_t today = medDay();
    h += "<tr><td>"; h += clockText(cfg.med);
    h += "</td><td>Med time: amber light + notification (";
    h += today < 0 ? "unknown" : medSentDay == today ? "sent" : lastTakenDay == today ? "not needed" : "pending";
    h += ")</td></tr>";
  }
  if (cfg.late >= 0) {
    h += "<tr><td>"; h += clockText(cfg.late); h += "</td><td>Late: red light</td></tr>";
  }
  for (int i = 0; i < NIGHT_MAX; i++) {
    if (cfg.night[i] < 0) continue;
    h += "<tr><td>"; h += clockText(cfg.night[i]);
    h += "</td><td>Reminder notification ("; h += reminderState(i); h += ")</td></tr>";
  }
  if (cfg.morning >= 0) {
    h += "<tr><td>"; h += clockText(cfg.morning);
    h += "</td><td>If yesterday was missed: alert every "; h += String(cfg.followupMin);
    h += " min until muted"; if (missAcknowledged()) h += " (muted today)";
    h += "</td></tr>";
  }
  h += "</table></div>";

  h += "<h2>Light</h2><div class='card'><table>";
  h += "<tr><td>Green, " + (cfg.calmSolid ? String("solid") : "every " + String(cfg.greenSec) + " s") + "</td><td>Taken today</td></tr>";
  h += "<tr><td>Blue, " + (cfg.calmSolid ? String("solid") : "every " + String(cfg.blueSec) + " s") + "</td><td>Not yet today, nothing wrong</td></tr>";
  h += "<tr><td>Amber, every " + String(cfg.amberSec) + " s</td><td>Med time (" + clockText(cfg.med) + "), not taken yet</td></tr>";
  h += "<tr><td>Red, " + (cfg.redSolid ? String("solid") : "every " + String(cfg.redSec) + " s") + "</td><td>Late (" + clockText(cfg.late) + "), not taken yet</td></tr>";
  h += "<tr><td>Solid red</td><td>Missed yesterday; until muted or med time</td></tr>";
  h += "<tr><td>10 quick flashes</td><td>When amber/red starts, then every " + String(cfg.burstMin) + " min</td></tr>";
  h += "<tr><td>White, breathing</td><td>Starting up / waiting for the clock</td></tr>"
       "<tr><td>Magenta blips</td><td>No Wi-Fi; phone features paused</td></tr>"
       "<tr><td>Purple</td><td>Wi-Fi setup mode</td></tr></table></div>";

  h += "<h2>Button</h2><div class='card'><table>"
       "<tr><td>Hold 1 s</td><td>Mark today as taken</td></tr>"
       "<tr><td>Double-tap</td><td>While green: undo today's press</td></tr>"
       "<tr><td>Hold at power-up</td><td>Open Wi-Fi setup</td></tr></table></div>";

  h += "<h2>From your phone</h2><div class='card'><table>"
       "<tr><td>Shortcut</td><td>\"Took meds\" marks today as taken from anywhere</td></tr>"
       "<tr><td>Took it</td><td>Button on the night reminders</td></tr>"
       "<tr><td>Mute</td><td>Button on the missed-dose alerts</td></tr>"
       "<tr><td>Delay</td><td>Up to 30 s for the light to update</td></tr></table></div>";

  h += "<h2>Schedule</h2><div class='card'><table><tr><td>Day starts</td><td>";
  h += formatClock(cfg.dayStart, 0);
  h += " (a press before then counts for the previous day)</td></tr></table>";
  h += "<a class='btn alt' style='display:block;text-align:center;text-decoration:none' href='/settings'>Settings</a></div>";

  h += "<p style='opacity:.5;font-size:13px'>JSON: <a href='/api/status'>/api/status</a></p></body></html>";
  server.send(200, "text/html", h);
}

// Local buttons on the status page (home Wi-Fi only).
static void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

static void handleTakenPost() {
  if (recordTaken(medDay())) requestFeedback(FB_TAKEN);
  redirectHome();
}

static void handleMutePost() {
  if (currentStatus() == MISSED && !missAcknowledged()) acknowledgeMiss();
  redirectHome();
}

// ---------------------------------------------------------------- settings page

static String timeValue(int32_t clock) {  // for <input type=time>
  if (clock < 0) return String("");
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", (int)(clock / 60), (int)(clock % 60));
  return String(buf);
}

// "HH:MM" -> minutes after midnight; blank -> -1 (off); bad -> -2.
static int32_t parseTime(const String &v) {
  if (v.length() == 0) return -1;
  int h, m;
  if (sscanf(v.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return -2;
  return h * 60 + m;
}

static bool parseNumber(const char *name, int lo, int hi, int32_t &out) {
  String v = server.arg(name);
  if (v.length() == 0) return false;
  long n = strtol(v.c_str(), nullptr, 10);
  if (n < lo || n > hi) return false;
  out = n;
  return true;
}

static String settingsRow(const char *label, const char *name, const String &input, const char *hint) {
  String r = "<label><span>"; r += label; r += "</span>"; r += input; r += "</label>";
  if (hint[0]) { r += "<div class='hint'>"; r += hint; r += "</div>"; }
  (void)name;
  return r;
}

static String timeInput(const char *name, int32_t v) {
  return String("<input type='time' name='") + name + "' value='" + timeValue(v) + "'>";
}

// Flashing / Solid picker. Hides the matching interval rows when Solid.
static String modeSelect(const char *name, int32_t solid) {
  String r = String("<select name='") + name + "' onchange='modes()'>";
  r += String("<option value='0'") + (solid ? "" : " selected") + ">Flashing</option>";
  r += String("<option value='1'") + (solid ? " selected" : "") + ">Solid</option></select>";
  return r;
}

static String numberInput(const char *name, int32_t v, int lo, int hi) {
  return String("<input type='number' inputmode='numeric' name='") + name + "' value='" + String(v) +
         "' min='" + String(lo) + "' max='" + String(hi) + "'>";
}

static void sendSettingsPage(const String &message) {
  String h;
  h.reserve(6000);
  h += "<!doctype html><html><head><meta charset='utf-8'>"
       "<meta name='viewport' content='width=device-width,initial-scale=1'>"
       "<title>Meds settings</title><link rel='apple-touch-icon' href='/apple-touch-icon.png'><style>"
       "body{font:16px -apple-system,system-ui,sans-serif;margin:0 auto;max-width:520px;padding:16px;"
       "color:#1d1d1f;background:#fafafa}"
       "@media(prefers-color-scheme:dark){body{color:#eee;background:#111}.card{background:#1c1c1e!important}"
       "input,select{background:#2c2c2e;color:#eee}}"
       "h1{font-size:22px;margin:4px 0 12px}h2{font-size:16px;margin:20px 0 6px;opacity:.7}"
       ".card{background:#fff;border-radius:12px;padding:6px 14px;margin:8px 0}"
       "label{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:10px 0;"
       "border-top:1px solid rgba(128,128,128,.2)}.card label:first-child{border-top:0}"
       "#calmrows label,#redrows label{border-top:1px solid rgba(128,128,128,.2)}"
       "input,select{font-size:16px;padding:6px 8px;border:1px solid rgba(128,128,128,.4);border-radius:8px;"
       "width:120px;text-align:right}"
       ".hint{font-size:13px;opacity:.6;margin:-6px 0 8px}"
       ".btn{width:100%;margin-top:12px;padding:12px;border:0;border-radius:10px;font-size:16px;font-weight:600;"
       "background:#1f9d55;color:#fff}.alt{background:#8e8e93}"
       ".msg{padding:10px 14px;border-radius:10px;background:#fff3cd;color:#664d03;margin:8px 0}"
       "a{color:inherit}</style></head><body><p><a href='/'>&larr; Status</a></p><h1>Settings</h1>";
  if (message.length()) { h += "<div class='msg'>"; h += message; h += "</div>"; }

  h += "<form method='post' action='/settings'>";

  h += "<h2>Evening</h2><div class='card'>";
  h += settingsRow("Med time (amber)", "med", timeInput("med", cfg.med), "Light turns amber and your phone gets a notification.");
  h += settingsRow("Late (red)", "late", timeInput("late", cfg.late), "Light turns red. Must be after med time.");
  h += "</div>";

  h += "<h2>Reminder notifications</h2><div class='card'>";
  const char *labels[NIGHT_MAX] = {"Reminder 1", "Reminder 2", "Reminder 3", "Reminder 4"};
  const char *names[NIGHT_MAX] = {"n0", "n1", "n2", "n3"};
  for (int i = 0; i < NIGHT_MAX; i++)
    h += settingsRow(labels[i], names[i], timeInput(names[i], cfg.night[i]), i == 0 ? "\"Still not taken\" notifications. Leave blank for off." : "");
  h += "</div>";

  h += "<h2>Missed yesterday</h2><div class='card'>";
  h += settingsRow("Alerts start", "morning", timeInput("morning", cfg.morning), "Leave blank for off.");
  h += settingsRow("Repeat every (min)", "followup", numberInput("followup", cfg.followupMin, 5, 720), "");
  h += "</div>";

  h += "<h2>Light: green &amp; blue</h2><div class='card' style='border-left:5px solid #34c759'>";
  h += settingsRow("Mode", "calm", modeSelect("calm", cfg.calmSolid), "Green = taken today. Blue = not yet today.");
  h += "<div id='calmrows'>";
  h += settingsRow("Green flash every (s)", "green", numberInput("green", cfg.greenSec, 1, 600), "");
  h += settingsRow("Blue flash every (s)", "blue", numberInput("blue", cfg.blueSec, 1, 600), "");
  h += "</div>";
  h += settingsRow("Brightness", "bright", numberInput("bright", cfg.brightness, 1, 255), "1&ndash;255");
  h += "</div>";

  h += "<h2>Light: amber</h2><div class='card' style='border-left:5px solid #ff9f0a'>";
  h += settingsRow("Flash every (s)", "amber", numberInput("amber", cfg.amberSec, 1, 60), "Med time. Amber always flashes.");
  h += "</div>";

  h += "<h2>Light: red</h2><div class='card' style='border-left:5px solid #ff3b30'>";
  h += settingsRow("Mode", "red", modeSelect("red", cfg.redSolid), "Late. Missed-yesterday red is always solid.");
  h += "<div id='redrows'>";
  h += settingsRow("Flash every (s)", "redsec", numberInput("redsec", cfg.redSec, 1, 60), "");
  h += "</div>";
  h += "</div>";

  h += "<h2>Light: amber &amp; red</h2><div class='card'>";
  h += settingsRow("10 quick flashes every (min)", "burst", numberInput("burst", cfg.burstMin, 1, 120), "While amber or red, flashing or solid.");
  h += settingsRow("Brightness", "redbright", numberInput("redbright", cfg.redBrightness, 1, 255), "1&ndash;255");
  h += "</div>";

  h += "<h2>Day</h2><div class='card'>";
  h += settingsRow("New day starts at (hour)", "daystart", numberInput("daystart", cfg.dayStart, 0, 6),
                   "0&ndash;6 (3 = 3am). A press before this counts for the previous day.");
  h += "</div>";

  h += "<button class='btn'>Save</button></form>";
  // Solid greys out the interval rows instead of hiding them, so nothing jumps.
  h += "<script>function dim(id,on){var d=document.getElementById(id);d.style.opacity=on?.35:1;"
       "d.querySelectorAll('input').forEach(function(i){i.readOnly=on;});}"
       "function modes(){var f=document.forms[0];dim('calmrows',f.calm.value=='1');"
       "dim('redrows',f.red.value=='1');}modes();</script>";
  h += "<form method='post' action='/settings/reset' onsubmit=\"return confirm('Reset all settings to defaults?')\">"
       "<button class='btn alt'>Reset to defaults</button></form></body></html>";
  server.send(200, "text/html", h);
}

static void handleSettingsGet() { sendSettingsPage(""); }

static void handleSettingsPost() {
  Settings c = cfg;
  String err;

  struct { const char *name; int32_t *field; const char *label; } times[] = {
    {"med", &c.med, "Med time"}, {"late", &c.late, "Late"},
    {"n0", &c.night[0], "Reminder 1"}, {"n1", &c.night[1], "Reminder 2"},
    {"n2", &c.night[2], "Reminder 3"}, {"n3", &c.night[3], "Reminder 4"},
    {"morning", &c.morning, "Alerts start"},
  };
  for (auto &t : times) {
    int32_t v = parseTime(server.arg(t.name));
    if (v == -2) { err += t.label; err += " isn't a valid time. "; }
    else *t.field = v;
  }

  struct { const char *name; int lo, hi; int32_t *field; const char *label; } nums[] = {
    {"followup", 5, 720, &c.followupMin, "Repeat every"},
    {"green", 1, 600, &c.greenSec, "Green flash"},
    {"blue", 1, 600, &c.blueSec, "Blue flash"},
    {"amber", 1, 60, &c.amberSec, "Amber flash"},
    {"redsec", 1, 60, &c.redSec, "Red flash"},
    {"calm", 0, 1, &c.calmSolid, "Green/blue mode"},
    {"red", 0, 1, &c.redSolid, "Red mode"},
    {"burst", 1, 120, &c.burstMin, "Quick flashes"},
    {"bright", 1, 255, &c.brightness, "Green/blue brightness"},
    {"redbright", 1, 255, &c.redBrightness, "Amber/red brightness"},
    {"daystart", 0, 6, &c.dayStart, "New day starts"},
  };
  for (auto &n : nums) {
    if (!parseNumber(n.name, n.lo, n.hi, *n.field)) {
      err += n.label; err += " must be "; err += String(n.lo); err += "&ndash;"; err += String(n.hi); err += ". ";
    }
  }

  // Late must come after med time (counting from when the day starts).
  if (err.length() == 0 && c.med >= 0 && c.late >= 0) {
    int ds = c.dayStart * 60;
    if ((c.late - ds + 1440) % 1440 <= (c.med - ds + 1440) % 1440) err += "Late must be after med time. ";
  }

  if (err.length()) {
    sendSettingsPage(err + "Nothing was saved.");
    return;
  }
  saveSettings(c);
  Serial.println("Settings saved");
  sendSettingsPage("Saved.");
}

static void handleSettingsReset() {
  Settings c;
  defaultSettings(c);
  saveSettings(c);
  sendSettingsPage("Reset to defaults.");
}

static bool serverRunning = false;

static void handleServer() {
  bool shouldRun = WiFi.status() == WL_CONNECTED && !wm.getConfigPortalActive();
  if (shouldRun && !serverRunning) {
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    server.begin();
    serverRunning = true;
    Serial.printf("Status page: http://%s.local\n", HOSTNAME);
  } else if (!shouldRun && serverRunning) {
    server.stop();  // free port 80 for the Wi-Fi setup page
    MDNS.end();
    serverRunning = false;
  }
  if (serverRunning) server.handleClient();
}

// ---------------------------------------------------------------- Wi-Fi

// Two networks: "home" (picked on the setup page, saved in flash) and the
// phone hotspot from config.h. When disconnected we alternate between them.

#ifndef HOTSPOT_SSID
#define HOTSPOT_SSID ""
#define HOTSPOT_PASSWORD ""
#endif

// The home network is kept in our own storage, not the Wi-Fi chip's: the chip
// saves whatever network it last joined (including the hotspot), which would
// make it auto-join the wrong network and destabilize the setup page.
static String homeSsid, homePass;

static void loadHomeNetwork() {
  homeSsid = prefs.getString("wifi_ssid", "");
  homePass = prefs.getString("wifi_pass", "");
  Serial.printf("Home network: '%s'\n", homeSsid.c_str());
}

// Called by the setup page after you pick a network and it connects.
static void saveHomeNetwork() {
  homeSsid = wm.getWiFiSSID(true);
  homePass = wm.getWiFiPass(true);
  saveString("wifi_ssid", homeSsid);
  saveString("wifi_pass", homePass);
  Serial.printf("Saved home network: '%s'\n", homeSsid.c_str());
}

static void openSetup() {
  WiFi.disconnect();  // stop any connection attempt so the setup network is stable
  wm.startConfigPortal(SETUP_AP_NAME);
  Serial.println("Setup network open: MedButton-Setup");
}

static bool hasHotspot() { return strlen(HOTSPOT_SSID) > 0; }

// Connect without overwriting the saved home network in flash.
static void tryNetwork(const String &ssid, const String &pass) {
  Serial.printf("Trying Wi-Fi '%s'\n", ssid.c_str());
  WiFi.disconnect();
  WiFi.persistent(false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  WiFi.persistent(true);
}

static void handleWiFi() {
  static uint32_t lastAttempt = 0;
  static bool tryHotspotNext = false;
  wm.process();
  if (wm.getConfigPortalActive() || WiFi.status() == WL_CONNECTED) return;

  if (lastAttempt != 0 && millis() - lastAttempt < 20000) return;  // give each try 20 s
  lastAttempt = millis();

  bool haveHome = homeSsid.length() > 0;
  if (!haveHome) {
    // No home network yet: keep reopening the setup page (5 min each),
    // with a quick hotspot attempt in between.
    if (tryHotspotNext && hasHotspot()) tryNetwork(HOTSPOT_SSID, HOTSPOT_PASSWORD);
    else openSetup();
    tryHotspotNext = !tryHotspotNext;
    return;
  }
  if (haveHome && (!tryHotspotNext || !hasHotspot())) {
    tryNetwork(homeSsid, homePass);
  } else {
    tryNetwork(HOTSPOT_SSID, HOTSPOT_PASSWORD);
  }
  tryHotspotNext = !tryHotspotNext;
}

// ---------------------------------------------------------------- main

// Button and light, every 10 ms, independent of network work in loop().
static void uiTask(void *) {
  for (;;) {
    handleButton();
    if (!buttonBusy) {
      uint8_t f = feedbackRequest;
      if (f != FB_NONE) {
        feedbackRequest = FB_NONE;
        playFeedback(f);
      } else {
        drawStatus();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  stateLock = xSemaphoreCreateRecursiveMutex();
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  led.begin();
  led.setBrightness(255);  // brightness handled per color in show()
  loadState();
  show(255, 255, 255, cfg.brightness);
  loadCommandCursor();

  delay(50);
  bool forceSetup = digitalRead(BUTTON_PIN) == LOW;  // held while plugging in
  if (forceSetup) ignoreUntilRelease = true;

  configTzTime(TZ_STRING, "pool.ntp.org", "time.google.com");

  server.on("/", handleRoot);
  server.on("/api/status", handleApiStatus);
  server.on("/apple-touch-icon.png", [] {
    server.sendHeader("Cache-Control", "max-age=86400");
    server.send_P(200, "image/png", (const char *)ICON_PNG, ICON_PNG_LEN);
  });
  server.on("/api/taken", HTTP_POST, handleTakenPost);
  server.on("/api/mute", HTTP_POST, handleMutePost);
  server.on("/settings", HTTP_GET, handleSettingsGet);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/settings/reset", HTTP_POST, handleSettingsReset);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);  // forget whatever network the chip saved last
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  wm.setHostname(HOSTNAME);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(300);  // close setup page after 5 min idle
  wm.setConnectTimeout(20);
  wm.setTitle("Med Button");
  wm.setSaveConfigCallback(saveHomeNetwork);  // new home network picked on setup page

  loadHomeNetwork();
  if (forceSetup || homeSsid.length() == 0) {
    openSetup();  // first run, or button held at power-up
  }
  // Otherwise handleWiFi() connects to home, falling back to the hotspot.

  // Higher priority than loop() (1), so the button stays responsive.
  xTaskCreatePinnedToCore(uiTask, "ui", 4096, nullptr, 2, nullptr, 1);
}

// Network work only. These calls can block for a few seconds.
void loop() {
  if (WiFi.status() == WL_CONNECTED) lastOnlineMs = millis();
  handleWiFi();
  handleServer();
  handleNotifications();
  handleCommands();
  delay(10);
}
