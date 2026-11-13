// Tests for services::portal_security (src/services/portal_security.cpp): the
// on-device password (generation, persistence, factory reset) and the web
// portal request policy (Host allow-list, Digest auth, same-origin check).
#include "test_framework.h"

#include "../../src/services/portal_security.cpp"

#include <set>
#include <string>

namespace sec = services::portal_security;
using sec::Decision;

namespace {

constexpr char kStaIp[] = "192.168.1.50";

/** A same-origin, authenticated LAN request; tests tweak one field at a time. */
sec::Request lanRequest(const char* uri, bool is_post) {
  return sec::Request{uri,  is_post, kStaIp, "", "http://192.168.1.50/param",
                      kStaIp, true,    true};
}

uint32_t g_seq_values[8];
size_t g_seq_pos = 0;
uint32_t sequenceRng() { return g_seq_values[g_seq_pos++ % 8]; }

}  // namespace

SET_UP({
  fakenvs::g_store.clear();
  fakenvs::g_fail_begin = false;
  fakearduino::g_serial.clear();
  fakearduino::g_random_calls = 0;
  sec::s_password[0] = '\0';
});

// ---- Password ----

TEST(generated_password_has_fixed_length_and_alphabet) {
  char pw[sec::kPasswordLen + 1];
  sec::generatePassword(pw, sizeof(pw), esp_random);
  CHECK_EQ(strlen(pw), sec::kPasswordLen);
  CHECK(sec::passwordWellFormed(pw));
}

TEST(alphabet_has_no_lookalike_characters) {
  for (char c : std::string("0O1lIo")) {
    CHECK(strchr(sec::kPasswordAlphabet, c) == nullptr);
  }
  CHECK_EQ(strlen(sec::kPasswordAlphabet), size_t{56});
}

TEST(generation_rejects_biased_values) {
  // 0xFFFFFFFF is above the largest multiple of 56 below 2^32, so it must be
  // redrawn rather than mapped onto a symbol.
  g_seq_pos = 0;
  for (auto& v : g_seq_values) v = 0;
  g_seq_values[0] = 0xFFFFFFFFu;
  g_seq_values[1] = 3;
  char pw[sec::kPasswordLen + 1];
  sec::generatePassword(pw, sizeof(pw), sequenceRng);
  CHECK_EQ(pw[0], sec::kPasswordAlphabet[3]);
  CHECK_EQ(pw[1], sec::kPasswordAlphabet[0]);
}

TEST(generation_respects_small_buffers) {
  char pw[4];
  sec::generatePassword(pw, sizeof(pw), esp_random);
  CHECK_EQ(strlen(pw), size_t{3});
}

TEST(passwords_are_not_repeated) {
  std::set<std::string> seen;
  for (int i = 0; i < 200; ++i) {
    char pw[sec::kPasswordLen + 1];
    sec::generatePassword(pw, sizeof(pw), esp_random);
    seen.insert(pw);
  }
  CHECK_EQ(seen.size(), size_t{200});
}

TEST(well_formed_rejects_bad_passwords) {
  CHECK(!sec::passwordWellFormed(nullptr));
  CHECK(!sec::passwordWellFormed(""));
  CHECK(!sec::passwordWellFormed("abcdefghi"));     // too short
  CHECK(!sec::passwordWellFormed("abcdefghijk"));   // too long
  CHECK(!sec::passwordWellFormed("abcdefghi0"));    // 0 not in alphabet
  CHECK(!sec::passwordWellFormed("abcd'fghij"));
  CHECK(sec::passwordWellFormed("abcdefghij"));
}

TEST(init_generates_once_and_persists_across_reboots) {
  sec::init();
  const std::string first = sec::password();
  CHECK(sec::passwordWellFormed(first.c_str()));
  CHECK(fakearduino::g_random_calls > 0);

  sec::s_password[0] = '\0';  // reboot: RAM gone, NVS kept
  fakearduino::g_random_calls = 0;
  sec::init();
  CHECK_STREQ(sec::password(), first);
  CHECK_EQ(fakearduino::g_random_calls, 0);
}

