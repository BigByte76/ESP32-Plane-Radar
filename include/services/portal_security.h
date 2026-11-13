#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Access control for the web portal (setup AP and LAN portal, including the
 * firmware upload at /update).
 *
 * A random per-device password is generated on first boot, kept in NVS and
 * shown only on the device's own screen: on the Wi-Fi setup screen and on the
 * side panel when its title is tapped. It is
 *   - the WPA2 passphrase of the "PlaneRadar-Setup" access point, and
 *   - the HTTP Digest password (user kAuthUser) for the LAN portal.
 * Holding BOOT for 3 s (factory reset) replaces it with a new one.
 *
 * The request policy itself (evaluate()) is pure so it can be unit tested; the
 * WebServer glue lives in wifi_setup.cpp.
 */
namespace services::portal_security {

constexpr char kAuthUser[] = "admin";
constexpr char kAuthRealm[] = "Plane Radar";
/** 10 chars from a 56-symbol alphabet: ~58 bits. Also >= 8 for WPA2. */
constexpr size_t kPasswordLen = 10;
/** Characters a password may contain (no 0/O, 1/l/I look-alikes). */
extern const char kPasswordAlphabet[];

/** Load the password from NVS, generating and saving one if missing. Call once
 *  in setup(), before Wi-Fi starts (the RNG then draws on its own entropy source). */
void init();

/** The current password (NUL-terminated, kPasswordLen chars). */
const char* password();

/** Replace the password with a new random one and save it (factory reset).
 *  Never leaves the device without a password, so the AP is never open. */
void regenerate();

/** Fill `out` with kPasswordLen random alphabet chars + NUL, drawing from `rng`.
 *  Uses rejection sampling, so every symbol is equally likely. */
void generatePassword(char* out, size_t out_len, uint32_t (*rng)());

/** True if `pw` is exactly kPasswordLen chars, all from kPasswordAlphabet. */
bool passwordWellFormed(const char* pw);

/** What the portal should do with a request. */
enum class Decision {
  kAllow,         // pass to the WiFiManager handler
  kRedirect,      // Host is not ours (DNS rebinding / captive check): 302 to the device IP
  kUnauthorized,  // 401, ask for the Digest password
  kForbidden,     // 403, cross-site request to a route that changes state
};

struct Request {
  const char* uri;      // path only, no query
  bool is_post;
  const char* host;     // Host header ("" if absent)
  const char* origin;   // Origin header ("" if absent)
  const char* referer;  // Referer header ("" if absent)
  const char* sta_ip;   // the device's LAN address ("" if not connected)
  bool require_auth;    // LAN portal; the setup AP is already WPA2-protected
  bool authenticated;   // the request carried valid credentials
};

/** The access policy: Host allow-list, then authentication, then a same-origin
 *  check for every request that changes state. */
Decision evaluate(const Request& req);

/** True when Host (port and one trailing dot ignored, case-insensitive) is the
 *  device: its LAN IP, the setup AP IP, or the mDNS name with or without ".local". */
bool hostAllowed(const char* host, const char* sta_ip);

/** POSTs, and the WiFiManager GET routes that save, erase, restart or exit. */
bool changesState(const char* uri, bool is_post);

/** True when Origin (if present) or else Referer names the same host:port as Host.
 *  Requests with neither, or with "Origin: null", are not same-origin. */
bool sameOrigin(const char* origin, const char* referer, const char* host);

/** HTML-escape `in` (& < > " ') into `out`, truncating at a whole entity.
 *  Returns the length written (excluding the NUL). */
size_t htmlEscape(const char* in, char* out, size_t out_len);

}  // namespace services::portal_security
