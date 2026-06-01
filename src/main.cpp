#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#if defined(CYD_TOUCH_ENABLED)
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#endif
#include <time.h>
#include "version.h"

constexpr const char *kPortalSsid = "Bins Display Setup";
constexpr const char *kDefaultPostcode = "AL15SR";
constexpr const char *kDefaultUprn = "10001062494";
constexpr const char *kEndpoint =
    "https://gis.stalbans.gov.uk/NoticeBoard9/VeoliaProxy.NoticeBoard.asmx/"
    "GetServicesByUprnAndNoticeBoard";
constexpr const char *kQuickSearchEndpoint = "https://gis.stalbans.gov.uk/NoticeBoard9/quicksearch.asmx";
constexpr uint32_t kFetchIntervalMs = 6UL * 60UL * 60UL * 1000UL;
constexpr uint8_t kDisplayModeCount = 5;
#ifndef TFT_ROTATION
#define TFT_ROTATION 1
#endif
#if defined(CHEAP_YELLOW_DISPLAY)
constexpr int kButtonLeft = 0;
constexpr int kButtonRight = -1;
#else
constexpr int kButtonLeft = 0;
constexpr int kButtonRight = 35;
#endif

TFT_eSPI tft;
Preferences prefs;
#if defined(CYD_TOUCH_ENABLED)
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, CYD_TOUCH_IRQ);
#endif

struct ServiceDate {
  String name;
  String label;
  String iso;
  String date;
  String schedule;
  time_t dayStart = 0;
  bool valid = false;
};

struct AppConfig {
  String postcode = kDefaultPostcode;
  String uprn = kDefaultUprn;
  bool showStatusWhenIdle = true;
  uint8_t displayMode = 0;
};

struct BinState {
  ServiceDate refuse;
  ServiceDate recycling;
  ServiceDate food;
  ServiceDate garden;
  String lastError;
  time_t lastFetch = 0;
  bool loaded = false;
};

AppConfig config;
BinState bins;
uint32_t lastFetchAttemptMs = 0;
uint32_t lastDrawMs = 0;
bool lastLeft = true;
bool lastRight = true;
bool previewAlert = false;
bool rightLongHandled = false;
uint32_t rightPressedAtMs = 0;
#if defined(CHEAP_YELLOW_DISPLAY)
bool leftLongHandled = false;
uint32_t leftPressedAtMs = 0;
#endif
#if defined(CYD_TOUCH_ENABLED)
bool lastTouch = false;
bool touchLongHandled = false;
bool touchRightSide = false;
uint32_t touchPressedAtMs = 0;
#endif

String xmlEscape(const String &value) {
  String out;
  out.reserve(value.length());
  for (char c : value) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += c; break;
    }
  }
  return out;
}

String compactPostcode(String value) {
  value.trim();
  value.toUpperCase();
  value.replace(" ", "");
  return value;
}

time_t parseIsoDateStart(const String &iso) {
  if (iso.length() < 10) return 0;
  tm value = {};
  value.tm_year = iso.substring(0, 4).toInt() - 1900;
  value.tm_mon = iso.substring(5, 7).toInt() - 1;
  value.tm_mday = iso.substring(8, 10).toInt();
  value.tm_hour = 0;
  value.tm_min = 0;
  value.tm_sec = 0;
  value.tm_isdst = -1;
  return mktime(&value);
}

String shortDate(const ServiceDate &svc) {
  if (!svc.valid) return "--";
  char buf[24];
  tm *lt = localtime(&svc.dayStart);
  strftime(buf, sizeof(buf), "%a %d %b", lt);
  return String(buf);
}

String dateOnly(const String &iso) {
  return iso.length() >= 10 ? iso.substring(0, 10) : "";
}

String tagValueAfter(const String &source, const String &tag, int startAt) {
  const String openTag = "<" + tag + ">";
  const String closeTag = "</" + tag + ">";
  int start = source.indexOf(openTag, startAt);
  if (start < 0) return "";
  start += openTag.length();
  int end = source.indexOf(closeTag, start);
  if (end < 0) return "";
  return source.substring(start, end);
}

