#pragma once

namespace ui {

/** Touch targets on the right-hand panel. */
enum class PanelButton {
  kNone,
  kZoomIn,   // tighter range
  kZoomOut,  // wider range
  kRunways,  // toggle runway overlay
  kLoginInfo,  // title row: show/hide the web portal login card
};

/**
 * Draw the 320x480 info panel to the right of the radar scope: range, aircraft
 * count, the nearest aircraft list, and touch buttons. Called after every radar
 * frame; unless `force`, it only redraws every kSidePanelRedrawIntervalMs. Uses the radar palette, so call after the radar has drawn once.
 */
void sidePanelDraw(bool force);

/** Show (or hide) the web portal address, user and password over the aircraft
 *  list; it hides itself after a minute. Redraw with sidePanelDraw(true). */
void sidePanelToggleLoginCard();

/** Map a screen coordinate (full 800x480 space) to a panel button. */
PanelButton sidePanelHitTest(int x, int y);

}  // namespace ui
