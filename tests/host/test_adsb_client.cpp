// End-to-end tests for services::adsb::fetchUpdate (src/services/adsb_client.cpp):
// URL construction, identity + chunked responses through BodyFramer and
// ArduinoJson, field fallbacks, filtering, limits and error handling.
#include "test_framework.h"

#include "../../src/services/adsb_client.cpp"

#include <string>
#include <vector>

namespace adsb = services::adsb;

namespace {

std::string chunked(const std::string& body, size_t chunk) {
  std::string out;
  for (size_t pos = 0; pos < body.size(); pos += chunk) {
    const size_t n = std::min(chunk, body.size() - pos);
    char hdr[16];
    snprintf(hdr, sizeof(hdr), "%zx\r\n", n);
    out += hdr;
    out += body.substr(pos, n);
    out += "\r\n";
  }
  return out + "0\r\n\r\n";
}

void serveIdentity(const std::string& body, bool with_length = true) {
  fakehttp::g_chunked = false;
  fakehttp::g_wire_body = body;
  fakehttp::g_content_length = with_length ? static_cast<int>(body.size()) : -1;
}

void serveChunked(const std::string& body, size_t chunk = 7) {
  fakehttp::g_chunked = true;
  fakehttp::g_wire_body = chunked(body, chunk);
}

std::vector<adsb::Aircraft> snapshot() {
  std::vector<adsb::Aircraft> v(adsb::kMaxAircraft);
  const size_t n = adsb::snapshotAircraft(v.data(), v.size(), nullptr);
  v.resize(n);
  return v;
}

bool fetch() { return adsb::fetchUpdate(51.5, -0.12, 10.0f); }

const char* kTwoPlanes = R"({"now":1,"ac":[
  {"hex":"4ca1b2","flight":"RYR12AB ","t":"B738","lat":51.51,"lon":-0.11,
   "alt_baro":35000,"gs":450.5,"track":90.0,"true_heading":88.0,"seen_pos":1.5,
   "squawk":"1234","rssi":-20.1,"category":"A3"},
  {"hex":"abc123","lat":51.40,"lon":-0.20,"alt_baro":"ground","gs":5},
  {"hex":"c0ffee","t":"A320","lat":51.45,"lon":-0.15,"alt_geom":1234.6,
   "mag_heading":270,"tas":200,"seen_pos":-3}
]})";

}  // namespace

SET_UP({
  fakehttp::reset();
  fakearduino::g_millis = 10000;
  fakearduino::g_serial.clear();
  adsb::s_aircraft_count = 0;
  adsb::s_last_update_ms = 0;
  adsb::s_poll_fn = nullptr;
});

TEST(init_creates_mutex_once) {
  const int before = fakertos::g_mutex_creates;
  adsb::init();
  adsb::init();
  CHECK_EQ(fakertos::g_mutex_creates, before + 1);
}

TEST(builds_expected_url_with_radius_in_nautical_miles) {
  serveIdentity(R"({"ac":[]})");
  CHECK(adsb::fetchUpdate(51.5, -0.12, 10.0f));
  CHECK_STREQ(fakehttp::g_last_url,
              "https://opendata.adsb.fi/api/v3/lat/51.500000/lon/-0.120000/dist/5.4");
  CHECK(fakehttp::g_ended);
}

