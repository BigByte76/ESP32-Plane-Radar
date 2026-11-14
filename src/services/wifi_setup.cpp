#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/display_schedule.h"
#include "services/portal_security.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

bool s_force_config_portal = false;
/** Set when a portal firmware upload starts; read by the ADS-B task (core 0). */
volatile bool s_ota_in_progress = false;
WiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

constexpr int kCoordParamLen = 20;
constexpr char kCoordInputAttrs[] =
    " type=\"number\" step=\"0.000001\"";

WiFiManagerParameter s_param_lat("radar_lat", "Latitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);

char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

// --- Display schedule ---
namespace sched = services::display_schedule;

constexpr int kTimeParamLen = 8;
constexpr int kTzParamLen = 63;
constexpr char kTimeInputAttrs[] = " type=\"time\"";

/** Section heading plus the device's current local time, so the TZ can be checked. */
char s_sched_heading_html[160] = "";
WiFiManagerParameter s_param_sched_heading(s_sched_heading_html);

char s_sched_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_sched_on("sched_on", "Turn display off on a schedule", "T", 2,
                                      s_sched_checkbox_attrs, WFM_LABEL_AFTER);
WiFiManagerParameter s_param_disp_off("disp_off", "Display off at", "23:00",
                                      kTimeParamLen, kTimeInputAttrs);
WiFiManagerParameter s_param_disp_on("disp_on", "Display on at", "07:00",
                                     kTimeParamLen, kTimeInputAttrs);
/** Time zone dropdown (custom HTML, rebuilt with the current zone selected).
 *  It writes the chosen POSIX string into the "tz" input, which is hidden unless
 *  "Other" is picked, so any POSIX TZ can still be entered by hand. */
char s_tz_select_html[4096] = "";
WiFiManagerParameter s_param_tz_select(s_tz_select_html);
char s_tz_input_attrs[96] = "type='hidden'";
WiFiManagerParameter s_param_tz("tz", "", "UTC0", kTzParamLen, s_tz_input_attrs,
                                WFM_NO_LABEL);
WiFiManagerParameter s_param_sched_help(
    "<small>While the display is off, tap the screen to wake it for a minute.</small>");

size_t s_tz_select_used = 0;

void tzSelectAppend(const char* fmt, ...) {
  if (s_tz_select_used >= sizeof(s_tz_select_html)) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(s_tz_select_html + s_tz_select_used,
                          sizeof(s_tz_select_html) - s_tz_select_used, fmt, args);
  va_end(args);
  if (n > 0) {
    s_tz_select_used += static_cast<size_t>(n);
  }
}

void refreshTimezoneSelect() {
  const char* current = sched::timezone();
  s_tz_select_used = 0;

  tzSelectAppend("<br/><label for='tzsel'>Time zone</label>"
         "<select id='tzsel' onchange=\"var t=document.getElementById('tz');"
         "t.type=this.value?'hidden':'text';"
         "if(this.value){t.value=this.value}else{t.focus()}\">");
  bool matched = false;
  for (size_t i = 0; i < sched::kTimezoneCount; ++i) {
    const bool sel = strcmp(sched::kTimezones[i].posix, current) == 0;
    matched = matched || sel;
    tzSelectAppend("<option value='%s'%s>%s</option>", sched::kTimezones[i].posix,
           sel ? " selected" : "", sched::kTimezones[i].label);
  }
  tzSelectAppend("<option value=''%s>Other (enter POSIX TZ)</option></select>",
         matched ? "" : " selected");
  if (s_tz_select_used >= sizeof(s_tz_select_html)) {
    Serial.println("Portal: time zone list truncated");
  }

  snprintf(s_tz_input_attrs, sizeof(s_tz_input_attrs), "%s",
           matched ? "type='hidden'"
                   : "type='text' placeholder='e.g. EST5EDT,M3.2.0,M11.1.0'");
}

void refreshScheduleHeading() {
  char now_buf[40];
  sched::formatNow(now_buf, sizeof(now_buf));
  // %Z comes from the user-supplied TZ string: escape it before it reaches HTML.
  char now_html[sizeof(now_buf) * 6];
  services::portal_security::htmlEscape(now_buf, now_html, sizeof(now_html));
  snprintf(s_sched_heading_html, sizeof(s_sched_heading_html),
           "<hr><h3>Display schedule</h3><small>Device time now: %s</small><br/>",
           now_html);
}

void refreshPortalParamValues() {
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::lat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::lon());
  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);
  snprintf(s_miles_checkbox_attrs, sizeof(s_miles_checkbox_attrs), "type=\"checkbox\"%s",
           ui::radar::useMiles() ? " checked" : "");
  s_param_miles.setValue("T", 2);
  snprintf(s_runways_checkbox_attrs, sizeof(s_runways_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::showRunways() ? " checked" : "");
  s_param_runways.setValue("T", 2);

  refreshScheduleHeading();
  snprintf(s_sched_checkbox_attrs, sizeof(s_sched_checkbox_attrs), "type=\"checkbox\"%s",
           sched::enabled() ? " checked" : "");
  s_param_sched_on.setValue("T", 2);
  char time_buf[kTimeParamLen + 1];
  sched::formatHhMm(time_buf, sizeof(time_buf), sched::offMinutes());
  s_param_disp_off.setValue(time_buf, kTimeParamLen);
  sched::formatHhMm(time_buf, sizeof(time_buf), sched::onMinutes());
  s_param_disp_on.setValue(time_buf, kTimeParamLen);
  s_param_tz.setValue(sched::timezone(), kTzParamLen);
  refreshTimezoneSelect();
}

