// Preview shim for <WiFi.h>: only what the side panel's login card reads.
#pragma once

#include <Arduino.h>

struct PreviewIp {
  String toString() const { return String("192.168.1.50"); }
};

struct PreviewWiFi {
  PreviewIp localIP() const { return PreviewIp{}; }
};

inline PreviewWiFi WiFi;