String firstUprnFromQuickSearchXml(const String &xml) {
  int pos = 0;
  while (true) {
    int columnStart = xml.indexOf("<Column>", pos);
    if (columnStart < 0) return "";
    int columnEnd = xml.indexOf("</Column>", columnStart);
    if (columnEnd < 0) return "";
    String column = xml.substring(columnStart, columnEnd);
    if (tagValueAfter(column, "Name", 0) == "UPRN") {
      String uprn = tagValueAfter(column, "Value", 0);
      uprn.trim();
      return uprn;
    }
    pos = columnEnd + 9;
  }
}

void drawText(int x, int y, const String &text, uint16_t color, uint8_t font = 2) {
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(text, x, y, font);
}

void drawCentered(int y, const String &text, uint16_t color, uint8_t font = 4) {
  tft.setTextColor(color, TFT_BLACK);
  tft.drawCentreString(text, tft.width() / 2, y, font);
}

bool largeScreen() {
  return tft.height() >= 200;
}

int bottomY(int offset = 16) {
  return max(0, tft.height() - offset);
}

bool readButton(int pin) {
  if (pin < 0) return true;
  return digitalRead(pin);
}

void disableUnusedSpiDevices() {
#if defined(CYD_SD_CS)
  pinMode(CYD_SD_CS, OUTPUT);
  digitalWrite(CYD_SD_CS, HIGH);
#endif
#if defined(TOUCH_CS)
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
#endif
}

#if defined(CYD_TOUCH_ENABLED)
void configureTouch() {
  touchSpi.begin(CYD_TOUCH_CLK, CYD_TOUCH_MISO, CYD_TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1);
}

bool readCydTouch(uint16_t *x, uint16_t *y) {
  if (!touch.touched()) return false;
  TS_Point p = touch.getPoint();
  *x = constrain(map(p.x, 200, 3900, 0, tft.width() - 1), 0, tft.width() - 1);
  *y = constrain(map(p.y, 200, 3900, 0, tft.height() - 1), 0, tft.height() - 1);
  return true;
}
#endif

String localTimeText(const char *fmt) {
  time_t now = time(nullptr);
  if (now < 1700000000) return "--";
  tm nowInfo = *localtime(&now);
  char buf[32];
  strftime(buf, sizeof(buf), fmt, &nowInfo);
  return String(buf);
}

time_t todayStart() {
  time_t now = time(nullptr);
  if (now < 1700000000) return 0;
  tm today = *localtime(&now);
  today.tm_hour = 0;
  today.tm_min = 0;
  today.tm_sec = 0;
  today.tm_isdst = -1;
  return mktime(&today);
}

int daysUntil(const ServiceDate &svc) {
  if (!svc.valid) return 9999;
  return static_cast<int>((svc.dayStart - todayStart()) / 86400);
}

bool sameDate(const ServiceDate &a, const ServiceDate &b) {
  return a.valid && b.valid && a.date == b.date;
}

bool inAlertWindow(const ServiceDate &svc) {
  if (!svc.valid) return false;
  const int diff = daysUntil(svc);
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  tm nowInfo = *localtime(&now);
  if (diff == 1 && nowInfo.tm_hour >= 18) return true;
  if (diff == 0 && nowInfo.tm_hour < 12) return true;
  return false;
}

void waitForClock() {
  for (int i = 0; i < 20; i++) {
    if (time(nullptr) >= 1700000000) return;
    delay(500);
  }
}

void loadConfig() {
  prefs.begin("bins", false);
  config.postcode = prefs.getString("postcode", kDefaultPostcode);
  config.uprn = prefs.getString("uprn", kDefaultUprn);
  config.showStatusWhenIdle = prefs.getBool("idleStatus", true);
  config.displayMode = prefs.getUChar("displayMode", 0) % kDisplayModeCount;

  bins.refuse.date = prefs.getString("refuseDate", "");
  bins.recycling.date = prefs.getString("recycleDate", "");
  bins.food.date = prefs.getString("foodDate", "");
  bins.garden.date = prefs.getString("gardenDate", "");
  bins.lastFetch = prefs.getLong64("lastFetch", 0);

  auto hydrate = [](ServiceDate &svc, const String &name, const String &label) {
    svc.name = name;
    svc.label = label;
    svc.iso = svc.date;
    svc.dayStart = parseIsoDateStart(svc.date);
    svc.valid = svc.dayStart > 0;
  };
  hydrate(bins.refuse, "Domestic Refuse Collection", "Refuse");
  hydrate(bins.recycling, "Domestic Recycling Collection", "Recycling");
  hydrate(bins.food, "Domestic Food Collection", "Food");
  hydrate(bins.garden, "Domestic Garden Waste Collection", "Garden");
  bins.loaded = bins.refuse.valid || bins.recycling.valid || bins.food.valid || bins.garden.valid;
}

