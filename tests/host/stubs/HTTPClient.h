// Host stub for the ESP32 HTTPClient: scripted status code / headers / body.
#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include <string>
#include <vector>

#define HTTP_CODE_OK 200
#define HTTPC_ERROR_CONNECTION_REFUSED (-1)
#define HTTPC_ERROR_NOT_CONNECTED (-4)
#define HTTPC_ERROR_READ_TIMEOUT (-11)

namespace fakehttp {
/** Status codes returned by successive GET() calls; the last one repeats. */
extern std::vector<int> g_get_codes;
extern int g_get_calls;
extern bool g_begin_ok;
extern bool g_chunked;
extern int g_content_length;  // -1 = no Content-Length header
extern std::string g_last_url;
extern bool g_ended;
/** Configure a response. `wire_body` is sent verbatim (already chunk-framed
 *  if `chunked`). */
void reset();
}  // namespace fakehttp

class HTTPClient {
 public:
  bool begin(WiFiClient& client, const String& url) {
    client_ = &client;
    fakehttp::g_last_url = url.c_str();
    return fakehttp::g_begin_ok;
  }
  void collectHeaders(const char* headers[], size_t count) {
    for (size_t i = 0; i < count; ++i) wanted_.push_back(headers[i]);
  }
  void setTimeout(unsigned long) {}
  void setConnectTimeout(int) {}
  int GET() {
    const size_t i = static_cast<size_t>(fakehttp::g_get_calls++);
    const auto& codes = fakehttp::g_get_codes;
    if (codes.empty()) return HTTP_CODE_OK;
    return i < codes.size() ? codes[i] : codes.back();
  }
  String header(const char* name) {
    bool collected = false;
    for (const auto& w : wanted_) collected |= (w == name);
    if (collected && std::string(name) == "Transfer-Encoding" &&
        fakehttp::g_chunked) {
      return String("chunked");
    }
    return String("");
  }
  int getSize() { return fakehttp::g_chunked ? -1 : fakehttp::g_content_length; }
  WiFiClient* getStreamPtr() { return client_; }
  bool connected() {
    return fakehttp::g_wire_pos < fakehttp::g_wire_body.size();
  }
  void end() { fakehttp::g_ended = true; }

 private:
  WiFiClient* client_ = nullptr;
  std::vector<std::string> wanted_;
};