TEST(parses_identity_body_and_skips_ground_traffic) {
  serveIdentity(kTwoPlanes);
  CHECK(fetch());
  const auto ac = snapshot();
  CHECK_EQ(ac.size(), size_t{2});
  if (ac.size() != 2) return;

  CHECK_NEAR(ac[0].lat, 51.51, 1e-5);
  CHECK_NEAR(ac[0].lon, -0.11, 1e-5);
  CHECK_STREQ(ac[0].callsign, "RYR12AB");  // trailing space trimmed
  CHECK_STREQ(ac[0].type, "B738");
  CHECK_STREQ(ac[0].alt, "35000 ft");
  CHECK_NEAR(ac[0].nose_deg, 88.0, 1e-5);   // true_heading preferred for nose
  CHECK_NEAR(ac[0].track_deg, 90.0, 1e-5);  // track preferred for motion
  CHECK_NEAR(ac[0].gs_knots, 450.5, 1e-4);
  CHECK_EQ(ac[0].pos_age_ms, uint32_t{1500});

  CHECK_STREQ(ac[1].callsign, "c0ffee");  // no flight -> hex
  CHECK_STREQ(ac[1].type, "A320");
  CHECK_STREQ(ac[1].alt, "1235 ft");      // alt_geom fallback, rounded
  CHECK_NEAR(ac[1].nose_deg, 270.0, 1e-5);
  CHECK_NEAR(ac[1].track_deg, 270.0, 1e-5);  // falls back to mag_heading
  CHECK_NEAR(ac[1].gs_knots, 200.0, 1e-5);   // tas fallback
  CHECK_EQ(ac[1].pos_age_ms, uint32_t{0});   // negative seen_pos clamped
}

TEST(parses_chunked_body_with_awkward_chunk_sizes) {
  for (size_t chunk : {1, 2, 3, 7, 64, 511, 4096}) {
    for (size_t delivery : {1, 5, 512}) {
      fakehttp::reset();
      fakehttp::g_delivery_size = delivery;
      serveChunked(kTwoPlanes, chunk);
      if (!fetch() || adsb::aircraftCount() != 2) {
        testfw::fail(__FILE__, __LINE__,
                     "chunk=" + std::to_string(chunk) +
                         " delivery=" + std::to_string(delivery));
      }
      // drain() must consume the terminating chunk and trailer.
      CHECK_EQ(fakehttp::g_wire_pos, fakehttp::g_wire_body.size());
    }
  }
}

TEST(parses_close_delimited_identity_body) {
  serveIdentity(kTwoPlanes, /*with_length=*/false);
  CHECK(fetch());
  CHECK_EQ(adsb::aircraftCount(), size_t{2});
}

TEST(records_update_time_for_dead_reckoning) {
  serveIdentity(R"({"ac":[]})");
  fakearduino::g_millis = 123456;
  CHECK(fetch());
  unsigned long t = 0;
  adsb::Aircraft buf[1];
  adsb::snapshotAircraft(buf, 1, &t);
  CHECK_EQ(t, 123456UL);
  CHECK_EQ(adsb::lastUpdateMs(), 123456UL);
}

TEST(heading_and_speed_fallback_chains) {
  serveIdentity(R"({"ac":[
    {"hex":"a1","lat":1,"lon":1,"dir":45,"ias":120},
    {"hex":"a2","lat":1,"lon":1,"track":10,"mag_heading":20},
    {"hex":"a3","lat":1,"lon":1},
    {"hex":"a4","lat":1,"lon":1,"true_heading":30,"gs":0,"tas":300}
  ]})");
  CHECK(fetch());
  const auto ac = snapshot();
  CHECK_EQ(ac.size(), size_t{4});
  if (ac.size() != 4) return;
  CHECK_NEAR(ac[0].nose_deg, 45, 1e-5);
  CHECK_NEAR(ac[0].track_deg, 45, 1e-5);
  CHECK_NEAR(ac[0].gs_knots, 120, 1e-5);
  CHECK_NEAR(ac[1].nose_deg, 20, 1e-5);  // mag_heading beats track for nose
  CHECK_NEAR(ac[1].track_deg, 10, 1e-5);
  CHECK_NEAR(ac[2].nose_deg, 0, 1e-5);
  CHECK_NEAR(ac[2].gs_knots, 0, 1e-5);
  CHECK_STREQ(ac[2].alt, "");
  CHECK_NEAR(ac[3].track_deg, 30, 1e-5);
  CHECK_NEAR(ac[3].gs_knots, 0, 1e-5);   // gs present (even 0) wins over tas
}

