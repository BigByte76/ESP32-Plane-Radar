#pragma once

#include <cstdint>

namespace ui::radar {

/**
 * Radar scope geometry. The original firmware targeted a 240x240 round panel;
 * on the 800x480 ESP32-8048S050 the scope is a 480x480 square on the left of
 * the screen (config::kRadarOriginX/Y), so every pixel size is scaled by kScale.
 * All coordinates below are local to the 480x480 radar sprite.
 */
constexpr int kScale = 2;

constexpr int kSize = 240 * kScale;  // 480
constexpr int kCenterX = kSize / 2;
constexpr int kCenterY = kSize / 2;

/** Outermost grid ring (inside edge labels). */
constexpr int kGridOuterRadius = 107 * kScale;

/** N: offset from top edge (top_center, negative = up). */
constexpr int kCardinalNorthOffsetY = -1 * kScale;
/** S: offset from bottom edge (bottom_center, positive = down). */
constexpr int kCardinalSouthOffsetY = 3 * kScale;

/** Gap between scale label right edge and outer ring on the east spoke (px). */
constexpr int kScaleGapFromOuterRing = 6 * kScale;

/** Target cap height (px) for N/S/E/W. */
constexpr int kCardinalLabelHeightPx = 14 * kScale;
/** Scale label is this many px shorter than cardinals. */
constexpr int kScaleBelowCardinalPx = 3 * kScale;

constexpr int kRingCount = 4;

/** Shared grid stroke: drawWideLine half-width; rings use the same px count. */
constexpr float kGridStrokeHalfWidth = 1.0f * kScale * 0.75f;

constexpr int kCenterDotRadius = 2 * kScale;

/** Filled aircraft symbol (nose triangle). */
constexpr int kAircraftNoseLenPx = 8 * kScale;
constexpr int kAircraftTailLenPx = 3 * kScale;
constexpr int kAircraftTailHalfPx = 4 * kScale;
/** Track vector: ground distance covered in this many seconds at current gs. */
constexpr float kAircraftTrackHorizonSec = 60.0f;
/** Minimum visible vector when gs > 0 (px). */
constexpr int kAircraftSpeedLineMinPx = 2 * kScale;
/** Track line length uses this outer_km, not the active range preset. */
constexpr float kAircraftTrackRefOuterKm = 13.3f;
/** Shorter than full 60 s horizon at ref scale; x1.5 length boost applied. */
constexpr float kAircraftTrackLengthScale = 1.5f / 5.0f;
/** drawWideLine half-width for speed vectors. */
constexpr float kAircraftTrackLineHalfWidth = 1.0f * kScale * 0.75f;

constexpr float kRunwayLineWidthPx = 2.0f * kScale;
constexpr float kRunwayLineHalfWidth = kRunwayLineWidthPx * 0.5f;
constexpr int kRunwayLabelHeightPx = 12 * kScale;
constexpr int kRunwayLabelGapPx = 3 * kScale;
/** Gap from triangle edge to tag block (px). */
constexpr int kAircraftLabelGapPx = 1 * kScale;
/** Keep symbol centroid inside outer ring by at least this inset (px). */
constexpr int kAircraftInsideRingInsetPx =
    kAircraftNoseLenPx + kAircraftTailHalfPx + 1;

/** Beyond-ring traffic: bearing cues on screen rim (correct direction, fixed radius). */
constexpr int kBeyondRingDotRadiusPx = 4 * kScale;
constexpr int kBeyondRingScreenMarginPx = 2 * kScale;
/** Target cap height (px) for aircraft tags. Kept a bit under 2x so the three-line
 *  tags don't swamp the scope when traffic is dense. */
constexpr int kAircraftTagLabelHeightPx = 11 * kScale;

/** Largest VLW scale factor the font-size search may pick (the embedded VLW is
 *  15 pt, so ~28 px labels need roughly 1.8-2x). */
constexpr float kVlwMaxScale = 3.0f;

/** RGB565 palette targets (applied in initPalette). */
constexpr uint8_t kBgR = 4;
constexpr uint8_t kBgG = 10;
constexpr uint8_t kBgB = 28;
constexpr uint8_t kGridR = 16;
constexpr uint8_t kGridG = 100;
constexpr uint8_t kGridB = 32;
constexpr uint8_t kAircraftR = 255;
constexpr uint8_t kAircraftG = 0;
constexpr uint8_t kAircraftB = 0;
constexpr uint8_t kTrackR = 255;
constexpr uint8_t kTrackG = 0;
constexpr uint8_t kTrackB = 255;
constexpr uint8_t kTagTypeR = 255;
constexpr uint8_t kTagTypeG = 200;
constexpr uint8_t kTagTypeB = 0;
constexpr uint8_t kTagAltR = 90;
constexpr uint8_t kTagAltG = 200;
constexpr uint8_t kTagAltB = 255;
constexpr uint8_t kRunwayR = 56;
constexpr uint8_t kRunwayG = 150;
constexpr uint8_t kRunwayB = 170;
/** Lighter teal for ICAO labels (vs runway lines). */
constexpr uint8_t kRunwayLabelR = 110;
constexpr uint8_t kRunwayLabelG = 210;
constexpr uint8_t kRunwayLabelB = 230;

extern uint16_t kColorBackground;
extern uint16_t kColorGrid;
extern uint16_t kColorLabel;
extern uint16_t kColorCenter;
extern uint16_t kColorAircraft;
extern uint16_t kColorTrackVector;
extern uint16_t kColorTagType;
extern uint16_t kColorTagAltitude;
extern uint16_t kColorRunway;
extern uint16_t kColorRunwayLabel;

}  // namespace ui::radar
