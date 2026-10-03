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
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <time.h>
// The C3 is wired to the calc's USB pads through its USB-Serial-JTAG port. A classic
// ESP32 dev board instead talks through its CH340 USB-UART on UART0 (external, on an
// OTG cable): no USB pull-up control, no ID/VBUS pins, and GPIO3 is its UART RX.
#if CONFIG_IDF_TARGET_ESP32C3
#define USB_BRIDGE_C3 1
#include "soc/usb_serial_jtag_struct.h"
#else
#define USB_BRIDGE_C3 0
#endif

// ---- Pins (ESP32-C3 Super Mini) ----
#if USB_BRIDGE_C3
static const int PIN_ID_FET = 3;  // N-MOSFET gate: HIGH grounds the calc's USB ID pin
static const int PIN_VBUS   = 4;  // VBUS through 100k/47k divider (5 V -> ~1.6 V)
static const int PIN_LED    = 8;  // onboard blue LED, active low
static const int LED_ON = LOW, LED_OFF = HIGH;
#else
static const int PIN_LED    = 2;  // DevKit onboard LED, active high
static const int LED_ON = HIGH, LED_OFF = LOW;
#endif

// ---- Tunables ----
static const uint32_t VBUS_MV_THRESHOLD = 1000;
static const uint32_t HELLO_TIMEOUT_MS  = 8000;    // VBUS up but no HELLO -> it's a PC
static const uint32_t REARM_LOW_MS      = 1000;    // VBUS must be low this long to re-arm
static const uint32_t WIFI_IDLE_OFF_MS  = 120000;  // radio off when calc program not running
static const uint32_t PORTAL_TIMEOUT_S  = 180;
static const size_t   MAX_LINE          = 512;
static const size_t   MAX_RAW_BODY      = 8192;
static const size_t   MAX_TEXT_OUT      = 2048;
static const size_t   WRAP_COLS         = 26;      // calc homescreen width
static const uint16_t ASK_FIRST_TRY_MS  = 10000;   // preferred model, before falling back
static const uint16_t ASK_TIMEOUT_MS    = 30000;   // fallback; both fit the calc's 60 s wait
static const uint16_t SNAP_TIMEOUT_MS   = 58000;   // Pi: photo + Gemini, with its own fallback
static const char *DEFAULT_PI_HOST      = "raspberry.local:8084";

static const char *GEMINI_DEFAULT_MODEL  = "gemini-3.5-flash";
static const char *GEMINI_FALLBACK_MODEL = "gemini-flash-lite-latest";
static const char *GEMINI_SYSTEM =
    "You answer on a TI-84 calculator screen, 26 characters wide. "
    "Lead with the answer itself on the first line. Then, only if it helps, "
    "up to 3 short lines of key steps or explanation, unless the user asks for more. "
    "Do not restate the question. No headings, labels or step numbers. "
    "No column alignment, padding spaces or lines of dashes. "
    "Plain ASCII only: no Markdown, LaTeX, emoji or tables. "
    "Write math inline, e.g. x^2+3x-4=0, sqrt(2), pi. Simplify fully.";

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
static uint32_t portalStarted = 0;
static String lineBuf;

static bool vbusPresent() {
#if USB_BRIDGE_C3
  return analogReadMilliVolts(PIN_VBUS) > VBUS_MV_THRESHOLD;
#else
  return false;
#endif
}

static void usbPullup(bool attach) {
  // With the override set and every pull disabled, the host sees no device.
  // dp_pullup must be restored on attach: clearing the override alone leaves D+ floating.
#if USB_BRIDGE_C3
  USB_SERIAL_JTAG.conf0.dp_pullup = attach ? 1 : 0;
  USB_SERIAL_JTAG.conf0.dm_pullup = 0;
  USB_SERIAL_JTAG.conf0.dp_pulldown = 0;
  USB_SERIAL_JTAG.conf0.dm_pulldown = 0;
  USB_SERIAL_JTAG.conf0.pad_pull_override = attach ? 0 : 1;
#else
  (void)attach;
#endif
}

static void setState(LinkState s) {
  linkState = s;
  stateSince = millis();
  switch (s) {
    case RELEASED:
      usbPullup(false);
#if USB_BRIDGE_C3
      digitalWrite(PIN_ID_FET, LOW);
#endif
      break;
    case ARMED:
#if USB_BRIDGE_C3
      digitalWrite(PIN_ID_FET, HIGH);
#endif
      usbPullup(true);
      break;
    case ACTIVE:
      lastActivity = millis();
      break;
  }
}