TEST(entries_without_position_are_skipped) {
  serveIdentity(R"({"ac":[
    {"hex":"n1","lon":1},
    {"hex":"n2","lat":1},
    {"hex":"n3","lat":"x","lon":1},
    {"hex":"ok","lat":1,"lon":2}
  ]})");
  CHECK(fetch());
  const auto ac = snapshot();
  CHECK_EQ(ac.size(), size_t{1});
  if (!ac.empty()) CHECK_STREQ(ac[0].callsign, "ok");
}

TEST(strings_are_truncated_to_field_size) {
  serveIdentity(R"({"ac":[{"flight":"ABCDEFGHIJKL","t":"LONGTYPE","lat":1,"lon":1}]})");
  CHECK(fetch());
  const auto ac = snapshot();
  CHECK_EQ(ac.size(), size_t{1});
  if (ac.empty()) return;
  CHECK_STREQ(ac[0].callsign, "ABCDEFGH");  // 8 chars + NUL
  CHECK_STREQ(ac[0].type, "LONG");          // 4 chars + NUL
}

TEST(all_space_flight_falls_back_to_hex) {
  serveIdentity(R"({"ac":[{"flight":"        ","hex":"deadbe","lat":1,"lon":1}]})");
  CHECK(fetch());
  const auto ac = snapshot();
  if (!ac.empty()) CHECK_STREQ(ac[0].callsign, "deadbe");
}

TEST(seen_pos_is_capped_at_30s) {
  serveIdentity(R"({"ac":[{"hex":"a","lat":1,"lon":1,"seen_pos":120.0}]})");
  CHECK(fetch());
  const auto ac = snapshot();
  if (!ac.empty()) CHECK_EQ(ac[0].pos_age_ms, uint32_t{30000});
}

TEST(caps_at_max_aircraft) {
  std::string body = R"({"ac":[)";
  for (size_t i = 0; i < adsb::kMaxAircraft + 20; ++i) {
    if (i) body += ",";
    body += R"({"hex":"h)" + std::to_string(i) + R"(","lat":1,"lon":1})";
  }
  body += "]}";
  serveChunked(body, 300);
  CHECK(fetch());
  CHECK_EQ(adsb::aircraftCount(), adsb::kMaxAircraft);
  const auto ac = snapshot();
  CHECK_STREQ(ac.back().callsign, "h127");
}

TEST(snapshot_respects_max_out) {
  serveIdentity(kTwoPlanes);
  CHECK(fetch());
  adsb::Aircraft one[1];
  CHECK_EQ(adsb::snapshotAircraft(one, 1, nullptr), size_t{1});
  CHECK_STREQ(one[0].callsign, "RYR12AB");
}

TEST(missing_ac_array_publishes_empty_list) {
  serveIdentity(kTwoPlanes);
  CHECK(fetch());
  fakehttp::reset();
  serveIdentity(R"({"msg":"No error","now":1})");
  CHECK(fetch());
  CHECK_EQ(adsb::aircraftCount(), size_t{0});
}

// ---------------- failures keep the previous list ----------------

namespace {
void primeWithTwoPlanes() {
  serveIdentity(kTwoPlanes);
  fetch();
  fakehttp::reset();
  fakearduino::g_serial.clear();
}
}  // namespace

TEST(http_error_status_keeps_previous_data) {
  primeWithTwoPlanes();
  const unsigned long t = adsb::lastUpdateMs();
  fakehttp::g_get_codes = {503};
  fakearduino::g_millis += 5000;
  CHECK(!fetch());
  CHECK_EQ(adsb::aircraftCount(), size_t{2});
  CHECK_EQ(adsb::lastUpdateMs(), t);
  CHECK(fakearduino::g_serial.find("HTTP 503") != std::string::npos);
}

