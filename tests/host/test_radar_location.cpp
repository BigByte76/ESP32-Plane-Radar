// Tests for services::location (src/services/radar_location.cpp): portal
// string parsing, validation and NVS persistence.
#include "test_framework.h"

#include "../../src/services/radar_location.cpp"

#include "config.h"

namespace loc = services::location;

namespace {
void reboot() {
  loc::s_lat = config::kDefaultRadarLat;
  loc::s_lon = config::kDefaultRadarLon;
  loc::init();
}
}  // namespace

SET_UP({
  fakenvs::reset();
  loc::s_lat = config::kDefaultRadarLat;
  loc::s_lon = config::kDefaultRadarLon;
});

TEST(defaults_when_nothing_stored) {
  reboot();
  CHECK_NEAR(loc::lat(), config::kDefaultRadarLat, 1e-12);
  CHECK_NEAR(loc::lon(), config::kDefaultRadarLon, 1e-12);
}

TEST(save_valid_coordinates_updates_and_persists) {
  reboot();
  CHECK(loc::saveFromStrings("51.4700", "-0.4543"));
  CHECK_NEAR(loc::lat(), 51.47, 1e-9);
  CHECK_NEAR(loc::lon(), -0.4543, 1e-9);
  reboot();
  CHECK_NEAR(loc::lat(), 51.47, 1e-9);
  CHECK_NEAR(loc::lon(), -0.4543, 1e-9);
}

TEST(boundary_values_are_accepted) {
  CHECK(loc::saveFromStrings("90", "180"));
  CHECK(loc::saveFromStrings("-90", "-180"));
  CHECK(loc::saveFromStrings("0", "0"));
  CHECK(loc::saveFromStrings("+45.5", "1e1"));
  CHECK_NEAR(loc::lon(), 10.0, 1e-12);
}

TEST(invalid_strings_are_rejected_and_keep_previous) {
  CHECK(loc::saveFromStrings("10", "20"));
  const char* bad[][2] = {
      {nullptr, "1"}, {"1", nullptr}, {"", "1"},     {"1", ""},
      {"abc", "1"},   {"1", "1x"},    {"12.5 ", "1"}, {" ", "1"},
      {"90.0001", "0"}, {"-90.1", "0"}, {"0", "180.5"}, {"0", "-181"},
      {"nan", "0"},
  };
  for (const auto& b : bad) {
    if (loc::saveFromStrings(b[0], b[1])) {
      testfw::fail(__FILE__, __LINE__,
                   std::string("accepted lat=") + (b[0] ? b[0] : "null") +
                       " lon=" + (b[1] ? b[1] : "null"));
    }
  }
  CHECK_NEAR(loc::lat(), 10.0, 1e-12);
  CHECK_NEAR(loc::lon(), 20.0, 1e-12);
}

TEST(corrupt_stored_values_are_ignored_on_boot) {
  Preferences p;
  p.begin("radar", false);
  p.putDouble("lat", 123.0);  // out of range
  p.putDouble("lon", 5.0);
  p.end();
  reboot();
  CHECK_NEAR(loc::lat(), config::kDefaultRadarLat, 1e-12);
}

TEST(only_one_key_stored_uses_defaults) {
  Preferences p;
  p.begin("radar", false);
  p.putDouble("lat", 12.0);
  p.end();
  reboot();
  CHECK_NEAR(loc::lat(), config::kDefaultRadarLat, 1e-12);
}

TEST(clear_restores_defaults_and_erases_nvs) {
  CHECK(loc::saveFromStrings("1", "2"));
  loc::clear();
  CHECK_NEAR(loc::lat(), config::kDefaultRadarLat, 1e-12);
  reboot();
  CHECK_NEAR(loc::lat(), config::kDefaultRadarLat, 1e-12);
  CHECK_NEAR(loc::lon(), config::kDefaultRadarLon, 1e-12);
}

TEST(default_location_is_itself_valid) {
  CHECK(loc::validLatLon(config::kDefaultRadarLat, config::kDefaultRadarLon));
}