void saveConfig() {
  prefs.putString("postcode", compactPostcode(config.postcode));
  prefs.putString("uprn", config.uprn);
  prefs.putBool("idleStatus", config.showStatusWhenIdle);
  prefs.putUChar("displayMode", config.displayMode % kDisplayModeCount);
}

void saveBinCache() {
  prefs.putString("refuseDate", bins.refuse.date);
  prefs.putString("recycleDate", bins.recycling.date);
  prefs.putString("foodDate", bins.food.date);
  prefs.putString("gardenDate", bins.garden.date);
  prefs.putLong64("lastFetch", bins.lastFetch);
}

bool lookupUprnForPostcode(const String &postcode, String &uprnOut) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, kQuickSearchEndpoint)) return false;

  String body =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<soap:Envelope xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
      "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
      "xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
      "<soap:Body>"
      "<GetMoreResults xmlns=\"http://tempuri.org/\">"
      "<filter>" + xmlEscape(postcode) + "</filter>"
      "<startIndex>0</startIndex>"
      "<endIndex>1</endIndex>"
      "<searchId>4</searchId>"
      "</GetMoreResults>"
      "</soap:Body>"
      "</soap:Envelope>";

  http.addHeader("Content-Type", "text/xml; charset=utf-8");
  http.addHeader("SOAPAction", "\"http://tempuri.org/GetMoreResults\"");
  int status = http.POST(body);
  String response = http.getString();
  http.end();

  if (status != 200) return false;
  String uprn = firstUprnFromQuickSearchXml(response);
  if (uprn.length() == 0) return false;
  uprnOut = uprn;
  return true;
}

void showSetupScreen() {
  tft.fillScreen(TFT_BLACK);
  drawCentered(largeScreen() ? 36 : 18, "Bins Display", TFT_CYAN, 4);
  drawCentered(largeScreen() ? 92 : 64, "Setup Wi-Fi", TFT_WHITE, 4);
  drawCentered(largeScreen() ? 146 : 106, kPortalSsid, TFT_YELLOW, 2);
  drawCentered(largeScreen() ? 178 : 136, "Open 192.168.4.1", TFT_LIGHTGREY, 2);
}

void resetStoredSetup(WiFiManager &wm) {
  wm.resetSettings();
  prefs.clear();
  config.postcode = kDefaultPostcode;
  config.uprn = kDefaultUprn;
  config.showStatusWhenIdle = true;
  config.displayMode = 0;
  saveConfig();
}