TEST(corrupt_stored_password_is_replaced) {
  Preferences p;
  p.begin("portalauth", false);
  p.putString("pw", "short");
  p.end();
  sec::init();
  CHECK(sec::passwordWellFormed(sec::password()));
  CHECK(std::string(sec::password()) != "short");
}

TEST(regenerate_replaces_and_saves_password) {
  sec::init();
  const std::string before = sec::password();
  sec::regenerate();
  const std::string after = sec::password();
  CHECK(after != before);
  CHECK(sec::passwordWellFormed(after.c_str()));
  sec::s_password[0] = '\0';
  sec::init();
  CHECK_STREQ(sec::password(), after);
}

TEST(password_is_never_empty_even_without_nvs) {
  fakenvs::g_fail_begin = true;
  CHECK(sec::passwordWellFormed(sec::password()));
}

// ---- Host allow-list ----

TEST(host_allow_list) {
  CHECK(sec::hostAllowed("192.168.1.50", kStaIp));
  CHECK(sec::hostAllowed("192.168.1.50:80", kStaIp));
  CHECK(sec::hostAllowed("192.168.4.1", ""));
  CHECK(sec::hostAllowed("plane-radar.local", kStaIp));
  CHECK(sec::hostAllowed("PLANE-RADAR.LOCAL.", kStaIp));
  CHECK(sec::hostAllowed("plane-radar", kStaIp));
  CHECK(!sec::hostAllowed("evil.example", kStaIp));          // DNS rebinding
  CHECK(!sec::hostAllowed("plane-radar.local.evil.example", kStaIp));
  CHECK(!sec::hostAllowed("192.168.1.51", kStaIp));
  CHECK(!sec::hostAllowed("", kStaIp));
  CHECK(!sec::hostAllowed(nullptr, kStaIp));
  CHECK(!sec::hostAllowed("192.168.1.50", ""));              // not on the LAN
}

// ---- Same-origin ----

TEST(same_origin_prefers_origin_then_referer) {
  const char* host = "plane-radar.local";
  CHECK(sec::sameOrigin("http://plane-radar.local", "", host));
  CHECK(sec::sameOrigin("http://PLANE-RADAR.local:80", "", host));
  CHECK(sec::sameOrigin("", "http://plane-radar.local/info?x=1", host));
  CHECK(!sec::sameOrigin("http://evil.example", "http://plane-radar.local/", host));
  CHECK(!sec::sameOrigin("null", "", host));
  CHECK(!sec::sameOrigin("", "", host));
  CHECK(!sec::sameOrigin("https://plane-radar.local", "", host));
  CHECK(!sec::sameOrigin("http://plane-radar.local:8080", "", host));
  CHECK(!sec::sameOrigin("http://plane-radar.local@evil.example", "", host));
  CHECK(!sec::sameOrigin("", "http://plane-radar.local.evil.example/", host));
  CHECK(!sec::sameOrigin("http://plane-radar.local", "", ""));
}

TEST(state_changing_routes) {
  CHECK(sec::changesState("/anything", true));
  for (const char* uri :
       {"/wifisave", "/paramsave", "/restart", "/exit", "/close", "/erase", "/u"}) {
    CHECK(sec::changesState(uri, false));
  }
  for (const char* uri : {"/", "/wifi", "/info", "/param", "/update", "/status"}) {
    CHECK(!sec::changesState(uri, false));
  }
}

// ---- Policy ----

TEST(authenticated_same_origin_requests_are_allowed) {
  CHECK_EQ(sec::evaluate(lanRequest("/u", true)), Decision::kAllow);
  CHECK_EQ(sec::evaluate(lanRequest("/erase", false)), Decision::kAllow);
  CHECK_EQ(sec::evaluate(lanRequest("/info", false)), Decision::kAllow);
}

