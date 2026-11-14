// Storage for the host stubs' globals.
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <freertos/semphr.h>
#include <hardware/display.h>

namespace fakearduino {
unsigned long g_millis = 0;
std::string g_tz;
std::string g_ntp1;
std::string g_ntp2;
int g_config_tz_calls = 0;
std::string g_serial;
uint32_t g_random_state = 0x12345678u;
int g_random_calls = 0;
}  // namespace fakearduino

FakeSerial Serial;
FakeTft tft;

namespace fakenvs {
std::map<std::string, Namespace> g_store;
bool g_fail_begin = false;
}  // namespace fakenvs

namespace fakertos {
int g_mutex_creates = 0;
int g_take_count = 0;
int g_give_count = 0;
}  // namespace fakertos

namespace fakehttp {
const char* g_last_ca_cert = nullptr;
bool g_last_insecure = false;
std::string g_wire_body;
size_t g_wire_pos = 0;
size_t g_delivery_size = 512;
std::vector<int> g_get_codes;
int g_get_calls = 0;
bool g_begin_ok = true;
bool g_chunked = false;
int g_content_length = -1;
std::string g_last_url;
bool g_ended = false;

void reset() {
  g_wire_body.clear();
  g_wire_pos = 0;
  g_delivery_size = 512;
  g_get_codes.clear();
  g_get_calls = 0;
  g_begin_ok = true;
  g_chunked = false;
  g_content_length = -1;
  g_last_url.clear();
  g_ended = false;
  g_last_ca_cert = nullptr;
  g_last_insecure = false;
}
}  // namespace fakehttp