void connectWifi(bool forcePortal) {
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(0);
  wm.setTitle("Bins Display Setup");

  if (forcePortal) {
    resetStoredSetup(wm);
  }

  char postcode[16];
  char uprn[24];
  char idleStatus[4];
  strlcpy(postcode, config.postcode.c_str(), sizeof(postcode));
  strlcpy(uprn, config.uprn.c_str(), sizeof(uprn));
  strlcpy(idleStatus, config.showStatusWhenIdle ? "1" : "0", sizeof(idleStatus));

  WiFiManagerParameter postcodeParam("postcode", "Postcode", postcode, sizeof(postcode));
  WiFiManagerParameter uprnParam("uprn", "UPRN", uprn, sizeof(uprn));
  WiFiManagerParameter statusParam("idleStatus", "Idle status screen 1=yes 0=no", idleStatus, sizeof(idleStatus));
  wm.addParameter(&postcodeParam);
  wm.addParameter(&uprnParam);
  wm.addParameter(&statusParam);

  const String previousPostcode = compactPostcode(config.postcode);
  showSetupScreen();
  bool connected = forcePortal ? wm.startConfigPortal(kPortalSsid) : wm.autoConnect(kPortalSsid);
  if (!connected) {
    ESP.restart();
  }

  config.postcode = compactPostcode(postcodeParam.getValue());
  config.uprn = String(uprnParam.getValue());
  config.uprn.trim();
  config.showStatusWhenIdle = String(statusParam.getValue()) != "0";

  if (config.postcode.length() > 0 && (config.postcode != previousPostcode || config.uprn.length() == 0)) {
    tft.fillScreen(TFT_BLACK);
    drawCentered(42, "Finding UPRN", TFT_CYAN, 4);
    drawCentered(82, config.postcode, TFT_WHITE, 4);
    String foundUprn;
    if (lookupUprnForPostcode(config.postcode, foundUprn)) {
      config.uprn = foundUprn;
      drawCentered(116, "Found", TFT_GREEN, 2);
      delay(900);
    } else {
      drawCentered(116, "UPRN unchanged", TFT_YELLOW, 2);
      delay(1200);
    }
  }
  saveConfig();
}

bool parseService(JsonObject svc, ServiceDate &target, const String &label) {
  target.name = svc["ServiceName"] | "";
  target.label = label;
  JsonArray headers = svc["ServiceHeaders"].as<JsonArray>();
  if (headers.isNull() || headers.size() == 0) return false;
  JsonObject first = headers[0];
  target.iso = first["Next"] | "";
  target.date = dateOnly(target.iso);
  target.schedule = first["ScheduleDescription"] | "";
  target.dayStart = parseIsoDateStart(target.date);
  target.valid = target.dayStart > 0;
  return target.valid;
}

bool fetchBins() {
  if (WiFi.status() != WL_CONNECTED) {
    bins.lastError = "Wi-Fi offline";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, kEndpoint)) {
    bins.lastError = "HTTP setup failed";
    return false;
  }

  http.addHeader("Content-Type", "application/json; charset=UTF-8");
  http.addHeader("X-Requested-With", "XMLHttpRequest");
  String payload = "{\"uprn\":" + config.uprn + ",\"noticeBoard\":\"default\"}";
  int status = http.POST(payload);
  String response = http.getString();
  http.end();

  if (status != 200) {
    bins.lastError = "HTTP " + String(status);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    bins.lastError = "JSON parse failed";
    return false;
  }

  JsonArray services = doc["d"].as<JsonArray>();
  if (services.isNull()) {
    bins.lastError = "No service data";
    return false;
  }

  for (JsonObject svc : services) {
    String name = svc["ServiceName"] | "";
    if (name == "Domestic Refuse Collection") parseService(svc, bins.refuse, "Refuse");
    if (name == "Domestic Recycling Collection") parseService(svc, bins.recycling, "Recycling");
    if (name == "Domestic Food Collection") parseService(svc, bins.food, "Food");
    if (name == "Domestic Garden Waste Collection") parseService(svc, bins.garden, "Garden");
  }

  bins.lastError = "";
  bins.lastFetch = time(nullptr);
  bins.loaded = true;
  saveBinCache();
  return true;
}

ServiceDate nextMainCollection() {
  if (bins.refuse.valid && (!bins.recycling.valid || bins.refuse.dayStart < bins.recycling.dayStart)) {
    return bins.refuse;
  }
  return bins.recycling;
}

void orderedMainCollections(ServiceDate &first, ServiceDate &second) {
  if (!bins.refuse.valid) {
    first = bins.recycling;
    second = bins.refuse;
    return;
  }
  if (!bins.recycling.valid) {
    first = bins.refuse;
    second = bins.recycling;
    return;
  }
  if (bins.refuse.valid && bins.recycling.valid && bins.refuse.dayStart <= bins.recycling.dayStart) {
    first = bins.refuse;
    second = bins.recycling;
    return;
  }
  first = bins.recycling;
  second = bins.refuse;
}

uint16_t accentFor(const ServiceDate &svc) {
  return svc.label == "Refuse" ? TFT_ORANGE : TFT_GREEN;
}

