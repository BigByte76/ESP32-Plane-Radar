#include "services/portal_security.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cctype>
#include <cstring>
#include <strings.h>

#ifdef ESP_PLATFORM
#include <bootloader_random.h>
#include <esp_random.h>
#endif

#include "config.h"

namespace services::portal_security {

const char kPasswordAlphabet[] =
    "ABCDEFGHJKLMNPQRSTUVWXYZ"  // no I, O
    "abcdefghijkmnpqrstuvwxyz"  // no l, o
    "23456789";                 // no 0, 1

namespace {

/** Own namespace so it never shares an NVS handle with other settings. */
constexpr char kPrefsNamespace[] = "portalauth";
constexpr char kPrefsPasswordKey[] = "pw";
constexpr char kApIp[] = "192.168.4.1";
constexpr size_t kAlphabetSize = sizeof(kPasswordAlphabet) - 1;

char s_password[kPasswordLen + 1] = {};

uint32_t hardwareRandom() { return esp_random(); }

/** esp_random() is only truly random while the RF subsystem runs, or while the
 *  bootloader's SAR-ADC entropy source is on. init() runs before Wi-Fi starts,
 *  so switch that source on around the draw. */
void generateWithHardwareEntropy(char* out, size_t out_len) {
#ifdef ESP_PLATFORM
  bootloader_random_enable();
#endif
  generatePassword(out, out_len, hardwareRandom);
#ifdef ESP_PLATFORM
  bootloader_random_disable();
#endif
}

void save() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    Serial.println("Portal password: NVS unavailable, not saved");
    return;
  }
  prefs.putString(kPrefsPasswordKey, s_password);
  prefs.end();
}

/** Copy `in` lowercased into `out`, dropping a ":port" suffix and one trailing
 *  dot. Returns false if it does not fit or is empty. */
bool normalizeAuthority(const char* in, size_t in_len, char* out, size_t out_len) {
  size_t n = 0;
  for (size_t i = 0; i < in_len && in[i] != '\0' && in[i] != ':'; ++i) {
    if (n + 1 >= out_len) {
      return false;
    }
    out[n++] = static_cast<char>(tolower(static_cast<unsigned char>(in[i])));
  }
  if (n > 0 && out[n - 1] == '.') {
    --n;
  }
  out[n] = '\0';
  return n > 0;
}

/** Port of an authority ("host:port"), with an absent port meaning 80. */
const char* authorityPort(const char* in, size_t in_len) {
  const void* colon = memchr(in, ':', in_len);
  if (colon == nullptr) {
    return "80";
  }
  return static_cast<const char*>(colon) + 1;
}

size_t portLen(const char* port, size_t max) {
  size_t n = 0;
  while (n < max && port[n] != '\0' && isdigit(static_cast<unsigned char>(port[n]))) {
    ++n;
  }
  return n;
}

/** Compare the authority of an "http://authority/..." URL with a Host header. */
bool urlMatchesHost(const char* url, const char* host) {
  constexpr char kScheme[] = "http://";
  constexpr size_t kSchemeLen = sizeof(kScheme) - 1;
  if (strncasecmp(url, kScheme, kSchemeLen) != 0) {
    return false;
  }
  const char* auth = url + kSchemeLen;
  const size_t auth_len = strcspn(auth, "/?#");
  if (auth_len == 0 || memchr(auth, '@', auth_len) != nullptr) {
    return false;  // no authority, or userinfo (http://evil@host) games
  }

  char a[96];
  char b[96];
  const size_t host_len = strlen(host);
  if (!normalizeAuthority(auth, auth_len, a, sizeof(a)) ||
      !normalizeAuthority(host, host_len, b, sizeof(b)) || strcmp(a, b) != 0) {
    return false;
  }
  const char* pa = authorityPort(auth, auth_len);
  const char* pb = authorityPort(host, host_len);
  const size_t la = portLen(pa, auth_len);
  const size_t lb = portLen(pb, host_len);
  return la == lb && strncmp(pa, pb, la) == 0;
}

}  // namespace

void generatePassword(char* out, size_t out_len, uint32_t (*rng)()) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  // Largest multiple of the alphabet size that fits in 2^32: values at or above
  // it are redrawn so that no symbol is favoured.
  constexpr uint64_t kRange = uint64_t{1} << 32;
  constexpr uint64_t kBound = kRange - (kRange % kAlphabetSize);
  const size_t len = out_len - 1 < kPasswordLen ? out_len - 1 : kPasswordLen;
  for (size_t i = 0; i < len; ++i) {
    uint32_t v = 0;
    do {
      v = rng();
    } while (v >= kBound);
    out[i] = kPasswordAlphabet[v % kAlphabetSize];
  }
  out[len] = '\0';
}

