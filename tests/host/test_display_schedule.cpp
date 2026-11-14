// Tests for services::display_schedule (src/services/display_schedule.cpp):
// HH:MM parsing, the off-window (incl. spanning midnight), wake/force-on,
// NVS persistence, the portal save path and the POSIX TZ table.
#include "test_framework.h"

// Pre-include everything the unit includes, then route its time() calls to a
// fake wall clock. The function-like macro only rewrites `time(` in the .cpp.
#include <Arduino.h>
#include <Preferences.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
time_t g_fake_now = 0;
}
static time_t fake_time(time_t* out) {
  if (out) *out = g_fake_now;
  return g_fake_now;
}
#define time(x) fake_time(x)
#include "../../src/services/display_schedule.cpp"
#undef time

namespace ds = services::display_schedule;

namespace {

constexpr time_t kSynced = 1767225600;  // 2026-01-01 00:00:00 UTC

/** Set the fake clock to an absolute UTC date/time. */
void setUtc(int year, int mon, int day, int hh, int mm) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = mon - 1;
  t.tm_mday = day;
  t.tm_hour = hh;
  t.tm_min = mm;
  g_fake_now = timegm(&t);
}

/** Set the clock to hh:mm UTC on 2026-01-01 (TZ is UTC0 unless changed). */
void setClock(int hh, int mm) { setUtc(2026, 1, 1, hh, mm); }

/** Power-cycle: reset RAM state but keep NVS contents. */
void rebootRuntime() {
  ds::s_enabled = false;
  ds::s_off_min = config::kDefaultDisplayOffMin;
  ds::s_on_min = config::kDefaultDisplayOnMin;
  ds::s_tz[0] = '\0';
  ds::s_off = false;
  ds::s_forced_on = false;
  ds::s_wake_active = false;
  ds::s_wake_until_ms = 0;
  tft = FakeTft{};
  fakearduino::g_millis = 1000;
  fakearduino::g_config_tz_calls = 0;
  g_fake_now = kSynced;
}

void resetModule() {
  fakenvs::reset();
  rebootRuntime();
}

std::string hhmm(uint16_t minutes) {
  char buf[8];
  ds::formatHhMm(buf, sizeof(buf), minutes);
  return buf;
}

/** Parse "(UTC+5:30)" style offsets from a timezone label, in seconds. */
long labelOffsetSeconds(const char* label) {
  const char* p = strstr(label, "(UTC");
  if (p == nullptr) return 0;  // plain "UTC"
  p += 4;
  if (*p != '+' && *p != '-') return 0;
  const int sign = *p == '-' ? -1 : 1;
  int h = 0;
  int m = 0;
  sscanf(p + 1, "%d:%d", &h, &m);
  return sign * (h * 3600L + m * 60L);
}

long gmtOffsetAt(time_t t) {
  struct tm local = {};
  localtime_r(&t, &local);
  return local.tm_gmtoff;
}

}  // namespace

SET_UP({ resetModule(); });

// ---------------- parsing / formatting ----------------

TEST(parse_hhmm_accepts_valid_forms) {
  uint16_t m = 0;
  CHECK(ds::parseHhMm("00:00", &m));
  CHECK_EQ(m, uint16_t{0});
  CHECK(ds::parseHhMm("7:05", &m));
  CHECK_EQ(m, uint16_t{425});
  CHECK(ds::parseHhMm("23:59", &m));
  CHECK_EQ(m, uint16_t{1439});
  CHECK(ds::parseHhMm("22:30:15", &m));  // browser <input type=time> seconds
  CHECK_EQ(m, uint16_t{1350});
}

TEST(parse_hhmm_rejects_invalid) {
  uint16_t m = 999;
  for (const char* s : {"", "7", "24:00", "12:60", "-1:00", "ab:cd", "12-30"}) {
    if (ds::parseHhMm(s, &m)) {
      testfw::fail(__FILE__, __LINE__, std::string("accepted \"") + s + "\"");
    }
  }
  CHECK(!ds::parseHhMm(nullptr, &m));
  CHECK_EQ(m, uint16_t{999});  // untouched on failure
}

TEST(format_hhmm_zero_pads) {
  CHECK_STREQ(hhmm(0), "00:00");
  CHECK_STREQ(hhmm(425), "07:05");
  CHECK_STREQ(hhmm(1439), "23:59");
}