String countdownText(const ServiceDate &svc) {
  const int diff = daysUntil(svc);
  if (diff == 0) return "Today";
  if (diff == 1) return "Tomorrow";
  if (diff >= 9999) return "--";
  return String(diff) + " days";
}

String friendlyDay(const ServiceDate &svc) {
  const int diff = daysUntil(svc);
  if (diff == 0) return "Today";
  if (diff == 1) return "Tomorrow";
  if (!svc.valid) return "--";
  char buf[12];
  tm *lt = localtime(&svc.dayStart);
  strftime(buf, sizeof(buf), "%A", lt);
  return String(buf);
}

String conciseDate(const ServiceDate &svc) {
  if (!svc.valid) return "--";
  char buf[16];
  tm *lt = localtime(&svc.dayStart);
  strftime(buf, sizeof(buf), "%d %b", lt);
  return String(buf);
}

String weekdayOnly(const ServiceDate &svc) {
  if (!svc.valid) return "--";
  char buf[12];
  tm *lt = localtime(&svc.dayStart);
  strftime(buf, sizeof(buf), "%a", lt);
  return String(buf);
}

String putOutLabel(const ServiceDate &svc) {
  if (svc.label == "Recycling") return "Recycling";
  if (sameDate(svc, bins.food)) return "Refuse + Food";
  return "Refuse";
}

ServiceDate alertCollection() {
  if (inAlertWindow(bins.refuse)) return bins.refuse;
  if (inAlertWindow(bins.recycling)) return bins.recycling;
  return ServiceDate();
}

String footerText(bool detailed) {
  if (detailed) {
    return WiFi.status() == WL_CONNECTED ? WiFi.SSID() + " " + String(WiFi.RSSI()) + "dBm" : "Wi-Fi offline";
  }
  if (bins.lastError.length()) return bins.lastError;
  return "Updated " + localTimeText("%H:%M");
}

void drawFooter(bool detailed) {
  drawText(6, bottomY(16), footerText(detailed).substring(0, 42), TFT_LIGHTGREY, 2);
}

void drawModeDot() {
  const int firstX = tft.width() - 44;
  const int y = bottomY(11);
  for (int i = 0; i < kDisplayModeCount; i++) {
    tft.fillCircle(firstX + (i * 9), y, 2, i == config.displayMode ? TFT_CYAN : TFT_DARKGREY);
  }
}

String compactTimeText(time_t value) {
  if (value < 1700000000) return "Never";
  tm info = *localtime(&value);
  char buf[24];
  strftime(buf, sizeof(buf), "%a %H:%M", &info);
  return String(buf);
}

String uptimeText() {
  uint32_t totalSeconds = millis() / 1000UL;
  uint32_t hours = totalSeconds / 3600UL;
  uint32_t minutes = (totalSeconds % 3600UL) / 60UL;
  if (hours > 0) return String(hours) + "h " + String(minutes) + "m";
  return String(minutes) + "m";
}

String wifiSummary() {
  if (WiFi.status() != WL_CONNECTED) return "Offline";
  return WiFi.SSID() + " " + String(WiFi.RSSI()) + "dBm";
}

String alertLabel() {
  String label;
  if (inAlertWindow(bins.refuse)) {
    label = putOutLabel(bins.refuse);
  }
  if (inAlertWindow(bins.recycling)) {
    if (label.length()) label += " / ";
    label += putOutLabel(bins.recycling);
  }
  return label;
}

void drawFocusAlert(const ServiceDate &svc) {
  tft.fillScreen(TFT_BLACK);
  if (largeScreen()) {
    drawCentered(24, "TONIGHT", TFT_RED, 4);
    drawCentered(72, putOutLabel(svc), TFT_WHITE, 4);
    drawCentered(128, "Put out by 6am", TFT_YELLOW, 4);
    drawCentered(184, localTimeText("%H:%M"), TFT_CYAN, 6);
    return;
  }
  drawCentered(2, "TONIGHT", TFT_RED, 4);
  drawCentered(32, putOutLabel(svc), TFT_WHITE, 4);
  drawCentered(74, "Put out by 6am", TFT_YELLOW, 4);
  drawCentered(108, localTimeText("%H:%M"), TFT_CYAN, 2);
}