void onPortalParamsSaved() {
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
  sched::saveFromPortal(s_param_sched_on.getValue(), s_param_disp_off.getValue(),
                        s_param_disp_on.getValue(), s_param_tz.getValue());

  refreshPortalParamValues();
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamValues();
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.addParameter(&s_param_sched_heading);
  wm.addParameter(&s_param_sched_on);
  wm.addParameter(&s_param_disp_off);
  wm.addParameter(&s_param_disp_on);
  wm.addParameter(&s_param_tz_select);
  wm.addParameter(&s_param_tz);
  wm.addParameter(&s_param_sched_help);
  wm.setSaveParamsCallback(onPortalParamsSaved);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  services::display_schedule::clear();
  services::portal_security::regenerate();
  Serial.println("WiFi credentials, location, units, display schedule and portal "
                 "password reset");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

/** Runs on the loop task just before WiFiManager calls Update.begin(). The
 *  upload itself blocks inside s_wm.process(), so the radar stops redrawing;
 *  show a status screen and stop ADS-B fetches so TLS traffic and the HTTPS
 *  task don't compete with the flash writes. */
void onPreOtaUpdate() {
  s_ota_in_progress = true;
  services::display_schedule::forceOn();
  Serial.println("OTA: firmware upload started");
  statusScreenOtaUpdating();
}

// --- Portal access control -------------------------------------------------
//
// WiFiManager has no authentication or CSRF protection of its own, and every
// route (including the firmware upload at /u) is live on the LAN. PortalGate is
// registered before WiFiManager's handlers, so the WebServer picks it for every
// request; it applies services::portal_security::evaluate() and only then
// forwards to the WiFiManager handler further down the chain.
//
// The WebServer chooses a handler from the request line alone, before headers
// are read, so the gate cannot just decline: it must claim every request and
// dispatch itself. Upload bodies are streamed to the chosen handler's upload()
// after the headers are parsed, which is where an upload is accepted or dropped.

namespace sec = services::portal_security;

constexpr char kUploadRoute[] = "/u";
const char* kGateHeaders[] = {"Origin", "Referer"};
const IPAddress kApIp(192, 168, 4, 1);

/** The firmware upload in the current request was allowed through. */
bool s_upload_allowed = false;

/** True for requests that arrived through the setup AP's own interface: the
 *  AP is WPA2-protected with the same password, so those skip HTTP auth. */
bool requestViaSetupAp(WebServer& server) {
  return server.client().localIP() == kApIp;
}

bool unsafeMethod(HTTPMethod method) {
  return method != HTTP_GET && method != HTTP_HEAD && method != HTTP_OPTIONS;
}

sec::Decision decide(WebServer& server, HTTPMethod method, const String& uri) {
  const String host = server.hostHeader();
  const String origin = server.header(kGateHeaders[0]);
  const String referer = server.header(kGateHeaders[1]);
  const String sta_ip = wifiLinkUp() ? WiFi.localIP().toString() : String();
  const bool require_auth = !requestViaSetupAp(server);
  const bool authenticated =
      require_auth && server.authenticate(sec::kAuthUser, sec::password());
  const sec::Request req{uri.c_str(),     unsafeMethod(method), host.c_str(),
                         origin.c_str(),  referer.c_str(),      sta_ip.c_str(),
                         require_auth,    authenticated};
  return sec::evaluate(req);
}

void respondDenied(WebServer& server, sec::Decision decision) {
  switch (decision) {
    case sec::Decision::kRedirect:
      server.sendHeader("Location",
                        "http://" + server.client().localIP().toString() + "/");
      server.send(302, "text/plain", "");
      return;
    case sec::Decision::kUnauthorized:
      server.requestAuthentication(
          DIGEST_AUTH, sec::kAuthRealm,
          "Log in as \"admin\" with the password shown on the Plane Radar "
          "screen (tap WEB LOGIN at the top of the side panel).");
      return;
    case sec::Decision::kForbidden:
      server.send(403, "text/plain",
                  "Cross-site request blocked. Open the portal directly and use "
                  "its buttons.");
      return;
    case sec::Decision::kAllow:
      return;
  }
}

class PortalGate : public RequestHandler {
 public:
  /** Called once per request, as soon as its request line is parsed (the gate
   *  is first in the chain). Clear upload state here as well as in handle(),
   *  which never runs if a client drops mid-upload. */
  bool canHandle(HTTPMethod, String) override {
    s_upload_allowed = false;
    return true;
  }
  bool canUpload(String uri) override { return uri == kUploadRoute; }

  bool handle(WebServer& server, HTTPMethod method, String uri) override {
    const bool upload_route = method == HTTP_POST && uri == kUploadRoute;
    const bool upload_allowed = s_upload_allowed;
    s_upload_allowed = false;

    const sec::Decision decision = decide(server, method, uri);
    if (decision != sec::Decision::kAllow) {
      respondDenied(server, decision);
      return true;
    }
    if (upload_route && !upload_allowed) {
      // WiFiManager would report success for a POST with no file and reboot.
      server.send(400, "text/plain", "No firmware file in the upload.");
      return true;
    }
    RequestHandler* target = nextHandlerFor(method, uri);
    if (target == nullptr) {
      return false;  // WebServer falls back to WiFiManager's not-found handler
    }
    return target->handle(server, method, uri);
  }

  void upload(WebServer& server, String uri, HTTPUpload& upload) override {
    if (upload.status == UPLOAD_FILE_START) {
      s_upload_allowed = decide(server, HTTP_POST, uri) == sec::Decision::kAllow;
      if (!s_upload_allowed) {
        Serial.println("OTA: rejected unauthenticated or cross-site upload");
      }
    }
    if (!s_upload_allowed) {
      return;  // drop the bytes; handle() answers 401/403 afterwards
    }
    RequestHandler* target = nextHandlerFor(HTTP_POST, uri);
    if (target != nullptr && target->canUpload(uri)) {
      target->upload(server, uri, upload);
    }
  }

 private:
  RequestHandler* nextHandlerFor(HTTPMethod method, const String& uri) {
    for (RequestHandler* h = next(); h != nullptr; h = h->next()) {
      if (h->canHandle(method, uri)) {
        return h;
      }
    }
    return nullptr;
  }
};

/** WiFiManager calls this right after creating its WebServer and before it
 *  registers its own routes, so the gate ends up first in the handler chain.
 *  The WebServer deletes its handlers when it is destroyed. */
void installPortalGate() {
  s_upload_allowed = false;
  s_wm.server->collectHeaders(kGateHeaders,
                              sizeof(kGateHeaders) / sizeof(kGateHeaders[0]));
  s_wm.server->addHandler(new PortalGate());
}

/** With rollback enabled, a freshly flashed image boots as "pending verify"
 *  and the bootloader reverts to the previous one if it resets before this is
 *  called. Mark it good once the portal (the OTA path) is reachable again. */
void markFirmwareValid() {
  static bool s_marked = false;
  if (s_marked) {
    return;
  }
  s_marked = true;
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("OTA: new firmware confirmed, rollback cancelled");
  }
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setWebServerCallback(installPortalGate);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setAPCallback(onConfigPortalApStarted);
  s_wm.setPreOtaUpdateCallback(onPreOtaUpdate);
  attachPortalParams(s_wm);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamValues();
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s (user \"%s\", password "
                "on screen)\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str(),
                services::portal_security::kAuthUser);
  markFirmwareValid();
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }

  if (conf.sta.ssid[0] == '\0') {
    return false;
  }

  // ESP-IDF stores the SSID in a fixed 32-byte field. A maximum-length
  // SSID has no room for a trailing NUL, so copy it to a larger buffer
  // and explicitly terminate it before constructing an Arduino String.
  char ssid_buf[sizeof(conf.sta.ssid) + 1] = {};
  memcpy(ssid_buf, conf.sta.ssid, sizeof(conf.sta.ssid));
  ssid_buf[sizeof(conf.sta.ssid)] = '\0';

  char pass_buf[sizeof(conf.sta.password) + 1] = {};
  memcpy(pass_buf, conf.sta.password, sizeof(conf.sta.password));
  pass_buf[sizeof(conf.sta.password)] = '\0';

  const String ssid(ssid_buf);
  const String pass(pass_buf);

  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  // WPA2 with the on-screen password: an open AP would let anyone in range
  // configure the device or flash it, and would expose the home Wi-Fi password
  // typed into the form.
  s_wm.startConfigPortal(config::kPortalApName, services::portal_security::password());
  markFirmwareValid();
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

