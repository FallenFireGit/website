// TI-84 Plus CE internal Wi-Fi bridge -- ESP32-C3 Super Mini firmware.
//
// The C3's USB-Serial-JTAG port (GPIO18/19) is soldered to the calculator's
// USB D-/D+ pads. The calculator runs as USB host (usbdrvce + srldrvce) and
// talks to us with a line-based text protocol. See ../README.md.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <time.h>
#include "soc/usb_serial_jtag_struct.h"

// ---- Pins (ESP32-C3 Super Mini) ----
static const int PIN_ID_FET = 3;  // N-MOSFET gate: HIGH grounds the calc's USB ID pin
static const int PIN_VBUS   = 4;  // VBUS through 100k/47k divider (5 V -> ~1.6 V)
static const int PIN_LED    = 8;  // onboard blue LED, active low

// ---- Tunables ----
static const uint32_t VBUS_MV_THRESHOLD = 1000;
static const uint32_t HELLO_TIMEOUT_MS  = 8000;    // VBUS up but no HELLO -> it's a PC
static const uint32_t REARM_LOW_MS      = 1000;    // VBUS must be low this long to re-arm
static const uint32_t WIFI_IDLE_OFF_MS  = 120000;  // radio off when calc program not running
static const uint32_t PORTAL_TIMEOUT_S  = 180;
static const size_t   MAX_LINE          = 512;
static const size_t   MAX_RAW_BODY      = 8192;
static const size_t   MAX_TEXT_OUT      = 2048;

static const char *HOSTNAME = "ti84-wifi";
static const char *VERSION  = "TI84-WIFI 1.0";

// ---- USB bus arbitration ----
//  RELEASED: ID floating, D+ pull-up off. The calc's mini-USB port behaves stock.
//  ARMED:    ID grounded (calc will be host), pull-up on, waiting for HELLO.
//  ACTIVE:   calc program said HELLO; serving commands.
enum LinkState { RELEASED, ARMED, ACTIVE };
static LinkState linkState = RELEASED;
static uint32_t stateSince = 0;
static uint32_t vbusLowSince = 0;
static uint32_t vbusHighSince = 0;
static uint32_t lastActivity = 0;

static Preferences prefs;
static WiFiManager wm;
static bool portalRunning = false;
static String lineBuf;

static bool vbusPresent() {
  return analogReadMilliVolts(PIN_VBUS) > VBUS_MV_THRESHOLD;
}

static void usbPullup(bool attach) {
  // With the override set and every pull disabled, the host sees no device.
  USB_SERIAL_JTAG.conf0.dp_pullup = 0;
  USB_SERIAL_JTAG.conf0.dm_pullup = 0;
  USB_SERIAL_JTAG.conf0.dp_pulldown = 0;
  USB_SERIAL_JTAG.conf0.dm_pulldown = 0;
  USB_SERIAL_JTAG.conf0.pad_pull_override = attach ? 0 : 1;
}

static void setState(LinkState s) {
  linkState = s;
  stateSince = millis();
  switch (s) {
    case RELEASED:
      usbPullup(false);
      digitalWrite(PIN_ID_FET, LOW);
      break;
    case ARMED:
      digitalWrite(PIN_ID_FET, HIGH);
      usbPullup(true);
      break;
    case ACTIVE:
      lastActivity = millis();
      break;
  }
}

static void updateLink() {
  uint32_t now = millis();
  bool vbus = vbusPresent();
  if (vbus) {
    vbusLowSince = 0;
    if (!vbusHighSince) vbusHighSince = now;
  } else {
    vbusHighSince = 0;
    if (!vbusLowSince) vbusLowSince = now;
  }

  switch (linkState) {
    case RELEASED:
      if (!vbus && now - vbusLowSince >= REARM_LOW_MS) setState(ARMED);
      break;
    case ARMED:
      // VBUS appeared but nobody said HELLO: a PC cable, not our program.
      if (vbus && now - vbusHighSince >= HELLO_TIMEOUT_MS) setState(RELEASED);
      break;
    case ACTIVE:
      // Calc program exited or crashed without BYE.
      if (!vbus && now - vbusLowSince >= REARM_LOW_MS) setState(ARMED);
      break;
  }
}

// ---- Output helpers. Every command ends with exactly one "OK ..." or "ERR ..." line. ----
static void ok(const String &msg = "") {
  Serial.print("OK");
  if (msg.length()) { Serial.print(' '); Serial.print(msg); }
  Serial.print('\n');
}

static void err(const String &msg) {
  Serial.print("ERR ");
  Serial.print(msg);
  Serial.print('\n');
}