TEST(http_begin_failure_returns_false) {
  primeWithTwoPlanes();
  fakehttp::g_begin_ok = false;
  CHECK(!fetch());
  CHECK_EQ(adsb::aircraftCount(), size_t{2});
}

TEST(connection_refused_is_retried_then_succeeds) {
  fakehttp::g_get_codes = {HTTPC_ERROR_CONNECTION_REFUSED,
                           HTTPC_ERROR_NOT_CONNECTED, HTTP_CODE_OK};
  serveIdentity(kTwoPlanes);
  CHECK(fetch());
  CHECK_EQ(fakehttp::g_get_calls, 3);
  CHECK_EQ(adsb::aircraftCount(), size_t{2});
}

TEST(persistent_connection_refused_times_out) {
  primeWithTwoPlanes();
  fakehttp::g_get_codes = {HTTPC_ERROR_CONNECTION_REFUSED};
  const unsigned long start = millis();
  CHECK(!fetch());
  CHECK(millis() - start >= 6000);  // gave up at the request deadline
  CHECK(fakearduino::g_serial.find("HTTP -11") != std::string::npos);
  CHECK_EQ(adsb::aircraftCount(), size_t{2});
}

TEST(other_http_client_errors_are_not_retried) {
  fakehttp::g_get_codes = {-3};
  CHECK(!fetch());
  CHECK_EQ(fakehttp::g_get_calls, 1);
}

TEST(malformed_chunked_body_is_reported) {
  primeWithTwoPlanes();
  fakehttp::g_chunked = true;
  fakehttp::g_wire_body = "zz\r\n{}\r\n0\r\n\r\n";
  CHECK(!fetch());
  CHECK(fakearduino::g_serial.find("malformed chunked body") != std::string::npos);
  CHECK_EQ(adsb::aircraftCount(), size_t{2});
}

TEST(empty_response_is_reported) {
  primeWithTwoPlanes();
  serveIdentity("");
  CHECK(!fetch());
  CHECK(fakearduino::g_serial.find("empty response") != std::string::npos);
  CHECK_EQ(adsb::aircraftCount(), size_t{2});
}

TEST(truncated_json_is_a_parse_error) {
  primeWithTwoPlanes();
  const std::string body = kTwoPlanes;
  fakehttp::g_chunked = false;
  fakehttp::g_wire_body = body.substr(0, body.size() / 2);
  fakehttp::g_content_length = static_cast<int>(body.size());
  CHECK(!fetch());
  CHECK(fakearduino::g_serial.find("JSON parse error") != std::string::npos);
  CHECK_EQ(adsb::aircraftCount(), size_t{2});
}

TEST(poll_callback_runs_during_fetch) {
  static int polls = 0;
  polls = 0;
  adsb::setPollFn([] { ++polls; });
  fakehttp::g_delivery_size = 16;
  serveChunked(kTwoPlanes, 50);
  CHECK(fetch());
  CHECK(polls > 1);  // once per GET attempt plus once per socket refill
  adsb::setPollFn(nullptr);
}

TEST(publish_and_snapshot_take_the_lock) {
  adsb::init();
  const int takes = fakertos::g_take_count;
  const int gives = fakertos::g_give_count;
  serveIdentity(kTwoPlanes);
  CHECK(fetch());
  snapshot();
  CHECK_EQ(fakertos::g_take_count - takes, 2);
  CHECK_EQ(fakertos::g_give_count - gives, 2);
}

// ---- Hardening against a hostile or broken server ----

TEST(tls_verifies_against_pinned_roots) {
  serveIdentity(R"({"ac":[]})");
  CHECK(fetch());
  CHECK(!fakehttp::g_last_insecure);
  CHECK(fakehttp::g_last_ca_cert == adsb::kRootCaPem);
  CHECK(strstr(adsb::kRootCaPem, "-----BEGIN CERTIFICATE-----") != nullptr);
}