static void updateLink() {
#ifdef BENCH_MODE
  return;  // bench: VBUS sense isn't wired, stay attached to the PC
#endif
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
// WiFi.SSID() is only filled while connected; the saved network lives in the driver config,
// which can't be read while the radio is off (wifiOff), so keep the last value seen.
static String savedSsid() {
  static String cached;
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK)
    cached = String(reinterpret_cast<const char *>(conf.sta.ssid));
  return cached;
}

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
  portalStarted = millis();
}

static void stopPortal() {
  if (!portalRunning) return;
  if (wm.getConfigPortalActive()) wm.stopConfigPortal();
  WiFi.mode(WIFI_STA);
  portalRunning = false;
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

// Fold common UTF-8 punctuation to ASCII; anything else becomes one '?'.
static String asciiFold(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length();) {
    uint8_t c = in[i];
    if (c < 0x80) { out += (char)c; i++; continue; }
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    uint32_t cp = n == 4 ? c & 0x07 : n == 3 ? c & 0x0F : n == 2 ? c & 0x1F : 0;
    for (int k = 1; k < n && i + k < in.length(); k++) cp = (cp << 6) | (in[i + k] & 0x3F);
    i += n;
    switch (cp) {
      case 0x2018: case 0x2019: out += '\''; break;
      case 0x201C: case 0x201D: out += '"'; break;
      case 0x2013: case 0x2014: case 0x2212: out += '-'; break;
      case 0x2026: out += "..."; break;
      case 0x00A0: out += ' '; break;
      case 0x00D7: out += '*'; break;
      case 0x00F7: out += '/'; break;
      case 0x00B0: out += " deg"; break;
      default: out += '?';
    }
  }
  return out;
}

// One data line per screen row, broken at spaces where possible.
static void sendWrapped(String line) {
  while (line.length() > WRAP_COLS) {
    int cut = line.lastIndexOf(' ', WRAP_COLS);
    if (cut <= 0) cut = WRAP_COLS;
    dataLine(line.substring(0, cut));
    line = line.substring(cut);
    line.trim();
  }
  if (line.length()) dataLine(line);
}

static void sendText(const String &text) {
  int start = 0;
  while (start <= (int)text.length()) {
    int nl = text.indexOf('\n', start);
    if (nl < 0) nl = text.length();
    String line = text.substring(start, nl);
    line.trim();
    if (line.length()) sendWrapped(line);
    start = nl + 1;
  }
}

// ---- Commands ----
static void cmdStatus() {
  bool up = WiFi.status() == WL_CONNECTED;
  dataLine(String(VERSION));
  dataLine(String("WiFi: ") + (up ? "connected" : "down"));
  if (up) {
    dataLine("SSID: " + asciiFold(WiFi.SSID()));
    dataLine("IP: " + WiFi.localIP().toString());
    dataLine("RSSI: " + String(WiFi.RSSI()) + " dBm");
  } else if (savedSsid().length()) {
    dataLine("Saved: " + asciiFold(savedSsid()));
  } else {
    dataLine("No saved network");
  }
  dataLine(String("Gemini key: ") + (prefs.isKey("gkey") ? "set" : "not set"));
  if (portalRunning && (WiFi.getMode() & WIFI_MODE_AP)) dataLine("Setup AP: TI84-Setup");
  ok();
}

static void cmdScan() {
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  if (n < 0) { err("scan failed"); return; }
  for (int i = 0; i < n && i < 20; i++) {
    String s = String(WiFi.RSSI(i)) + " ";
    s += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " " : "*";
    s += asciiFold(WiFi.SSID(i));
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
  stopPortal();
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);

  // The calc can only type ASCII: match "Conner's iPhone" to a nearby
  // "Conner’s iPhone", ignoring apostrophe style and case.
  int n = WiFi.scanNetworks();
  String typed = asciiFold(ssid);
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid) break;
    if (asciiFold(WiFi.SSID(i)).equalsIgnoreCase(typed)) { ssid = WiFi.SSID(i); break; }
  }
  WiFi.scanDelete();

  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  // 12 s plus the ~3 s scan stays inside the calc's 20 s wait for JOIN.
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) delay(100);
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
  http.useHTTP10(true);  // no chunked encoding: the raw stream read below is the body as-is
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
  String tz = prefs.isKey("tz") ? prefs.getString("tz") : "UTC0";
  configTzTime(tz.c_str(), "pool.ntp.org", "time.nist.gov");
  struct tm t;
  if (!getLocalTime(&t, 8000)) { err("ntp timeout"); return; }
  char buf[32];
  strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &t);
  dataLine(buf);
  ok();
}