static void dataLine(const String &s) {
  // A line that is exactly "OK..."/"ERR..." would confuse the calc; prefix it.
  if (s.startsWith("OK") || s.startsWith("ERR")) Serial.print(' ');
  Serial.print(s);
  Serial.print('\n');
}

// ---- Wi-Fi ----
static bool wifiUp(uint32_t timeoutMs = 10000) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin();  // credentials persisted in NVS by JOIN or the setup portal
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(100);
  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.begin();
    return true;
  }
  return false;
}

static void wifiOff() {
  if (portalRunning) return;
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
}

static void startPortal() {
  WiFi.mode(WIFI_AP_STA);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.startConfigPortal("TI84-Setup");
  portalRunning = true;
}

// ---- Text cleanup for the calculator's ASCII-only homescreen ----
static String htmlToText(const String &in, bool isHtml) {
  String out;
  out.reserve(MAX_TEXT_OUT);
  bool inTag = false, lastSpace = true, lastNl = true;
  for (size_t i = 0; i < in.length() && out.length() < MAX_TEXT_OUT; i++) {
    char c = in[i];
    if (isHtml) {
      if (c == '<') {
        // Skip <script>/<style> blocks entirely.
        if (in.startsWith("<script", i) || in.startsWith("<style", i)) {
          const char *closeTag = in.startsWith("<script", i) ? "</script>" : "</style>";
          int end = in.indexOf(closeTag, i);
          if (end < 0) break;
          i = end + strlen(closeTag) - 1;
          continue;
        }
        bool block = in.startsWith("<br", i) || in.startsWith("<p", i) ||
                     in.startsWith("</p", i) || in.startsWith("<li", i) ||
                     in.startsWith("<div", i) || in.startsWith("</div", i) ||
                     in.startsWith("<h", i) || in.startsWith("</h", i) ||
                     in.startsWith("<tr", i);
        if (block && !lastNl) { out += '\n'; lastNl = lastSpace = true; }
        inTag = true;
        continue;
      }
      if (inTag) { if (c == '>') inTag = false; continue; }
      if (c == '&') {
        static const struct { const char *ent; char ch; } ents[] = {
          {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'},
          {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", ' '}};
        bool matched = false;
        for (auto &e : ents) {
          if (in.startsWith(e.ent, i)) { c = e.ch; i += strlen(e.ent) - 1; matched = true; break; }
        }
        if (!matched) {
          int semi = in.indexOf(';', i);
          if (semi > 0 && semi - (int)i < 10) { i = semi; c = '?'; }
        }
      }
    }
    if (c == '\r') continue;
    if (c == '\n') {
      if (isHtml) c = ' ';
      else { if (!(lastNl && out.endsWith("\n\n"))) out += '\n'; lastNl = lastSpace = true; continue; }
    }
    if (c == '\t') c = ' ';
    if (c == ' ') {
      if (lastSpace) continue;
      lastSpace = true;
    } else {
      if (c < 0x20 || c > 0x7e) c = '?';
      lastSpace = false;
      lastNl = false;
    }
    out += c;
  }
  return out;
}

static void sendText(const String &text) {
  int start = 0;
  while (start <= (int)text.length()) {
    int nl = text.indexOf('\n', start);
    if (nl < 0) nl = text.length();
    String line = text.substring(start, nl);
    line.trim();
    if (line.length()) dataLine(line);
    start = nl + 1;
  }
}

// ---- Commands ----
static void cmdStatus() {
  bool up = WiFi.status() == WL_CONNECTED;
  dataLine(String(VERSION));
  dataLine(String("WiFi: ") + (up ? "connected" : "down"));
  if (up) {
    dataLine("SSID: " + WiFi.SSID());
    dataLine("IP: " + WiFi.localIP().toString());
    dataLine("RSSI: " + String(WiFi.RSSI()) + " dBm");
  } else if (WiFi.SSID().length()) {
    dataLine("Saved: " + WiFi.SSID());
  } else {
    dataLine("No saved network");
  }
  if (portalRunning) dataLine("Setup AP: TI84-Setup");
  ok();
}

static void cmdScan() {
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  if (n < 0) { err("scan failed"); return; }
  for (int i = 0; i < n && i < 20; i++) {
    String s = String(WiFi.RSSI(i)) + " ";
    s += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " " : "*";
    s += WiFi.SSID(i);
    dataLine(s);
  }
  WiFi.scanDelete();
  ok(String(n) + " found");
}

static void cmdJoin(const String &args) {
  int tab = args.indexOf('\t');
  String ssid = tab < 0 ? args : args.substring(0, tab);
  String pass = tab < 0 ? "" : args.substring(tab + 1);
  if (!ssid.length()) { err("usage: JOIN ssid<TAB>pass"); return; }
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(100);
  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.begin();
    ok(WiFi.localIP().toString());
  } else {
    err("could not connect");
  }
}

static void cmdGet(const String &url) {
  if (!url.startsWith("http://") && !url.startsWith("https://")) { err("url must start with http(s)://"); return; }
  if (!wifiUp()) { err("wifi down"); return; }

  WiFiClient plain;
  WiFiClientSecure secure;
  secure.setInsecure();  // no CA bundle on a calculator; good enough for reading pages
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  http.setUserAgent(VERSION);
  bool begun = url.startsWith("https://") ? http.begin(secure, url) : http.begin(plain, url);
  if (!begun) { err("bad url"); return; }

  const char *keys[] = {"Content-Type"};
  http.collectHeaders(keys, 1);
  int code = http.GET();
  if (code <= 0) { err(http.errorToString(code)); http.end(); return; }

  String raw;
  raw.reserve(MAX_RAW_BODY);
  WiFiClient *stream = http.getStreamPtr();
  int remaining = http.getSize();  // -1 when chunked/unknown
  uint32_t start = millis();
  while (http.connected() && raw.length() < MAX_RAW_BODY && (remaining != 0) && millis() - start < 10000) {
    while (stream->available() && raw.length() < MAX_RAW_BODY) {
      raw += (char)stream->read();
      if (remaining > 0) remaining--;
    }
    delay(1);
  }
  bool isHtml = http.header("Content-Type").indexOf("html") >= 0;
  http.end();

  sendText(htmlToText(raw, isHtml));
  ok("HTTP " + String(code));
}

static void cmdTime() {
  if (!wifiUp()) { err("wifi down"); return; }
  String tz = prefs.getString("tz", "UTC0");
  configTzTime(tz.c_str(), "pool.ntp.org", "time.nist.gov");
  struct tm t;
  if (!getLocalTime(&t, 8000)) { err("ntp timeout"); return; }
  char buf[32];
  strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &t);
  dataLine(buf);
  ok();
}

static void handleLine(String line) {
  line.trim();
  if (!line.length()) return;
  lastActivity = millis();

  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String args = sp < 0 ? "" : line.substring(sp + 1);
  cmd.toUpperCase();

  if (cmd == "HELLO") { setState(ACTIVE); ok(VERSION); return; }
  if (cmd == "PING") { ok("PONG"); return; }
  if (cmd == "STATUS") { cmdStatus(); return; }
  if (cmd == "SCAN") { cmdScan(); return; }
  if (cmd == "JOIN") { cmdJoin(args); return; }
  if (cmd == "FORGET") { WiFi.disconnect(true, true); ok(); return; }
  if (cmd == "SETUP") { startPortal(); ok("join AP TI84-Setup"); return; }
  if (cmd == "GET") { cmdGet(args); return; }
  if (cmd == "TIME") { cmdTime(); return; }
  if (cmd == "TZ") { prefs.putString("tz", args.length() ? args : "UTC0"); ok(); return; }
  if (cmd == "BYE") {
    ok();
    Serial.flush();
    delay(50);
    setState(RELEASED);
    return;
  }
  err("unknown command");
}

void setup() {
  pinMode(PIN_ID_FET, OUTPUT);
  digitalWrite(PIN_ID_FET, LOW);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  analogSetPinAttenuation(PIN_VBUS, ADC_11db);

  Serial.begin(115200);
  Serial.setTxTimeoutMs(50);  // never stall if the calc stops reading
  prefs.begin("ti84", false);

  ArduinoOTA.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  if (!WiFi.SSID().length()) startPortal();  // first boot: no saved network

  // Start released so a PC that is already plugged in is never disturbed.
  setState(RELEASED);
  vbusLowSince = vbusPresent() ? 0 : millis();
}

void loop() {
  updateLink();

  if (portalRunning) {
    wm.process();
    if (!wm.getConfigPortalActive()) portalRunning = false;
  }
  if (WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { handleLine(lineBuf); lineBuf = ""; }
    else if (lineBuf.length() < MAX_LINE) lineBuf += c;
  }

  // Save power while the calculator program is not running.
  if (linkState != ACTIVE && millis() - lastActivity > WIFI_IDLE_OFF_MS) wifiOff();

  // LED: solid = ACTIVE, slow blink = ARMED, off = RELEASED.
  bool led = linkState == ACTIVE || (linkState == ARMED && (millis() / 1000) % 4 == 0);
  digitalWrite(PIN_LED, led ? LOW : HIGH);

  delay(5);
}
