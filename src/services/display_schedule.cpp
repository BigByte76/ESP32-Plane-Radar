#include "services/display_schedule.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "hardware/display.h"

namespace services::display_schedule {

// POSIX TZ strings (DST rules included). "<+08>-8" style names are numeric
// abbreviations for zones without a common one. Sign is inverted: UTC-5 = "EST5".
const TimezoneOption kTimezones[] = {
    {"Hawaii (UTC-10)", "HST10"},
    {"Alaska (UTC-9)", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US & Canada Pacific (UTC-8)", "PST8PDT,M3.2.0,M11.1.0"},
    {"Arizona (UTC-7, no DST)", "MST7"},
    {"US & Canada Mountain (UTC-7)", "MST7MDT,M3.2.0,M11.1.0"},
    {"US & Canada Central (UTC-6)", "CST6CDT,M3.2.0,M11.1.0"},
    {"Mexico City (UTC-6)", "CST6"},
    {"US & Canada Eastern (UTC-5)", "EST5EDT,M3.2.0,M11.1.0"},
    {"Canada Atlantic (UTC-4)", "AST4ADT,M3.2.0,M11.1.0"},
    {"Newfoundland (UTC-3:30)", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"Brazil (Brasilia), Argentina (UTC-3)", "<-03>3"},
    {"UTC", "UTC0"},
    {"UK & Ireland (UTC+0)", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Portugal, Canary Islands (UTC+0)", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Central Europe (UTC+1)", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe (UTC+2)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"South Africa (UTC+2)", "SAST-2"},
    {"Turkey (UTC+3)", "<+03>-3"},
    {"Moscow (UTC+3)", "MSK-3"},
    {"Gulf / UAE (UTC+4)", "<+04>-4"},
    {"Pakistan (UTC+5)", "PKT-5"},
    {"India (UTC+5:30)", "IST-5:30"},
    {"Bangladesh (UTC+6)", "<+06>-6"},
    {"Thailand, Vietnam, Jakarta (UTC+7)", "<+07>-7"},
    {"China, Singapore, Perth (UTC+8)", "<+08>-8"},
    {"Japan, Korea (UTC+9)", "JST-9"},
    {"Darwin (UTC+9:30)", "ACST-9:30"},
    {"Adelaide (UTC+9:30)", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Brisbane (UTC+10)", "AEST-10"},
    {"Sydney, Melbourne, Hobart (UTC+10)", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"New Zealand (UTC+12)", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
const size_t kTimezoneCount = sizeof(kTimezones) / sizeof(kTimezones[0]);

namespace {

/** Own namespace (not "planeradar") so it never shares a handle with rangeInit. */
constexpr char kPrefsNamespace[] = "dispsched";
constexpr char kPrefsEnabledKey[] = "enabled";
constexpr char kPrefsOffKey[] = "offMin";
constexpr char kPrefsOnKey[] = "onMin";
constexpr char kPrefsTzKey[] = "tz";
constexpr size_t kTzMaxLen = 63;
constexpr uint16_t kMinutesPerDay = 24 * 60;
/** Any epoch before this means SNTP has not set the clock yet. */
constexpr time_t kMinValidEpoch = 1700000000;  // Nov 2023

bool s_enabled = false;
uint16_t s_off_min = config::kDefaultDisplayOffMin;
uint16_t s_on_min = config::kDefaultDisplayOnMin;
char s_tz[kTzMaxLen + 1] = {};

/** Written on the loop task, read by the ADS-B task. */
volatile bool s_off = false;
bool s_forced_on = false;
bool s_wake_active = false;
unsigned long s_wake_until_ms = 0;

void applyTimezone() {
  configTzTime(s_tz, config::kNtpServer1, config::kNtpServer2);
}

bool clockValid() { return time(nullptr) >= kMinValidEpoch; }

bool checkboxChecked(const char* value) {
  // WiFiManager posts the input's value ("T") when ticked, "" when not.
  return value != nullptr && value[0] != '\0';
}

/** POSIX TZ strings only need letters, digits and <>+-,.:/ ("<+0530>-5:30",
 *  "EST5EDT,M3.2.0,M11.1.0"). Anything else is refused: the value is echoed
 *  back into the portal form, so a quote or '&' would be an HTML injection. */
bool timezoneCharsValid(const char* s, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (!isalnum(c) && strchr("<>+-,.:/", c) == nullptr) {
      return false;
    }
  }
  return true;
}

/** Parse "H:MM" / "HH:MM" (browser time inputs may add ":SS"). */
bool parseHhMm(const char* s, uint16_t* out_min) {
  if (s == nullptr) {
    return false;
  }
  int h = -1;
  int m = -1;
  if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
    return false;
  }
  *out_min = static_cast<uint16_t>(h * 60 + m);
  return true;
}

/** True when local time-of-day falls inside the [off, on) window. */
bool inOffWindow(uint16_t now_min) {
  if (s_off_min == s_on_min) {
    return false;
  }
  if (s_off_min < s_on_min) {
    return now_min >= s_off_min && now_min < s_on_min;
  }
  // Window spans midnight, e.g. 23:00 -> 07:00.
  return now_min >= s_off_min || now_min < s_on_min;
}

bool scheduleWantsOff() {
  if (!s_enabled || s_forced_on || !clockValid()) {
    return false;
  }
  const time_t now = time(nullptr);
  struct tm local = {};
  localtime_r(&now, &local);
  return inOffWindow(static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min));
}

void setBacklight(bool on) {
  tft.setBrightness(on ? config::kDisplayBrightness : 0);
}

void save() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsEnabledKey, s_enabled);
  prefs.putUShort(kPrefsOffKey, s_off_min);
  prefs.putUShort(kPrefsOnKey, s_on_min);
  prefs.putString(kPrefsTzKey, s_tz);
  prefs.end();
}

}  // namespace

