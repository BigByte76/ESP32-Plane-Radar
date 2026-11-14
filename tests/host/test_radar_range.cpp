// Tests for ui::radar range presets, units and runway toggle
// (src/ui/radar_range.cpp), with NVS backed by the in-memory Preferences stub.
#include "test_framework.h"

// Compile the unit under test into this binary so its file-local state can be
// reset between tests.
#include "../../src/ui/radar_range.cpp"

#include "ui/radar_theme.h"

using namespace ui::radar;

namespace {

/** Reset module statics to power-on values, then "boot" from NVS. */
void reboot() {
  s_range_index = kDefaultRangeIndex;
  s_use_miles = false;
  s_show_runways = true;
  rangeInit();
}

std::string label(float km, bool miles) {
  char buf[16];
  formatRing3Label(buf, sizeof(buf), km, miles);
  return buf;
}

}  // namespace

SET_UP({
  fakenvs::reset();
  s_range_index = kDefaultRangeIndex;
  s_use_miles = false;
  s_show_runways = true;
});

TEST(presets_are_strictly_increasing_and_outer_is_four_thirds_of_ring3) {
  CHECK(kRangePresetCount >= 2);
  for (size_t i = 0; i < kRangePresetCount; ++i) {
    CHECK_NEAR(kRangePresets[i].outer_km, kRangePresets[i].ring3_km * 4.0 / 3.0, 1e-4);
    if (i > 0) CHECK(kRangePresets[i].ring3_km > kRangePresets[i - 1].ring3_km);
  }
}

TEST(defaults_with_empty_nvs_are_10km_km_units_runways_on) {
  reboot();
  CHECK_EQ(rangeIndex(), uint8_t{1});
  CHECK_NEAR(rangeCurrent().ring3_km, 10.0, 1e-6);
  CHECK(!useMiles());
  CHECK(showRunways());
}

TEST(defaults_when_nvs_cannot_be_opened) {
  fakenvs::g_fail_begin = true;
  reboot();
  CHECK_EQ(rangeIndex(), uint8_t{1});
  rangeNext();  // save fails silently, must not crash
  CHECK_EQ(rangeIndex(), uint8_t{2});
}

TEST(range_next_cycles_and_wraps) {
  reboot();
  for (size_t i = 0; i < kRangePresetCount; ++i) rangeNext();
  CHECK_EQ(rangeIndex(), uint8_t{1});  // full cycle back to start
  while (rangeIndex() != kRangePresetCount - 1) rangeNext();
  rangeNext();
  CHECK_EQ(rangeIndex(), uint8_t{0});
}

TEST(zoom_out_clamps_at_widest) {
  reboot();
  int steps = 0;
  while (rangeZoomOut()) ++steps;
  CHECK_EQ(steps, static_cast<int>(kRangePresetCount) - 2);
  CHECK_EQ(rangeIndex(), static_cast<uint8_t>(kRangePresetCount - 1));
  CHECK(!rangeZoomOut());
  CHECK_EQ(rangeIndex(), static_cast<uint8_t>(kRangePresetCount - 1));
}

TEST(zoom_in_clamps_at_tightest) {
  reboot();
  CHECK(rangeZoomIn());
  CHECK_EQ(rangeIndex(), uint8_t{0});
  CHECK(!rangeZoomIn());
  CHECK_EQ(rangeIndex(), uint8_t{0});
}

TEST(range_persists_across_reboot) {
  reboot();
  rangeZoomOut();
  rangeZoomOut();
  const uint8_t saved = rangeIndex();
  reboot();
  CHECK_EQ(rangeIndex(), saved);
}

TEST(out_of_range_saved_index_falls_back_to_default) {
  Preferences p;
  p.begin("planeradar", false);
  p.putUChar("rangeIdx", 200);
  p.end();
  reboot();
  CHECK_EQ(rangeIndex(), uint8_t{1});
}

TEST(fetch_radius_reaches_screen_edge) {
  reboot();
  for (size_t i = 0; i < kRangePresetCount; ++i) {
    s_range_index = static_cast<uint8_t>(i);
    const float outer = kRangePresets[i].outer_km;
    const float expected = outer * static_cast<float>(kCenterX - kBeyondRingScreenMarginPx) /
                           static_cast<float>(kGridOuterRadius);
    CHECK_NEAR(fetchRadiusKm(), expected, 1e-4);
    CHECK(fetchRadiusKm() > outer);  // beyond-ring rim dots need data
  }
}

TEST(ring3_label_km) {
  CHECK_STREQ(label(5.0f, false), "5km");
  CHECK_STREQ(label(10.0f, false), "10km");
  CHECK_STREQ(label(50.0f, false), "50km");
  CHECK_STREQ(label(12.5f, false), "13km");  // rounds half away from zero
}

TEST(ring3_label_miles) {
  CHECK_STREQ(label(5.0f, true), "3mi");    // 3.11
  CHECK_STREQ(label(10.0f, true), "6mi");   // 6.21
  CHECK_STREQ(label(15.0f, true), "9mi");   // 9.32
  CHECK_STREQ(label(25.0f, true), "16mi");  // 15.53
  CHECK_STREQ(label(50.0f, true), "31mi");  // 31.07
}

TEST(ring3_label_truncates_safely_in_small_buffer) {
  char buf[3];
  formatRing3Label(buf, sizeof(buf), 50.0f, false);
  CHECK_STREQ(buf, "50");
}

TEST(current_label_follows_range_and_units) {
  reboot();
  char buf[16];
  formatCurrentRing3Label(buf, sizeof(buf));
  CHECK_STREQ(buf, "10km");
  saveMilesFromPortal("T");
  formatCurrentRing3Label(buf, sizeof(buf));
  CHECK_STREQ(buf, "6mi");
}

TEST(portal_checkbox_values) {
  struct Case {
    const char* value;
    bool expected;
  } cases[] = {
      {nullptr, false}, {"", false}, {"T", true}, {"t", true},
      {"F", true},  // WiFiManager posts value= attr when ticked, even if prefilled "F"
      {"f", true},      {"on", true}, {"off", false}, {"TT", false}, {"yes", false},
  };
  for (const auto& c : cases) {
    saveMilesFromPortal(c.value);
    if (useMiles() != c.expected) {
      testfw::fail(__FILE__, __LINE__,
                   std::string("miles checkbox value ") + (c.value ? c.value : "null"));
    }
    saveRunwaysFromPortal(c.value);
    if (showRunways() != c.expected) {
      testfw::fail(__FILE__, __LINE__,
                   std::string("runways checkbox value ") + (c.value ? c.value : "null"));
    }
  }
}

TEST(units_and_runways_persist_across_reboot) {
  reboot();
  saveMilesFromPortal("T");
  saveRunwaysFromPortal("");
  reboot();
  CHECK(useMiles());
  CHECK(!showRunways());
}

TEST(toggle_runways_flips_and_persists) {
  reboot();
  toggleRunways();
  CHECK(!showRunways());
  reboot();
  CHECK(!showRunways());
  toggleRunways();
  reboot();
  CHECK(showRunways());
}

TEST(units_reset_restores_defaults_but_keeps_range) {
  reboot();
  rangeZoomOut();
  saveMilesFromPortal("T");
  saveRunwaysFromPortal("");
  unitsReset();
  CHECK(!useMiles());
  CHECK(showRunways());
  reboot();
  CHECK(!useMiles());
  CHECK(showRunways());
  CHECK_EQ(rangeIndex(), uint8_t{2});
}
