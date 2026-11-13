"""Tests for scripts/build_large_airports.py and the data it generated.

Run: python3 -m unittest discover -s tests/python   (or ./scripts/run-tests.sh)
No network access: fetch_csv is replaced with in-memory fixtures.
"""

from __future__ import annotations

import importlib.util
import re
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "build_large_airports.py"
GEN_H = ROOT / "include" / "data" / "large_airports.h"
GEN_CPP = ROOT / "src" / "data" / "large_airports_data.cpp"

_spec = importlib.util.spec_from_file_location("build_large_airports", SCRIPT)
bla = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bla)


def airport(ident, type_="large_airport", lat="51.47", lon="-0.4543"):
    return {"ident": ident, "type": type_, "latitude_deg": lat, "longitude_deg": lon}


def runway(airport_ident, le="09L", he="27R", length_ft="12000", closed="0",
           coords=("51.0", "-0.5", "51.0", "-0.4")):
    le_lat, le_lon, he_lat, he_lon = coords
    return {
        "airport_ident": airport_ident, "le_ident": le, "he_ident": he,
        "length_ft": length_ft, "closed": closed,
        "le_latitude_deg": le_lat, "le_longitude_deg": le_lon,
        "he_latitude_deg": he_lat, "he_longitude_deg": he_lon,
    }


class CoordE7Test(unittest.TestCase):
    def test_converts_and_rounds(self):
        self.assertEqual(bla.coord_e7("51.4700"), 514700000)
        self.assertEqual(bla.coord_e7("-0.4543"), -4543000)
        self.assertEqual(bla.coord_e7("0.00000006"), 1)
        self.assertEqual(bla.coord_e7(" 1.5 "), 15000000)

    def test_blank_is_none(self):
        for v in (None, "", "   "):
            self.assertIsNone(bla.coord_e7(v))

    def test_fits_int32(self):
        self.assertLess(abs(bla.coord_e7("180")), 2**31)
        self.assertLess(abs(bla.coord_e7("-180")), 2**31)


class HelipadTest(unittest.TestCase):
    def test_h_designators(self):
        for s in ("H", "H1", "H12", "H-1", "H_A"):
            self.assertTrue(bla.is_h_designator(s), s)
        for s in ("", "09", "HA", "H1A", "27H", "h1"):
            self.assertFalse(bla.is_h_designator(s), s)

    def test_both_ends_h_is_helipad_regardless_of_length(self):
        self.assertTrue(bla.is_helipad({"le_ident": "H1", "he_ident": "H2",
                                        "length_ft": "9000"}))

    def test_one_end_h_depends_on_length(self):
        self.assertTrue(bla.is_helipad({"le_ident": "H1", "he_ident": "",
                                        "length_ft": "100"}))
        self.assertFalse(bla.is_helipad({"le_ident": "H1", "he_ident": "",
                                         "length_ft": "3000"}))

    def test_bad_length_counts_as_short(self):
        self.assertTrue(bla.is_helipad({"le_ident": "h1", "length_ft": "abc"}))

    def test_normal_runway_is_not_helipad(self):
        self.assertFalse(bla.is_helipad({"le_ident": "09L", "he_ident": "27R",
                                         "length_ft": "100"}))


class BuildDatasetTest(unittest.TestCase):
    def build(self, airports, runways):
        def fake_fetch(url):
            return airports if url == bla.AIRPORTS_URL else runways
        with mock.patch.object(bla, "fetch_csv", side_effect=fake_fetch):
            return bla.build_dataset()

    def test_filters_airports(self):
        rows, _ = self.build(
            [airport("EGLL"), airport("KJFK"),
             airport("EGKK", type_="medium_airport"),
             airport("XX1"),                 # ident not 4 chars
             airport("ZZZZ", lat="")],       # no coords
            [])
        self.assertEqual([r[0] for r in rows], ["EGLL", "KJFK"])  # sorted

    def test_filters_and_converts_runways(self):
        rows, segs = self.build(
            [airport("KJFK"), airport("EGLL")],
            [runway("EGLL", length_ft="12799"),
             runway("EGLL", le="09R", he="27L", length_ft="12008"),
             runway("EGLL", closed="1"),
             runway("EGLL", le="H1", he="H2"),                      # helipad
             runway("EGLL", length_ft="0"),
             runway("EGLL", length_ft="n/a"),
             runway("EGLL", coords=("", "-0.5", "51", "-0.4")),     # missing end
             runway("EGKK"),                                        # not large
             runway("KJFK", length_ft="14511")])
        self.assertEqual([r[0] for r in rows], ["EGLL", "KJFK"])
        self.assertEqual(len(segs), 3)
        # Sorted by airport index, then longest runway first.
        self.assertEqual([(s[0], s[5]) for s in segs],
                         [(0, round(12799 * 0.3048)), (0, round(12008 * 0.3048)),
                          (1, round(14511 * 0.3048))])
        self.assertEqual(segs[0][1:5], (510000000, -5000000, 510000000, -4000000))


class RenderTest(unittest.TestCase):
    def test_header_counts(self):
        h = bla.render_header(3, 7)
        self.assertIn("constexpr size_t kAirportCount = 3;", h)
        self.assertIn("constexpr size_t kRunwayCount = 7;", h)
        self.assertIn("do not edit", h)

    def test_cpp_rows(self):
        cpp = bla.render_cpp([("EGLL", 514700000, -4543000)],
                             [(0, 1, 2, 3, 4, 3901)])
        self.assertIn('  {"EGLL", 514700000, -4543000},', cpp)
        self.assertIn("  {0, 1, 2, 3, 4, 3901},", cpp)
        self.assertIn('#include "data/large_airports.h"', cpp)


class GeneratedDataTest(unittest.TestCase):
    """The checked-in generated files must be internally consistent."""

    @classmethod
    def setUpClass(cls):
        cls.h = GEN_H.read_text(encoding="utf-8")
        cpp = GEN_CPP.read_text(encoding="utf-8")
        cls.airports = re.findall(r'^\s*\{"(\w{4})", (-?\d+), (-?\d+)\},$', cpp, re.M)
        cls.runways = [tuple(map(int, m)) for m in re.findall(
            r"^\s*\{(\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+), (\d+)\},$", cpp, re.M)]

    def count(self, name):
        return int(re.search(rf"{name} = (\d+);", self.h).group(1))

    def test_counts_match_header(self):
        self.assertEqual(len(self.airports), self.count("kAirportCount"))
        self.assertEqual(len(self.runways), self.count("kRunwayCount"))

    def test_airports_sorted_unique(self):
        idents = [a[0] for a in self.airports]
        self.assertEqual(idents, sorted(set(idents)))

    def test_values_in_range(self):
        for ident, lat, lon in self.airports:
            self.assertLessEqual(abs(int(lat)), 900000000, ident)
            self.assertLessEqual(abs(int(lon)), 1800000000, ident)
        n = len(self.airports)
        prev = (-1, 0)
        for idx, le_lat, le_lon, he_lat, he_lon, length_m in self.runways:
            self.assertLess(idx, n)
            self.assertLess(idx, 2**16)             # uint16_t airport_idx
            self.assertTrue(0 < length_m < 2**16)   # uint16_t length_m
            for v, lim in ((le_lat, 9e8), (he_lat, 9e8), (le_lon, 18e8), (he_lon, 18e8)):
                self.assertLessEqual(abs(v), lim)
            self.assertGreaterEqual((idx, -length_m), prev)  # generator sort order
            prev = (idx, -length_m)


if __name__ == "__main__":
    unittest.main()
