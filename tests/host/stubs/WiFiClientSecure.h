// Host stub for WiFiClient/WiFiClientSecure: serves bytes from fakehttp's body,
// at most `fakehttp::g_delivery_size` per available()/readBytes() burst so the
// framing decoder sees chunk boundaries split across socket reads.
#pragma once

#include <Arduino.h>

#include <algorithm>
#include <string>

namespace fakehttp {
/** CA bundle passed to the last WiFiClientSecure (nullptr: none / insecure). */
extern const char* g_last_ca_cert;
extern bool g_last_insecure;
extern std::string g_wire_body;   // raw bytes on the socket after the headers
extern size_t g_wire_pos;
extern size_t g_delivery_size;    // max bytes per read burst
}  // namespace fakehttp

class WiFiClient {
 public:
  virtual ~WiFiClient() = default;
  int available() {
    const size_t left = fakehttp::g_wire_body.size() - fakehttp::g_wire_pos;
    return static_cast<int>(std::min(left, fakehttp::g_delivery_size));
  }
  size_t readBytes(char* buf, size_t len) {
    const size_t left = fakehttp::g_wire_body.size() - fakehttp::g_wire_pos;
    const size_t n = std::min({len, left, fakehttp::g_delivery_size});
    memcpy(buf, fakehttp::g_wire_body.data() + fakehttp::g_wire_pos, n);
    fakehttp::g_wire_pos += n;
    return n;
  }
};

class WiFiClientSecure : public WiFiClient {
 public:
  void setInsecure() { fakehttp::g_last_insecure = true; }
  void setCACert(const char* rootCA) { fakehttp::g_last_ca_cert = rootCA; }
};