/** Run the setup portal until the device is online. The portal closes after
 *  kWifiPortalTimeoutSec without a client (so a router outage or a jammed
 *  network does not leave the AP up for good); the saved network is then
 *  retried, and the portal reopens if that fails too. */
bool portalUntilConnected() {
  for (;;) {
    if (openConfigPortal() && wifiLinkUp()) {
      break;
    }
    if (storedWifiCredentials() && connectSavedNetwork(true)) {
      break;
    }
    Serial.println("WiFi still not connected — reopening setup portal");
  }
  WiFi.setAutoReconnect(true);
  Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str());
  return true;
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool wifiOtaInProgress() { return s_ota_in_progress; }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      Serial.println("BOOT held — resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  return connectSavedNetwork(true);
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      // Keep "Device time now" on the portal page current.
      static unsigned long s_last_heading_ms = 0;
      if (millis() - s_last_heading_ms >= 5000) {
        s_last_heading_ms = millis();
        refreshScheduleHeading();
      }
      bootButtonPollLongPress();
      s_wm.process();
    }
    // A successful upload reboots inside process(); getting here with the flag
    // still set means the upload failed or was aborted, so resume normal work.
    if (s_ota_in_progress && !Update.isRunning()) {
      s_ota_in_progress = false;
      Serial.println("OTA: upload failed — resuming");
    }
  } else {
    stopLanWebPortal();
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    return portalUntilConnected();
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }
  return portalUntilConnected();
}