TEST(format_now_reports_unsynced_clock) {
  g_fake_now = 1000;  // 1970: SNTP has not run yet
  char buf[32];
  ds::formatNow(buf, sizeof(buf));
  CHECK_STREQ(buf, "not synced yet");
}

TEST(format_now_shows_local_time_and_zone) {
  ds::init();
  setClock(22, 14);  // 2026-01-01 is a Thursday
  char buf[32];
  ds::formatNow(buf, sizeof(buf));
  CHECK_STREQ(buf, "Thu 22:14 UTC");
}

// ---------------- off window ----------------

TEST(window_same_day) {
  ds::s_off_min = 9 * 60;
  ds::s_on_min = 17 * 60;
  CHECK(!ds::inOffWindow(8 * 60 + 59));
  CHECK(ds::inOffWindow(9 * 60));       // off time is inclusive
  CHECK(ds::inOffWindow(16 * 60 + 59));
  CHECK(!ds::inOffWindow(17 * 60));     // on time is exclusive
}

TEST(window_spanning_midnight) {
  ds::s_off_min = 23 * 60;
  ds::s_on_min = 7 * 60;
  CHECK(!ds::inOffWindow(22 * 60 + 59));
  CHECK(ds::inOffWindow(23 * 60));
  CHECK(ds::inOffWindow(0));
  CHECK(ds::inOffWindow(6 * 60 + 59));
  CHECK(!ds::inOffWindow(7 * 60));
  CHECK(!ds::inOffWindow(12 * 60));
}

TEST(equal_times_disable_window) {
  ds::s_off_min = ds::s_on_min = 600;
  for (uint16_t m = 0; m < 1440; m += 7) CHECK(!ds::inOffWindow(m));
}

// ---------------- update() / backlight ----------------

TEST(disabled_schedule_never_turns_display_off) {
  ds::init();
  setClock(2, 0);
  CHECK(!ds::update());
  CHECK_EQ(tft.brightness_calls, 0);
}

TEST(enabled_schedule_turns_off_and_back_on) {
  ds::init();
  ds::saveFromPortal("T", "23:00", "07:00", "UTC0");
  setClock(22, 59);
  CHECK(!ds::update());
  setClock(23, 0);
  CHECK(ds::update());
  CHECK(ds::isOff());
  CHECK_EQ(tft.brightness, 0);
  setClock(3, 0);
  CHECK(ds::update());
  CHECK_EQ(tft.brightness_calls, 1);  // only switches on transitions
  setClock(7, 0);
  CHECK(!ds::update());
  CHECK(!ds::isOff());
  CHECK_EQ(tft.brightness, static_cast<int>(config::kDisplayBrightness));
  CHECK_EQ(tft.brightness_calls, 2);
}

TEST(unsynced_clock_keeps_display_on) {
  ds::init();
  ds::saveFromPortal("T", "00:00", "23:59", "UTC0");
  g_fake_now = 3600;  // 1970-01-01 01:00, inside the window but not synced
  CHECK(!ds::update());
}

TEST(window_uses_local_time_zone) {
  ds::init();
  // US Eastern: off 23:00-07:00 local. 2026-01-01 03:30 UTC = 22:30 EST.
  ds::saveFromPortal("T", "23:00", "07:00", "EST5EDT,M3.2.0,M11.1.0");
  setUtc(2026, 1, 1, 3, 30);
  CHECK(!ds::update());
  setUtc(2026, 1, 1, 4, 0);  // 23:00 EST
  CHECK(ds::update());
  // Summer: 2026-07-01 03:30 UTC = 23:30 EDT -> off.
  setUtc(2026, 7, 1, 3, 30);
  CHECK(ds::update());
  setUtc(2026, 7, 1, 11, 0);  // 07:00 EDT -> on
  CHECK(!ds::update());
}

TEST(wake_temporarily_overrides_for_wake_period) {
  ds::init();
  ds::saveFromPortal("T", "23:00", "07:00", "UTC0");
  setClock(1, 0);
  CHECK(ds::update());
  ds::wakeTemporarily();
  CHECK(!ds::update());
  CHECK_EQ(tft.brightness, static_cast<int>(config::kDisplayBrightness));
  fakearduino::g_millis += config::kDisplayWakeMs - 1;
  CHECK(!ds::update());
  fakearduino::g_millis += 1;
  CHECK(ds::update());
  CHECK_EQ(tft.brightness, 0);
}