TEST(lan_requests_without_credentials_get_401) {
  auto req = lanRequest("/update", false);
  req.authenticated = false;
  CHECK_EQ(sec::evaluate(req), Decision::kUnauthorized);
  req = lanRequest("/u", true);
  req.authenticated = false;
  CHECK_EQ(sec::evaluate(req), Decision::kUnauthorized);
  req = lanRequest("/", false);
  req.authenticated = false;
  CHECK_EQ(sec::evaluate(req), Decision::kUnauthorized);
}

TEST(favicon_is_exempt_from_auth_but_nothing_else) {
  auto req = lanRequest("/favicon.ico", false);
  req.authenticated = false;
  CHECK_EQ(sec::evaluate(req), Decision::kAllow);
  req.is_post = true;
  CHECK_EQ(sec::evaluate(req), Decision::kUnauthorized);
}

TEST(cross_site_upload_is_forbidden_even_with_cached_credentials) {
  // A malicious page POSTing to /u: the browser attaches the cached Digest
  // login, but Origin names the attacker's site.
  auto req = lanRequest("/u", true);
  req.origin = "http://evil.example";
  CHECK_EQ(sec::evaluate(req), Decision::kForbidden);
  req.origin = "";
  req.referer = "";
  CHECK_EQ(sec::evaluate(req), Decision::kForbidden);
}

TEST(cross_site_get_to_erase_is_forbidden) {
  auto req = lanRequest("/erase", false);
  req.referer = "";  // <img src="http://plane-radar.local/erase"> with no-referrer
  CHECK_EQ(sec::evaluate(req), Decision::kForbidden);
  req.referer = "http://evil.example/";
  CHECK_EQ(sec::evaluate(req), Decision::kForbidden);
}

TEST(cross_site_read_only_get_is_allowed_after_auth) {
  auto req = lanRequest("/info", false);
  req.referer = "";
  CHECK_EQ(sec::evaluate(req), Decision::kAllow);
}

TEST(foreign_host_is_redirected_before_anything_else) {
  auto req = lanRequest("/u", true);
  req.host = "rebind.evil.example";
  req.origin = "http://rebind.evil.example";  // same-origin from the attacker's view
  CHECK_EQ(sec::evaluate(req), Decision::kRedirect);
}

TEST(setup_ap_skips_auth_but_keeps_csrf_check) {
  sec::Request req{"/wifisave", true, "192.168.4.1", "http://192.168.4.1", "",
                   "", false, false};
  CHECK_EQ(sec::evaluate(req), Decision::kAllow);
  req.origin = "http://evil.example";
  CHECK_EQ(sec::evaluate(req), Decision::kForbidden);
  // Captive-portal probes use foreign hosts and must be redirected to the portal.
  sec::Request probe{"/hotspot-detect.html", false, "captive.apple.com", "", "",
                     "", false, false};
  CHECK_EQ(sec::evaluate(probe), Decision::kRedirect);
}

// ---- HTML escaping ----

TEST(html_escape_neutralises_markup) {
  char out[64];
  CHECK_EQ(sec::htmlEscape("a<b>&'\"", out, sizeof(out)), strlen(out));
  CHECK_STREQ(out, "a&lt;b&gt;&amp;&#39;&quot;");
  sec::htmlEscape("Mon 12:00 <+08>", out, sizeof(out));
  CHECK_STREQ(out, "Mon 12:00 &lt;+08&gt;");
}

TEST(html_escape_truncates_at_entity_boundary) {
  char out[8];
  sec::htmlEscape("ab<cdef", out, sizeof(out));
  CHECK_STREQ(out, "ab&lt;c");
  char tiny[3];
  sec::htmlEscape("<<", tiny, sizeof(tiny));
  CHECK_STREQ(tiny, "");
  sec::htmlEscape(nullptr, out, sizeof(out));
  CHECK_STREQ(out, "");
}