bool passwordWellFormed(const char* pw) {
  if (pw == nullptr || strnlen(pw, kPasswordLen + 1) != kPasswordLen) {
    return false;
  }
  for (size_t i = 0; i < kPasswordLen; ++i) {
    if (strchr(kPasswordAlphabet, pw[i]) == nullptr) {
      return false;
    }
  }
  return true;
}

void init() {
  char stored[kPasswordLen + 2] = {};
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    if (prefs.isKey(kPrefsPasswordKey)) {
      prefs.getString(kPrefsPasswordKey, stored, sizeof(stored));
    }
    prefs.end();
  }
  if (passwordWellFormed(stored)) {
    memcpy(s_password, stored, sizeof(s_password));
    return;
  }
  generateWithHardwareEntropy(s_password, sizeof(s_password));
  save();
  Serial.println("Portal password: generated a new one");
}

const char* password() {
  if (s_password[0] == '\0') {
    init();  // never hand out an empty password (that would open the AP)
  }
  return s_password;
}

void regenerate() {
  generateWithHardwareEntropy(s_password, sizeof(s_password));
  save();
}

bool hostAllowed(const char* host, const char* sta_ip) {
  if (host == nullptr) {
    return false;
  }
  char h[96];
  if (!normalizeAuthority(host, strlen(host), h, sizeof(h))) {
    return false;
  }
  if (sta_ip != nullptr && sta_ip[0] != '\0' && strcmp(h, sta_ip) == 0) {
    return true;
  }
  return strcmp(h, kApIp) == 0 || strcmp(h, config::kPortalHostUrl) == 0 ||
         strcmp(h, config::kPortalHostname) == 0;
}

bool changesState(const char* uri, bool is_post) {
  if (is_post) {
    return true;
  }
  if (uri == nullptr) {
    return false;
  }
  static const char* const kStateChangingGets[] = {
      "/wifisave", "/paramsave", "/restart", "/exit", "/close", "/erase", "/u",
  };
  for (const char* route : kStateChangingGets) {
    if (strcmp(uri, route) == 0) {
      return true;
    }
  }
  return false;
}

bool sameOrigin(const char* origin, const char* referer, const char* host) {
  if (host == nullptr || host[0] == '\0') {
    return false;
  }
  if (origin != nullptr && origin[0] != '\0') {
    return urlMatchesHost(origin, host);  // also rejects "null"
  }
  if (referer != nullptr && referer[0] != '\0') {
    return urlMatchesHost(referer, host);
  }
  return false;
}

Decision evaluate(const Request& req) {
  if (!hostAllowed(req.host, req.sta_ip)) {
    return Decision::kRedirect;
  }
  // A browser fetches /favicon.ico without credentials; answering it with 401
  // would make the WebServer mint a new Digest nonce and invalidate the login
  // the page itself just used. WiFiManager has no favicon (it 404s), so the
  // exemption exposes nothing.
  const bool favicon = !req.is_post && req.uri != nullptr &&
                       strcmp(req.uri, "/favicon.ico") == 0;
  if (req.require_auth && !req.authenticated && !favicon) {
    return Decision::kUnauthorized;
  }
  if (changesState(req.uri, req.is_post) &&
      !sameOrigin(req.origin, req.referer, req.host)) {
    return Decision::kForbidden;
  }
  return Decision::kAllow;
}

size_t htmlEscape(const char* in, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return 0;
  }
  size_t n = 0;
  for (const char* p = in; p != nullptr && *p != '\0'; ++p) {
    const char* rep = nullptr;
    switch (*p) {
      case '&': rep = "&amp;"; break;
      case '<': rep = "&lt;"; break;
      case '>': rep = "&gt;"; break;
      case '"': rep = "&quot;"; break;
      case '\'': rep = "&#39;"; break;
      default: break;
    }
    const size_t rep_len = rep != nullptr ? strlen(rep) : 1;
    if (n + rep_len >= out_len) {
      break;
    }
    if (rep != nullptr) {
      memcpy(out + n, rep, rep_len);
    } else {
      out[n] = *p;
    }
    n += rep_len;
  }
  out[n] = '\0';
  return n;
}

}  // namespace services::portal_security