void init() {
  strncpy(s_tz, config::kDefaultTimezone, kTzMaxLen);
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    s_enabled = prefs.getBool(kPrefsEnabledKey, false);
    const uint16_t off = prefs.getUShort(kPrefsOffKey, config::kDefaultDisplayOffMin);
    const uint16_t on = prefs.getUShort(kPrefsOnKey, config::kDefaultDisplayOnMin);
    s_off_min = off < kMinutesPerDay ? off : config::kDefaultDisplayOffMin;
    s_on_min = on < kMinutesPerDay ? on : config::kDefaultDisplayOnMin;
    if (prefs.isKey(kPrefsTzKey)) {
      prefs.getString(kPrefsTzKey, s_tz, sizeof(s_tz));
    }
    prefs.end();
  }
  if (s_tz[0] == '\0' || !timezoneCharsValid(s_tz, strlen(s_tz))) {
    strncpy(s_tz, config::kDefaultTimezone, kTzMaxLen);
  }
  // SNTP keeps retrying in the background, so this is safe before Wi-Fi is up.
  applyTimezone();

  char off_buf[6];
  char on_buf[6];
  formatHhMm(off_buf, sizeof(off_buf), s_off_min);
  formatHhMm(on_buf, sizeof(on_buf), s_on_min);
  Serial.printf("Display schedule: %s, off %s on %s, TZ %s\n",
                s_enabled ? "enabled" : "disabled", off_buf, on_buf, s_tz);
}

bool update() {
  bool want_off = scheduleWantsOff();
  if (s_wake_active) {
    if (static_cast<long>(millis() - s_wake_until_ms) < 0) {
      want_off = false;
    } else {
      s_wake_active = false;
    }
  }
  if (want_off != s_off) {
    s_off = want_off;
    setBacklight(!want_off);
    Serial.printf("Display %s (schedule)\n", want_off ? "off" : "on");
  }
  return s_off;
}

bool isOff() { return s_off; }

void wakeTemporarily() {
  s_wake_active = true;
  s_wake_until_ms = millis() + config::kDisplayWakeMs;
}

void forceOn() {
  s_forced_on = true;
  s_off = false;
  setBacklight(true);
}

bool enabled() { return s_enabled; }
uint16_t offMinutes() { return s_off_min; }
uint16_t onMinutes() { return s_on_min; }
const char* timezone() { return s_tz; }

void formatHhMm(char* buf, size_t len, uint16_t minutes) {
  snprintf(buf, len, "%02u:%02u", static_cast<unsigned>(minutes / 60),
           static_cast<unsigned>(minutes % 60));
}

void formatNow(char* buf, size_t len) {
  if (!clockValid()) {
    snprintf(buf, len, "not synced yet");
    return;
  }
  const time_t now = time(nullptr);
  struct tm local = {};
  localtime_r(&now, &local);
  strftime(buf, len, "%a %H:%M %Z", &local);
}

void saveFromPortal(const char* enabled_checkbox, const char* off_hhmm,
                    const char* on_hhmm, const char* tz) {
  s_enabled = checkboxChecked(enabled_checkbox);

  uint16_t parsed = 0;
  if (parseHhMm(off_hhmm, &parsed)) {
    s_off_min = parsed;
  } else {
    Serial.println("Invalid display-off time in portal, keeping previous");
  }
  if (parseHhMm(on_hhmm, &parsed)) {
    s_on_min = parsed;
  } else {
    Serial.println("Invalid display-on time in portal, keeping previous");
  }

  // Trim surrounding whitespace from the TZ string.
  if (tz != nullptr) {
    while (isspace(static_cast<unsigned char>(*tz))) {
      ++tz;
    }
    size_t n = strnlen(tz, kTzMaxLen);
    while (n > 0 && isspace(static_cast<unsigned char>(tz[n - 1]))) {
      --n;
    }
    char trimmed[kTzMaxLen + 1];
    memcpy(trimmed, tz, n);
    trimmed[n] = '\0';
    if (n > 0 && !timezoneCharsValid(trimmed, n)) {
      Serial.println("Invalid characters in portal time zone, keeping previous");
    } else if (n > 0 && strcmp(trimmed, s_tz) != 0) {
      memcpy(s_tz, trimmed, n + 1);
      applyTimezone();
    }
  }

  save();
  char off_buf[6];
  char on_buf[6];
  formatHhMm(off_buf, sizeof(off_buf), s_off_min);
  formatHhMm(on_buf, sizeof(on_buf), s_on_min);
  Serial.printf("Display schedule: %s, off %s on %s, TZ %s\n",
                s_enabled ? "enabled" : "disabled", off_buf, on_buf, s_tz);
}

void clear() {
  s_enabled = false;
  s_off_min = config::kDefaultDisplayOffMin;
  s_on_min = config::kDefaultDisplayOnMin;
  strncpy(s_tz, config::kDefaultTimezone, kTzMaxLen);
  s_tz[kTzMaxLen] = '\0';
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
}

}  // namespace services::display_schedule
