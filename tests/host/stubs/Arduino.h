// Host stub for the Arduino core: fake clock, Serial sink, String, SNTP/TZ.
#pragma once

#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

// ---- Fake monotonic clock ----
namespace fakearduino {
extern unsigned long g_millis;
/** Last TZ / NTP servers passed to configTzTime(). */
extern std::string g_tz;
extern std::string g_ntp1;
extern std::string g_ntp2;
extern int g_config_tz_calls;
/** Everything printed to Serial since the last reset. */
extern std::string g_serial;
}  // namespace fakearduino

/** State of the fake esp_random() (xorshift32); tests may reseed it. */
namespace fakearduino {
extern uint32_t g_random_state;
extern int g_random_calls;
}  // namespace fakearduino

inline unsigned long millis() { return fakearduino::g_millis; }
inline uint32_t esp_random() {
  ++fakearduino::g_random_calls;
  uint32_t x = fakearduino::g_random_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  fakearduino::g_random_state = x;
  return x;
}
inline void delay(unsigned long ms) { fakearduino::g_millis += ms; }

// ---- Serial ----
class FakeSerial {
 public:
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fakearduino::g_serial += buf;
    return n;
  }
  size_t println(const char* s) {
    fakearduino::g_serial += s;
    fakearduino::g_serial += "\n";
    return strlen(s) + 1;
  }
  size_t print(const char* s) {
    fakearduino::g_serial += s;
    return strlen(s);
  }
};
extern FakeSerial Serial;

// ---- String (subset used by the firmware under test) ----
class String {
 public:
  String() = default;
  String(const char* s) : s_(s ? s : "") {}
  String(const std::string& s) : s_(s) {}
  String(double v, unsigned int decimals) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", static_cast<int>(decimals), v);
    s_ = buf;
  }
  String(float v, unsigned int decimals) : String(static_cast<double>(v), decimals) {}
  String& operator+=(const String& o) {
    s_ += o.s_;
    return *this;
  }
  String& operator+=(const char* o) {
    s_ += o;
    return *this;
  }
  bool equalsIgnoreCase(const String& o) const {
    if (s_.size() != o.s_.size()) return false;
    for (size_t i = 0; i < s_.size(); ++i) {
      if (tolower(static_cast<unsigned char>(s_[i])) !=
          tolower(static_cast<unsigned char>(o.s_[i]))) {
        return false;
      }
    }
    return true;
  }
  bool operator==(const char* o) const { return s_ == o; }
  const char* c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }

 private:
  std::string s_;
};

// ---- esp32 time helpers ----
inline void configTzTime(const char* tz, const char* server1,
                         const char* server2 = nullptr,
                         const char* server3 = nullptr) {
  (void)server3;
  fakearduino::g_tz = tz;
  fakearduino::g_ntp1 = server1 ? server1 : "";
  fakearduino::g_ntp2 = server2 ? server2 : "";
  ++fakearduino::g_config_tz_calls;
  setenv("TZ", tz, 1);
  tzset();
}