TEST(wake_survives_millis_wraparound) {
  ds::init();
  ds::saveFromPortal("T", "23:00", "07:00", "UTC0");
  setClock(1, 0);
  fakearduino::g_millis = 0xFFFFFFFFUL - 1000;  // ~1 s before 32-bit rollover
  ds::wakeTemporarily();
  fakearduino::g_millis = static_cast<unsigned long>(
      static_cast<uint32_t>(fakearduino::g_millis + 5000));
  CHECK(!ds::update());  // still within the 60 s wake
}

TEST(force_on_overrides_schedule_until_reboot) {
  ds::init();
  ds::saveFromPortal("T", "23:00", "07:00", "UTC0");
  setClock(1, 0);
  CHECK(ds::update());
  ds::forceOn();
  CHECK(!ds::isOff());
  CHECK_EQ(tft.brightness, static_cast<int>(config::kDisplayBrightness));
  CHECK(!ds::update());
  setClock(2, 0);
  CHECK(!ds::update());
}

// ---------------- persistence / portal ----------------

TEST(init_defaults_with_empty_nvs) {
  ds::init();
  CHECK(!ds::enabled());
  CHECK_EQ(ds::offMinutes(), config::kDefaultDisplayOffMin);
  CHECK_EQ(ds::onMinutes(), config::kDefaultDisplayOnMin);
  CHECK_STREQ(ds::timezone(), config::kDefaultTimezone);
  CHECK_STREQ(fakearduino::g_tz, config::kDefaultTimezone);
  CHECK_STREQ(fakearduino::g_ntp1, config::kNtpServer1);
  CHECK_STREQ(fakearduino::g_ntp2, config::kNtpServer2);
}

TEST(save_from_portal_persists_across_reboot) {
  ds::init();
  ds::saveFromPortal("T", "22:15", "06:45", "CET-1CEST,M3.5.0,M10.5.0/3");
  rebootRuntime();
  ds::init();
  CHECK(ds::enabled());
  CHECK_EQ(ds::offMinutes(), uint16_t{22 * 60 + 15});
  CHECK_EQ(ds::onMinutes(), uint16_t{6 * 60 + 45});
  CHECK_STREQ(ds::timezone(), "CET-1CEST,M3.5.0,M10.5.0/3");
  CHECK_STREQ(fakearduino::g_tz, "CET-1CEST,M3.5.0,M10.5.0/3");
}

TEST(unchecked_checkbox_disables) {
  ds::init();
  ds::saveFromPortal("T", "22:00", "06:00", "UTC0");
  ds::saveFromPortal("", "22:00", "06:00", "UTC0");
  CHECK(!ds::enabled());
  ds::saveFromPortal(nullptr, "22:00", "06:00", "UTC0");
  CHECK(!ds::enabled());
}

TEST(invalid_times_keep_previous_values) {
  ds::init();
  ds::saveFromPortal("T", "21:00", "05:00", "UTC0");
  ds::saveFromPortal("T", "25:00", "garbage", "UTC0");
  CHECK_EQ(ds::offMinutes(), uint16_t{21 * 60});
  CHECK_EQ(ds::onMinutes(), uint16_t{5 * 60});
}

TEST(tz_is_trimmed_and_empty_keeps_previous) {
  ds::init();
  ds::saveFromPortal("T", "21:00", "05:00", "  JST-9 \t");
  CHECK_STREQ(ds::timezone(), "JST-9");
  const int calls = fakearduino::g_config_tz_calls;
  ds::saveFromPortal("T", "21:00", "05:00", "   ");
  CHECK_STREQ(ds::timezone(), "JST-9");
  ds::saveFromPortal("T", "21:00", "05:00", nullptr);
  CHECK_STREQ(ds::timezone(), "JST-9");
  ds::saveFromPortal("T", "21:00", "05:00", "JST-9");
  CHECK_EQ(fakearduino::g_config_tz_calls, calls);  // unchanged TZ: no re-config
}

TEST(overlong_tz_is_truncated_not_overflowed) {
  ds::init();
  const std::string long_tz(200, 'A');
  ds::saveFromPortal("T", "21:00", "05:00", long_tz.c_str());
  CHECK_EQ(strlen(ds::timezone()), ds::kTzMaxLen);
}

TEST(corrupt_nvs_minutes_fall_back_to_defaults) {
  Preferences p;
  p.begin("dispsched", false);
  p.putUShort("offMin", 5000);
  p.putUShort("onMin", 1440);
  p.putString("tz", "");
  p.end();
  ds::init();
  CHECK_EQ(ds::offMinutes(), config::kDefaultDisplayOffMin);
  CHECK_EQ(ds::onMinutes(), config::kDefaultDisplayOnMin);
  CHECK_STREQ(ds::timezone(), config::kDefaultTimezone);
}

