#pragma once

namespace ui {

/** Touch targets on the right-hand panel. */
enum class PanelButton {
  kNone,
  kZoomIn,   // tighter range
  kZoomOut,  // wider range
  kRunways,  // toggle runway overlay
};

/**
 * Draw the 320x480 info panel to the right of the radar scope: range, aircraft
 * count, the nearest aircraft list, and touch buttons. Called after every radar
 * frame; unless `force`, it only redraws every kSidePanelRedrawIntervalMs. Uses the radar palette, so call after the radar has drawn once.
 */
void sidePanelDraw(bool force);

/** Map a screen coordinate (full 800x480 space) to a panel button. */
PanelButton sidePanelHitTest(int x, int y);

}  // namespace ui