TEST(non_finite_and_out_of_range_numbers_are_sanitised) {
  serveIdentity(R"({"ac":[
    {"hex":"inf","lat":1,"lon":1,"gs":1e999,"track":-1e999,"alt_baro":1e999,"seen_pos":1e999},
    {"hex":"big","lat":1,"lon":1,"gs":1e30,"track":725,"true_heading":-90,"alt_baro":3e9},
    {"hex":"neg","lat":1,"lon":1,"gs":-50,"seen_pos":-1e999}
  ]})");
  CHECK(fetch());
  const auto ac = snapshot();
  CHECK_EQ(ac.size(), size_t{3});
  if (ac.size() != 3) return;
  CHECK_NEAR(ac[0].gs_knots, 0, 1e-6);       // infinity: treated as absent
  CHECK_NEAR(ac[0].track_deg, 0, 1e-6);
  CHECK_STREQ(ac[0].alt, "");
  CHECK_EQ(ac[0].pos_age_ms, uint32_t{0});   // inf seen_pos is ignored, not UB
  CHECK_NEAR(ac[1].gs_knots, 0, 1e-6);       // beyond kMaxGroundSpeedKnots
  CHECK_NEAR(ac[1].track_deg, 5, 1e-3);      // 725 -> 5
  CHECK_NEAR(ac[1].nose_deg, 270, 1e-3);     // -90 -> 270
  CHECK_STREQ(ac[1].alt, "");                // absurd altitude: no tag
  CHECK_NEAR(ac[2].gs_knots, 0, 1e-6);
  CHECK_EQ(ac[2].pos_age_ms, uint32_t{0});
}

TEST(positions_outside_the_globe_are_skipped) {
  serveIdentity(R"({"ac":[
    {"hex":"p1","lat":91,"lon":1},
    {"hex":"p2","lat":1,"lon":-180.5},
    {"hex":"p3","lat":1e999,"lon":1},
    {"hex":"ok","lat":-90,"lon":180}
  ]})");
  CHECK(fetch());
  const auto ac = snapshot();
  CHECK_EQ(ac.size(), size_t{1});
  if (!ac.empty()) CHECK_STREQ(ac[0].callsign, "ok");
}

TEST(oversized_response_fails_cleanly_and_keeps_previous_data) {
  serveIdentity(R"({"ac":[{"hex":"keep","lat":1,"lon":1}]})");
  CHECK(fetch());
  fakehttp::reset();

  // Far beyond kJsonBudgetBytes once parsed, even after the field filter.
  std::string body = R"({"ac":[)";
  for (int i = 0; i < 40000; ++i) {
    if (i) body += ",";
    body += R"({"hex":"h)" + std::to_string(i) + R"(","lat":1,"lon":1,"gs":1,"track":2})";
  }
  body += "]}";
  serveIdentity(body);
  fakearduino::g_serial.clear();
  CHECK(!fetch());
  CHECK(fakearduino::g_serial.find("response too large") != std::string::npos);
  const auto ac = snapshot();
  CHECK_EQ(ac.size(), size_t{1});
  if (!ac.empty()) CHECK_STREQ(ac[0].callsign, "keep");
}

TEST(budget_allocator_tracks_and_refuses) {
  adsb::BudgetAllocator a(100);
  void* p = a.allocate(60);
  CHECK(p != nullptr);
  CHECK(a.allocate(50) == nullptr);   // 40 left
  p = a.reallocate(p, 90);            // grow by 30 of the 40 left
  CHECK(p != nullptr);
  CHECK(a.reallocate(p, 120) == nullptr);
  p = a.reallocate(p, 10);            // shrink returns budget
  CHECK(p != nullptr);
  void* q = a.allocate(90);
  CHECK(q != nullptr);
  a.deallocate(p);
  a.deallocate(q);
  a.deallocate(nullptr);
  void* r = a.allocate(100);
  CHECK(r != nullptr);
  a.deallocate(r);
}