void drawFocusLayout(bool detailed) {
  ServiceDate first;
  ServiceDate second;
  orderedMainCollections(first, second);
  tft.fillScreen(TFT_BLACK);
  const int lineY = largeScreen() ? 82 : 55;
  const int rowY = largeScreen() ? 96 : 62;
  const int dateY = largeScreen() ? 136 : 95;
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(localTimeText("%H:%M"), 6, largeScreen() ? 8 : 0, 6);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%a %d %b"), tft.width() - 8, largeScreen() ? 18 : 8, 2);

  tft.drawFastHLine(6, lineY, tft.width() - 12, accentFor(first));
  drawText(8, rowY, first.label, accentFor(first), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(friendlyDay(first), tft.width() - 8, rowY, 4);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawRightString(conciseDate(first), tft.width() - 8, dateY, 2);
  drawText(8, dateY, "Next collection", TFT_LIGHTGREY, 2);

  if (second.valid) {
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(second.label + " " + conciseDate(second), 8, largeScreen() ? 154 : 116, 2);
  }
  if (largeScreen()) {
    drawFooter(detailed);
  }
  if (detailed) drawModeDot();
}

void drawStackAlert(const ServiceDate &svc) {
  tft.fillScreen(TFT_BLACK);
  if (largeScreen()) {
    drawText(10, 12, "Put out", TFT_RED, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawRightString(localTimeText("%H:%M"), tft.width() - 10, 17, 4);
    tft.drawFastHLine(12, 60, tft.width() - 24, TFT_RED);
    drawCentered(88, putOutLabel(svc), TFT_WHITE, 4);
    drawCentered(142, "Tonight", TFT_YELLOW, 4);
    drawModeDot();
    return;
  }
  drawText(6, 2, "Put out", TFT_RED, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 8, 7, 4);
  tft.drawFastHLine(8, 41, tft.width() - 16, TFT_RED);
  drawCentered(51, putOutLabel(svc), TFT_WHITE, 4);
  drawCentered(91, "Tonight", TFT_YELLOW, 4);
  drawModeDot();
}

void drawStackLayout(bool detailed) {
  ServiceDate first;
  ServiceDate second;
  orderedMainCollections(first, second);
  tft.fillScreen(TFT_BLACK);
  const int firstY = largeScreen() ? 72 : 37;
  const int dividerY = largeScreen() ? 125 : 75;
  const int secondY = largeScreen() ? 145 : 82;
  drawText(6, largeScreen() ? 12 : 2, localTimeText("%a %d"), TFT_CYAN, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 8, largeScreen() ? 17 : 7, 4);

  drawText(8, firstY + 6, "1", TFT_LIGHTGREY, 2);
  drawText(28, firstY, first.label, accentFor(first), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(shortDate(first), tft.width() - 8, firstY + 2, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawRightString(countdownText(first), tft.width() - 8, firstY + 19, 2);

  tft.drawFastHLine(8, dividerY, tft.width() - 16, TFT_DARKGREY);
  drawText(8, secondY + 6, "2", TFT_LIGHTGREY, 2);
  drawText(28, secondY, second.label, accentFor(second), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(shortDate(second), tft.width() - 8, secondY + 2, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawRightString(countdownText(second), tft.width() - 8, secondY + 19, 2);
  if (detailed) drawModeDot();
}

void drawTimelineAlert(const ServiceDate &svc) {
  tft.fillScreen(TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.drawCentreString("TONIGHT", tft.width() / 2, largeScreen() ? 32 : 6, 4);
  tft.drawCentreString(putOutLabel(svc), tft.width() / 2, largeScreen() ? 88 : 44, 4);
  tft.setTextColor(TFT_YELLOW, TFT_RED);
  tft.drawCentreString("Before 6am", tft.width() / 2, largeScreen() ? 150 : 91, 4);
}

void drawClockLayout(bool detailed) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  const String timeText = localTimeText("%H:%M");
  tft.drawCentreString(timeText == "--" ? "Syncing" : timeText, tft.width() / 2, largeScreen() ? 28 : 6, timeText == "--" ? 4 : 6);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString(localTimeText("%A"), tft.width() / 2, largeScreen() ? 110 : 70, 4);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawCentreString(localTimeText("%d %B"), tft.width() / 2, largeScreen() ? 152 : 101, 4);
  if (detailed) drawModeDot();
}

void drawBoldAlert(const ServiceDate &svc) {
  tft.fillScreen(TFT_BLACK);
  const int headerHeight = largeScreen() ? 42 : 28;
  tft.fillRect(0, 0, tft.width(), headerHeight, TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.drawCentreString("BINS TONIGHT", tft.width() / 2, largeScreen() ? 10 : 4, 4);
  drawCentered(largeScreen() ? 66 : 42, putOutLabel(svc), accentFor(svc), 4);
  drawCentered(largeScreen() ? 118 : 76, "Put out", TFT_WHITE, 4);
  drawCentered(largeScreen() ? 162 : 105, "before 6am", TFT_YELLOW, largeScreen() ? 4 : 2);
}

void drawBoldLayout(bool detailed) {
  ServiceDate first = nextMainCollection();
  ServiceDate second = first.label == "Refuse" ? bins.recycling : bins.refuse;
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, largeScreen() ? 12 : 8, tft.height(), accentFor(first));
  drawText(largeScreen() ? 22 : 16, largeScreen() ? 14 : 4, first.label, accentFor(first), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 8, largeScreen() ? 16 : 6, 4);
  drawCentered(largeScreen() ? 78 : 43, shortDate(first), TFT_WHITE, 4);
  drawCentered(largeScreen() ? 130 : 78, countdownText(first), TFT_YELLOW, 4);
  if (second.valid) {
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawCentreString("Then " + second.label + " " + conciseDate(second), tft.width() / 2, largeScreen() ? 190 : 112, 2);
  }
  if (detailed) drawModeDot();
}

void drawStatusLayout() {
  tft.fillScreen(TFT_BLACK);
  const int rowGap = largeScreen() ? 25 : 18;
  const int startY = largeScreen() ? 58 : 35;
  drawText(6, largeScreen() ? 12 : 0, "Bins v" + String(FIRMWARE_VERSION), TFT_CYAN, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 8, largeScreen() ? 18 : 6, 4);

  drawText(8, startY, "Wi-Fi", TFT_LIGHTGREY, 2);
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.drawString(wifiSummary().substring(0, largeScreen() ? 34 : 24), 82, startY, 2);

  drawText(8, startY + rowGap, "IP", TFT_LIGHTGREY, 2);
  drawText(82, startY + rowGap, WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "--", TFT_WHITE, 2);

  drawText(8, startY + (rowGap * 2), "Postcode", TFT_LIGHTGREY, 2);
  drawText(82, startY + (rowGap * 2), compactPostcode(config.postcode), TFT_WHITE, 2);

  drawText(8, startY + (rowGap * 3), "UPRN", TFT_LIGHTGREY, 2);
  drawText(82, startY + (rowGap * 3), config.uprn.substring(0, largeScreen() ? 24 : 18), TFT_WHITE, 2);

  drawText(8, startY + (rowGap * 4), "Fetch", TFT_LIGHTGREY, 2);
  drawText(82, startY + (rowGap * 4), compactTimeText(bins.lastFetch), bins.lastError.length() ? TFT_YELLOW : TFT_WHITE, 2);

  const String health = bins.lastError.length() ? bins.lastError : "OK";
  tft.setTextColor(bins.lastError.length() ? TFT_YELLOW : TFT_GREEN, TFT_BLACK);
  tft.drawString(("Up " + uptimeText() + " " + health).substring(0, largeScreen() ? 36 : 24), 8, bottomY(18), 2);
  drawModeDot();
}

void drawNormalLayout(bool detailed) {
  if (config.displayMode == 0) drawFocusLayout(detailed);
  if (config.displayMode == 1) drawStackLayout(detailed);
  if (config.displayMode == 2) drawClockLayout(detailed);
  if (config.displayMode == 3) drawBoldLayout(detailed);
  if (config.displayMode == 4) drawStatusLayout();
}

void drawAlertLayout(const ServiceDate &svc) {
  if (config.displayMode == 0) drawFocusAlert(svc);
  if (config.displayMode == 1) drawStackAlert(svc);
  if (config.displayMode == 2) drawTimelineAlert(svc);
  if (config.displayMode == 3) drawBoldAlert(svc);
  if (config.displayMode == 4) drawFocusAlert(svc);
}

void drawScreen() {
  ServiceDate alert = previewAlert ? nextMainCollection() : alertCollection();
  if (alert.valid) {
    drawAlertLayout(alert);
    return;
  }
  drawNormalLayout(config.showStatusWhenIdle);
}

void cycleDisplayMode() {
  config.displayMode = (config.displayMode + 1) % kDisplayModeCount;
  saveConfig();
  drawScreen();
}

void refreshCollections() {
  lastFetchAttemptMs = 0;
  drawCentered(bottomY(19), "Refreshing...", TFT_YELLOW, 2);
  fetchBins();
  drawScreen();
}

#if defined(CYD_TOUCH_ENABLED)
void handleTouch() {
  uint16_t x = 0;
  uint16_t y = 0;
  const bool touched = readCydTouch(&x, &y);

  if (touched && !lastTouch) {
    touchPressedAtMs = millis();
    touchLongHandled = false;
    touchRightSide = x >= (tft.width() / 2);
  }

  if (touched && touchRightSide && !touchLongHandled && millis() - touchPressedAtMs > 900UL) {
    previewAlert = !previewAlert;
    touchLongHandled = true;
    drawScreen();
  }

  if (!touched && lastTouch && !touchLongHandled) {
    if (touchRightSide) {
      refreshCollections();
    } else {
      cycleDisplayMode();
    }
  }

  lastTouch = touched;
}
#endif

void handleButtons() {
  bool left = readButton(kButtonLeft);
  bool right = readButton(kButtonRight);
#if defined(CHEAP_YELLOW_DISPLAY)
  if (!left && lastLeft) {
    leftPressedAtMs = millis();
    leftLongHandled = false;
  }

  if (!left && !leftLongHandled && millis() - leftPressedAtMs > 900UL) {
    refreshCollections();
    leftLongHandled = true;
  }

  if (left && !lastLeft && !leftLongHandled) {
    cycleDisplayMode();
  }
#else
  if (!left && lastLeft) {
    cycleDisplayMode();
  }
#endif

  if (!right && lastRight) {
    rightPressedAtMs = millis();
    rightLongHandled = false;
  }

  if (!right && !rightLongHandled && millis() - rightPressedAtMs > 900UL) {
    previewAlert = !previewAlert;
    rightLongHandled = true;
    drawScreen();
  }

  if (right && !lastRight && !rightLongHandled) {
    refreshCollections();
  }
  lastLeft = left;
  lastRight = right;

#if defined(CYD_TOUCH_ENABLED)
  handleTouch();
#endif
}

void setup() {
  Serial.begin(115200);
  if (kButtonLeft >= 0) pinMode(kButtonLeft, INPUT_PULLUP);
  if (kButtonRight >= 0) pinMode(kButtonRight, INPUT_PULLUP);
  disableUnusedSpiDevices();

  tft.init();
  tft.setRotation(TFT_ROTATION);
#if defined(CYD_TOUCH_ENABLED)
  configureTouch();
#endif
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  loadConfig();
  const bool forcePortal = !readButton(kButtonLeft);
  if (forcePortal) {
    drawCentered(bottomY(24), "Resetting setup", TFT_YELLOW, 2);
    delay(1200);
  }
  connectWifi(forcePortal);
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0/2", "pool.ntp.org", "time.nist.gov");
  drawCentered(88, "Syncing time", TFT_CYAN, 4);
  waitForClock();

  drawCentered(88, "Fetching bins", TFT_CYAN, 4);
  fetchBins();
  drawScreen();
}

void loop() {
  handleButtons();

  const uint32_t nowMs = millis();
  if (nowMs - lastDrawMs > 30000UL) {
    lastDrawMs = nowMs;
    drawScreen();
  }

  if (nowMs - lastFetchAttemptMs > kFetchIntervalMs) {
    lastFetchAttemptMs = nowMs;
    fetchBins();
    drawScreen();
  }

  delay(50);
}