// ---- Gemini ----
// POSTs one generateContent request; returns the HTTP code (<= 0 on transport error).
static int geminiRequest(const String &model, const String &key, const String &body, uint16_t timeoutMs,
                         JsonDocument &resp, DeserializationError &jerr, String &transportErr) {
  WiFiClientSecure secure;
  secure.setInsecure();  // same trade-off as GET: no CA bundle on board
  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.useHTTP10(true);  // plain body so ArduinoJson can parse the stream directly
  String url = "https://generativelanguage.googleapis.com/v1beta/models/" + model + ":generateContent";
  if (!http.begin(secure, url)) { transportErr = "bad url"; return -1; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", key);
  int code = http.POST(body);
  if (code <= 0) { transportErr = http.errorToString(code); http.end(); return code; }

  // Keep only the answer text and any error message; the full reply can be large.
  JsonDocument filter;
  filter["candidates"][0]["content"]["parts"][0]["text"] = true;
  filter["candidates"][0]["finishReason"] = true;
  filter["error"]["message"] = true;
  jerr = deserializeJson(resp, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  return code;
}

static void cmdAsk(const String &question) {
  if (!question.length()) { err("usage: ASK question"); return; }
  String key = prefs.isKey("gkey") ? prefs.getString("gkey") : "";
  if (!key.length()) { err("no API key: send KEY <key>"); return; }
  if (!wifiUp()) { err("wifi down"); return; }
  String model = prefs.isKey("gmodel") ? prefs.getString("gmodel") : GEMINI_DEFAULT_MODEL;

  JsonDocument req;
  req["systemInstruction"]["parts"][0]["text"] = GEMINI_SYSTEM;
  req["contents"][0]["role"] = "user";
  req["contents"][0]["parts"][0]["text"] = question;
  req["generationConfig"]["maxOutputTokens"] = 2048;
  String body;
  serializeJson(req, body);

  JsonDocument resp;
  DeserializationError jerr;
  String transportErr;
  // Free-tier latency swings from ~1 s to 40+ s. Give the preferred model a short
  // window; if it's slow, busy, rate-limited or erroring, retry once on the
  // non-thinking lite model, which usually answers in about a second.
  int code = geminiRequest(model, key, body, ASK_FIRST_TRY_MS, resp, jerr, transportErr);
  bool retry = code <= 0 || code == 429 || code >= 500;
  if (retry && model != GEMINI_FALLBACK_MODEL) {
    resp.clear();
    code = geminiRequest(GEMINI_FALLBACK_MODEL, key, body, ASK_TIMEOUT_MS, resp, jerr, transportErr);
  }
  if (code <= 0) { err(transportErr); return; }

  if (code != 200) {
    String msg = resp["error"]["message"] | "";
    if (!msg.length()) msg = "HTTP " + String(code);
    err(asciiFold(msg).substring(0, 200));
    return;
  }
  if (jerr) { err(String("bad reply: ") + jerr.c_str()); return; }

  String answer;
  for (JsonVariant part : resp["candidates"][0]["content"]["parts"].as<JsonArray>()) {
    answer += part["text"] | "";
  }
  if (!answer.length()) {
    String why = resp["candidates"][0]["finishReason"] | "empty";
    err("no answer (" + why + ")");
    return;
  }
  sendText(htmlToText(asciiFold(answer), false));
  ok();
}

// ---- Pi camera (see ../pi/snap_server.py) ----
static void cmdSnap(const String &prompt) {
  if (!wifiUp()) { err("wifi down"); return; }
  String hostPort = prefs.isKey("pihost") ? prefs.getString("pihost") : DEFAULT_PI_HOST;
  int colon = hostPort.lastIndexOf(':');
  String host = colon < 0 ? hostPort : hostPort.substring(0, colon);
  uint16_t port = colon < 0 ? 80 : hostPort.substring(colon + 1).toInt();

  // The IDF resolver doesn't do mDNS; look .local names up ourselves.
  String addr = host;
  if (host.endsWith(".local")) {
    MDNS.begin(HOSTNAME);  // no-op if ArduinoOTA already started it
    IPAddress ip = MDNS.queryHost(host.substring(0, host.length() - 6), 3000);
    if (ip == IPAddress()) { err("Pi not found: " + host); return; }
    addr = ip.toString();
  }

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(SNAP_TIMEOUT_MS);
  http.useHTTP10(true);
  String url = "http://" + addr + ":" + String(port) + "/snap";
  int code = HTTPC_ERROR_CONNECTION_REFUSED;
  // A Pi that has just booted can be on Wi-Fi a few seconds before the service listens.
  for (int attempt = 0; attempt < 4 && code == HTTPC_ERROR_CONNECTION_REFUSED; attempt++) {
    if (attempt) delay(2500);
    if (!http.begin(client, url)) { err("bad Pi address"); return; }
    http.addHeader("Content-Type", "text/plain");
    code = http.POST(prompt);
    if (code <= 0) http.end();
  }
  if (code <= 0) { err("Pi: " + http.errorToString(code)); return; }
  String body = http.getString();
  http.end();

  if (code != 200) { err(asciiFold("Pi: " + body).substring(0, 200)); return; }
  sendText(htmlToText(asciiFold(body), false));
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
  if (cmd == "ASK") { cmdAsk(args); return; }
  if (cmd == "SNAP") { cmdSnap(args); return; }
  if (cmd == "PI") {
    if (args.length()) prefs.putString("pihost", args); else prefs.remove("pihost");
    ok(prefs.isKey("pihost") ? prefs.getString("pihost") : DEFAULT_PI_HOST);
    return;
  }
  if (cmd == "KEY") {
    if (args.length()) prefs.putString("gkey", args); else prefs.remove("gkey");
    ok(args.length() ? "key saved" : "key cleared");
    return;
  }
  if (cmd == "MODEL") {
    if (args.length()) prefs.putString("gmodel", args); else prefs.remove("gmodel");
    ok(prefs.isKey("gmodel") ? prefs.getString("gmodel") : GEMINI_DEFAULT_MODEL);
    return;
  }
  if (cmd == "TZ") { prefs.putString("tz", args.length() ? args : "UTC0"); ok(); return; }
  if (cmd == "BYE") {
    ok();
    Serial.flush();
    delay(50);
#ifndef BENCH_MODE
    setState(RELEASED);
#endif
    return;
  }
  err("unknown command");
}

void setup() {
#if USB_BRIDGE_C3
  pinMode(PIN_ID_FET, OUTPUT);
  digitalWrite(PIN_ID_FET, LOW);
  analogSetPinAttenuation(PIN_VBUS, ADC_11db);
#endif
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LED_OFF);

  Serial.begin(115200);
#if USB_BRIDGE_C3
  Serial.setTxTimeoutMs(50);  // never stall if the calc stops reading
#endif
  // Serial is the calc's protocol link: keep library and IDF logs off it.
  Serial.setDebugOutput(false);
  esp_log_level_set("*", ESP_LOG_NONE);
  wm.setDebugOutput(false);
  prefs.begin("ti84", false);

  ArduinoOTA.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  if (!savedSsid().length()) startPortal();  // first boot: no saved network

#ifdef BENCH_MODE
  setState(ARMED);  // pull-up on so the PC sees the serial port
#else
  // Only step aside if a PC cable is already plugged in. Otherwise stay attached:
  // a C3 powered by the calc's own VBUS boots when WIFI calls usb_Init, and
  // detaching here dropped that first enumeration ("No bridge found").
  setState(vbusPresent() ? RELEASED : ARMED);
#endif
  vbusLowSince = vbusPresent() ? 0 : millis();
}

void loop() {
  updateLink();

  if (portalRunning) {
    wm.process();
    if (!wm.getConfigPortalActive() || millis() - portalStarted > PORTAL_TIMEOUT_S * 1000UL) stopPortal();
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
  digitalWrite(PIN_LED, led ? LED_ON : LED_OFF);

  delay(5);
}