TEST(clear_resets_runtime_and_nvs) {
  ds::init();
  ds::saveFromPortal("T", "20:00", "04:00", "JST-9");
  ds::clear();
  CHECK(!ds::enabled());
  CHECK_STREQ(ds::timezone(), config::kDefaultTimezone);
  rebootRuntime();
  ds::init();
  CHECK(!ds::enabled());
  CHECK_EQ(ds::offMinutes(), config::kDefaultDisplayOffMin);
  CHECK_STREQ(ds::timezone(), config::kDefaultTimezone);
}

// ---------------- timezone table ----------------

TEST(timezone_table_entries_parse_and_match_labels) {
  CHECK(ds::kTimezoneCount > 0);
  for (size_t i = 0; i < ds::kTimezoneCount; ++i) {
    const auto& tz = ds::kTimezones[i];
    CHECK(strlen(tz.posix) <= ds::kTzMaxLen);
    configTzTime(tz.posix, "x");
    // Standard offset = the smaller of mid-January and mid-July offsets
    // (DST only ever adds time, in either hemisphere).
    const long jan = gmtOffsetAt(1768478400);  // 2026-01-15 12:00 UTC
    const long jul = gmtOffsetAt(1784116800);  // 2026-07-15 12:00 UTC
    const long standard = jan < jul ? jan : jul;
    const long expected = labelOffsetSeconds(tz.label);
    if (standard != expected) {
      testfw::fail(__FILE__, __LINE__,
                   std::string(tz.label) + " / " + tz.posix + ": standard offset " +
                       std::to_string(standard) + "s, label says " +
                       std::to_string(expected) + "s");
    }
  }
}

TEST(timezone_table_is_sorted_west_to_east) {
  long prev = -24 * 3600;
  for (size_t i = 0; i < ds::kTimezoneCount; ++i) {
    const long off = labelOffsetSeconds(ds::kTimezones[i].label);
    if (off < prev) {
      testfw::fail(__FILE__, __LINE__,
                   std::string("out of order: ") + ds::kTimezones[i].label);
    }
    prev = off;
  }
}

TEST(timezone_labels_are_unique) {
  for (size_t i = 0; i < ds::kTimezoneCount; ++i) {
    for (size_t j = i + 1; j < ds::kTimezoneCount; ++j) {
      CHECK(strcmp(ds::kTimezones[i].label, ds::kTimezones[j].label) != 0);
    }
  }
}

TEST(tz_with_html_metacharacters_is_rejected) {
  ds::init();
  ds::saveFromPortal("T", "21:00", "05:00", "JST-9");
  // Would break out of value='...' in the portal form (stored XSS).
  ds::saveFromPortal("T", "21:00", "05:00", "x' autofocus onfocus=alert(1) x='");
  CHECK_STREQ(ds::timezone(), "JST-9");
  ds::saveFromPortal("T", "21:00", "05:00", "EST5\"EDT");
  CHECK_STREQ(ds::timezone(), "JST-9");
  ds::saveFromPortal("T", "21:00", "05:00", "A&B5");
  CHECK_STREQ(ds::timezone(), "JST-9");
  CHECK(fakearduino::g_serial.find("Invalid characters") != std::string::npos);
}

TEST(valid_posix_tz_forms_are_accepted) {
  ds::init();
  for (const char* tz : {"<+0530>-5:30", "EST5EDT,M3.2.0,M11.1.0",
                         "CET-1CEST,M3.5.0,M10.5.0/3", "UTC0"}) {
    ds::saveFromPortal("T", "21:00", "05:00", tz);
    CHECK_STREQ(ds::timezone(), tz);
  }
  for (size_t i = 0; i < ds::kTimezoneCount; ++i) {
    ds::saveFromPortal("T", "21:00", "05:00", ds::kTimezones[i].posix);
    CHECK_STREQ(ds::timezone(), ds::kTimezones[i].posix);
  }
}

TEST(tampered_nvs_tz_falls_back_to_default) {
  Preferences p;
  p.begin("dispsched", false);
  p.putString("tz", "UTC0'><script>");
  p.end();
  ds::init();
  CHECK_STREQ(ds::timezone(), config::kDefaultTimezone);
}
