#pragma once

void statusScreenPortal();
void statusScreenConnectFailed();
void statusScreenWifiReset();
/** Shown while a firmware upload (OTA via the portal's /update page) is written. */
void statusScreenOtaUpdating();

/** Saved-network connect animation (call Tick until connect finishes). */
void statusScreenConnectingBegin(const char* ssid);
void statusScreenConnectingTick();
