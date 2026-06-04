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
constexpr uint8_t kBaseDisplayModeCount = 5;
constexpr uint8_t kLargeDisplayModeCount = 8;
constexpr uint8_t kPortraitDisplayModeCount = 5;
constexpr uint16_t kDefaultColorRed = 0x07FF;
constexpr uint16_t kDefaultColorGreen = 0xE0FF;
constexpr uint16_t kDefaultColorBlue = 0xFFE0;
constexpr uint16_t kDefaultColorYellow = 0x001F;
#ifndef TFT_ROTATION
#define TFT_ROTATION 1
#endif
#ifndef CYD_PORTRAIT_ROTATION
#define CYD_PORTRAIT_ROTATION 0
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
  bool portraitLayout = false;
  uint8_t displayMode = 0;
  uint16_t colorRed = kDefaultColorRed;
  uint16_t colorGreen = kDefaultColorGreen;
  uint16_t colorBlue = kDefaultColorBlue;
  uint16_t colorYellow = kDefaultColorYellow;
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

uint16_t uiRed();
uint16_t uiGreen();
uint16_t uiBlue();
uint16_t uiYellow();

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
uint8_t colorCalStep = 0;
uint16_t touchLastX = 0;
uint16_t touchLastY = 0;
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

bool portraitLayoutActive() {
#if defined(CHEAP_YELLOW_DISPLAY)
  return config.portraitLayout;
#else
  return false;
#endif
}

uint8_t displayModeCount() {
#if defined(CHEAP_YELLOW_DISPLAY)
  if (config.portraitLayout) return kPortraitDisplayModeCount;
#endif
  return largeScreen() ? kLargeDisplayModeCount : kBaseDisplayModeCount;
}

void applyDisplayRotation() {
#if defined(CHEAP_YELLOW_DISPLAY)
  const uint8_t rotation = config.portraitLayout ? CYD_PORTRAIT_ROTATION : TFT_ROTATION;
  tft.setRotation(rotation);
#if defined(CYD_TOUCH_ENABLED)
  touch.setRotation(rotation);
#endif
#else
  tft.setRotation(TFT_ROTATION);
#endif
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
  config.portraitLayout = prefs.getBool("portrait", false);
  config.displayMode = prefs.getUChar("displayMode", 0) % displayModeCount();
  config.colorRed = prefs.getUShort("colorRed", kDefaultColorRed);
  config.colorGreen = prefs.getUShort("colorGreen", kDefaultColorGreen);
  config.colorBlue = prefs.getUShort("colorBlue", kDefaultColorBlue);
  config.colorYellow = prefs.getUShort("colorYellow", kDefaultColorYellow);

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
  hydrate(bins.refuse, "Domestic Refuse Collection", "General Waste");
  hydrate(bins.recycling, "Domestic Recycling Collection", "Recycling");
  hydrate(bins.food, "Domestic Food Collection", "Food");
  hydrate(bins.garden, "Domestic Garden Waste Collection", "Garden");
  bins.loaded = bins.refuse.valid || bins.recycling.valid || bins.food.valid || bins.garden.valid;
}

void saveConfig() {
  prefs.putString("postcode", compactPostcode(config.postcode));
  prefs.putString("uprn", config.uprn);
  prefs.putBool("idleStatus", config.showStatusWhenIdle);
  prefs.putBool("portrait", config.portraitLayout);
  prefs.putUChar("displayMode", config.displayMode % displayModeCount());
  prefs.putUShort("colorRed", config.colorRed);
  prefs.putUShort("colorGreen", config.colorGreen);
  prefs.putUShort("colorBlue", config.colorBlue);
  prefs.putUShort("colorYellow", config.colorYellow);
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
  drawCentered(largeScreen() ? 36 : 18, "Bins Display", config.colorBlue, 4);
  drawCentered(largeScreen() ? 92 : 64, "Setup Wi-Fi", TFT_WHITE, 4);
  drawCentered(largeScreen() ? 146 : 106, kPortalSsid, config.colorYellow, 2);
  drawCentered(largeScreen() ? 178 : 136, "Open 192.168.4.1", TFT_LIGHTGREY, 2);
}

void resetStoredSetup(WiFiManager &wm) {
  wm.resetSettings();
  prefs.clear();
  config.postcode = kDefaultPostcode;
  config.uprn = kDefaultUprn;
  config.showStatusWhenIdle = true;
  config.portraitLayout = false;
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
    drawCentered(42, "Finding UPRN", config.colorBlue, 4);
    drawCentered(82, config.postcode, TFT_WHITE, 4);
    String foundUprn;
    if (lookupUprnForPostcode(config.postcode, foundUprn)) {
      config.uprn = foundUprn;
      drawCentered(116, "Found", config.colorGreen, 2);
      delay(900);
    } else {
      drawCentered(116, "UPRN unchanged", config.colorYellow, 2);
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
    if (name == "Domestic Refuse Collection") parseService(svc, bins.refuse, "General Waste");
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

bool isGeneralWaste(const ServiceDate &svc) {
  return svc.name == "Domestic Refuse Collection" || svc.label == "General Waste";
}

uint16_t accentFor(const ServiceDate &svc) {
  return isGeneralWaste(svc) ? config.colorYellow : config.colorGreen;
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
  if (isGeneralWaste(svc)) return "General Waste";
  return svc.label;
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
  const uint8_t count = displayModeCount();
  const int firstX = tft.width() - ((count * 9) - 1);
  const int y = bottomY(11);
  for (int i = 0; i < count; i++) {
    tft.fillCircle(firstX + (i * 9), y, 2, i == config.displayMode ? config.colorBlue : TFT_DARKGREY);
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

constexpr uint16_t swapRgb565Bytes(uint16_t color) {
  return (color >> 8) | (color << 8);
}

uint16_t analogAlertBackgroundColor() {
  return config.colorRed;
}

uint16_t uiRed() { return config.colorRed; }
uint16_t uiGreen() { return config.colorGreen; }
uint16_t uiBlue() { return config.colorBlue; }
uint16_t uiYellow() { return config.colorYellow; }

#if defined(CYD_TOUCH_ENABLED)
constexpr uint16_t kColorCandidates[] = {
    TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW,
    TFT_CYAN, TFT_MAGENTA, TFT_ORANGE, TFT_WHITE,
    0x0000, 0xF800, 0x07E0, 0x001F,
    swapRgb565Bytes(0xF800), swapRgb565Bytes(0x07E0),
    swapRgb565Bytes(0x001F), swapRgb565Bytes(0xFFE0)
};

String hexColor(uint16_t color) {
  char buf[7];
  snprintf(buf, sizeof(buf), "%04X", color);
  return String(buf);
}

String colorStepName() {
  if (colorCalStep == 0) return "RED";
  if (colorCalStep == 1) return "GREEN";
  if (colorCalStep == 2) return "BLUE";
  if (colorCalStep == 3) return "YELLOW";
  return "DONE";
}

void storeSelectedColor(uint16_t color) {
  if (colorCalStep == 0) config.colorRed = color;
  if (colorCalStep == 1) config.colorGreen = color;
  if (colorCalStep == 2) config.colorBlue = color;
  if (colorCalStep == 3) config.colorYellow = color;
  Serial.printf("Colour %s = 0x%04X\n", colorStepName().c_str(), color);
  if (colorCalStep < 4) colorCalStep++;
  saveConfig();
}
#endif

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
    drawCentered(24, "TONIGHT", uiRed(), 4);
    drawCentered(72, putOutLabel(svc), TFT_WHITE, 4);
    drawCentered(128, "Put out by 6am", uiYellow(), 4);
    drawCentered(184, localTimeText("%H:%M"), uiBlue(), 6);
    return;
  }
  drawCentered(2, "TONIGHT", uiRed(), 4);
  drawCentered(32, putOutLabel(svc), TFT_WHITE, 4);
  drawCentered(74, "Put out by 6am", uiYellow(), 4);
  drawCentered(108, localTimeText("%H:%M"), uiBlue(), 2);
}

void drawFocusLayout(bool detailed) {
  ServiceDate first;
  ServiceDate second;
  orderedMainCollections(first, second);
  tft.fillScreen(TFT_BLACK);
  const int lineY = largeScreen() ? 82 : 55;
  const int rowY = largeScreen() ? 96 : 62;
  const int dateY = largeScreen() ? 136 : 95;
  tft.setTextColor(uiBlue(), TFT_BLACK);
  tft.drawString(localTimeText("%H:%M"), 6, largeScreen() ? 8 : 0, 6);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%a %d %b"), tft.width() - 8, largeScreen() ? 18 : 8, 2);

  tft.drawFastHLine(6, lineY, tft.width() - 12, accentFor(first));
  drawText(8, rowY, first.label, accentFor(first), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(friendlyDay(first), tft.width() - 8, rowY, 4);
  tft.setTextColor(uiYellow(), TFT_BLACK);
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
    drawText(10, 12, "Put out", uiRed(), 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawRightString(localTimeText("%H:%M"), tft.width() - 10, 17, 4);
    tft.drawFastHLine(12, 60, tft.width() - 24, uiRed());
    drawCentered(88, putOutLabel(svc), TFT_WHITE, 4);
    drawCentered(142, "Tonight", uiYellow(), 4);
    drawModeDot();
    return;
  }
  drawText(6, 2, "Put out", uiRed(), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 8, 7, 4);
  tft.drawFastHLine(8, 41, tft.width() - 16, uiRed());
  drawCentered(51, putOutLabel(svc), TFT_WHITE, 4);
  drawCentered(91, "Tonight", uiYellow(), 4);
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
  drawText(6, largeScreen() ? 12 : 2, localTimeText("%a %d"), uiBlue(), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 8, largeScreen() ? 17 : 7, 4);

  drawText(8, firstY + 6, "1", TFT_LIGHTGREY, 2);
  drawText(28, firstY, first.label, accentFor(first), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(shortDate(first), tft.width() - 8, firstY + 2, 2);
  tft.setTextColor(uiYellow(), TFT_BLACK);
  tft.drawRightString(countdownText(first), tft.width() - 8, firstY + 19, 2);

  tft.drawFastHLine(8, dividerY, tft.width() - 16, TFT_DARKGREY);
  drawText(8, secondY + 6, "2", TFT_LIGHTGREY, 2);
  drawText(28, secondY, second.label, accentFor(second), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(shortDate(second), tft.width() - 8, secondY + 2, 2);
  tft.setTextColor(uiYellow(), TFT_BLACK);
  tft.drawRightString(countdownText(second), tft.width() - 8, secondY + 19, 2);
  if (detailed) drawModeDot();
}

void drawTimelineAlert(const ServiceDate &svc) {
  tft.fillScreen(uiRed());
  tft.setTextColor(TFT_WHITE, uiRed());
  tft.drawCentreString("TONIGHT", tft.width() / 2, largeScreen() ? 32 : 6, 4);
  tft.drawCentreString(putOutLabel(svc), tft.width() / 2, largeScreen() ? 88 : 44, 4);
  tft.setTextColor(uiYellow(), uiRed());
  tft.drawCentreString("Before 6am", tft.width() / 2, largeScreen() ? 150 : 91, 4);
}

void drawClockLayout(bool detailed) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(uiBlue(), TFT_BLACK);
  const String timeText = localTimeText("%H:%M");
  tft.drawCentreString(timeText == "--" ? "Syncing" : timeText, tft.width() / 2, largeScreen() ? 28 : 6, timeText == "--" ? 4 : 6);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString(localTimeText("%A"), tft.width() / 2, largeScreen() ? 110 : 70, 4);
  tft.setTextColor(uiYellow(), TFT_BLACK);
  tft.drawCentreString(localTimeText("%d %B"), tft.width() / 2, largeScreen() ? 152 : 101, 4);
  if (detailed) drawModeDot();
}

void drawBoldAlert(const ServiceDate &svc) {
  tft.fillScreen(TFT_BLACK);
  const int headerHeight = largeScreen() ? 42 : 28;
  tft.fillRect(0, 0, tft.width(), headerHeight, uiRed());
  tft.setTextColor(TFT_WHITE, uiRed());
  tft.drawCentreString("BINS TONIGHT", tft.width() / 2, largeScreen() ? 10 : 4, 4);
  drawCentered(largeScreen() ? 66 : 42, putOutLabel(svc), accentFor(svc), 4);
  drawCentered(largeScreen() ? 118 : 76, "Put out", TFT_WHITE, 4);
  drawCentered(largeScreen() ? 162 : 105, "before 6am", uiYellow(), largeScreen() ? 4 : 2);
}

void drawBoldLayout(bool detailed) {
  ServiceDate first = nextMainCollection();
  ServiceDate second = isGeneralWaste(first) ? bins.recycling : bins.refuse;
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, largeScreen() ? 12 : 8, tft.height(), accentFor(first));
  drawText(largeScreen() ? 22 : 16, largeScreen() ? 14 : 4, first.label, accentFor(first), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 8, largeScreen() ? 16 : 6, 4);
  drawCentered(largeScreen() ? 78 : 43, shortDate(first), TFT_WHITE, 4);
  drawCentered(largeScreen() ? 130 : 78, countdownText(first), uiYellow(), 4);
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
  drawText(6, largeScreen() ? 12 : 0, "Bins v" + String(FIRMWARE_VERSION), uiBlue(), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 8, largeScreen() ? 18 : 6, 4);

  drawText(8, startY, "Wi-Fi", TFT_LIGHTGREY, 2);
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? uiGreen() : uiRed(), TFT_BLACK);
  tft.drawString(wifiSummary().substring(0, largeScreen() ? 34 : 24), 82, startY, 2);

  drawText(8, startY + rowGap, "IP", TFT_LIGHTGREY, 2);
  drawText(82, startY + rowGap, WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "--", TFT_WHITE, 2);

  drawText(8, startY + (rowGap * 2), "Postcode", TFT_LIGHTGREY, 2);
  drawText(82, startY + (rowGap * 2), compactPostcode(config.postcode), TFT_WHITE, 2);

  drawText(8, startY + (rowGap * 3), "UPRN", TFT_LIGHTGREY, 2);
  drawText(82, startY + (rowGap * 3), config.uprn.substring(0, largeScreen() ? 24 : 18), TFT_WHITE, 2);

  drawText(8, startY + (rowGap * 4), "Fetch", TFT_LIGHTGREY, 2);
  drawText(82, startY + (rowGap * 4), compactTimeText(bins.lastFetch), bins.lastError.length() ? uiYellow() : TFT_WHITE, 2);

  const String health = bins.lastError.length() ? bins.lastError : "OK";
  tft.setTextColor(bins.lastError.length() ? uiYellow() : uiGreen(), TFT_BLACK);
  tft.drawString(("Up " + uptimeText() + " " + health).substring(0, largeScreen() ? 36 : 24), 8, bottomY(18), 2);
  drawModeDot();
}

void drawWideAlert(const ServiceDate &svc) {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 54, uiRed());
  tft.setTextColor(TFT_WHITE, uiRed());
  tft.drawString("TONIGHT", 12, 12, 4);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 12, 12, 4);
  drawText(14, 76, putOutLabel(svc), TFT_WHITE, 6);
  drawText(16, 150, "Put out before 6am", uiYellow(), 4);
  drawText(18, 206, shortDate(svc), accentFor(svc), 2);
  drawModeDot();
}

void drawAnalogClockFace(int cx, int cy, int radius, uint16_t faceColor, uint16_t handColor) {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    drawCentered(cy - 12, "Syncing", handColor, 4);
    return;
  }

  tm info = *localtime(&now);
  tft.drawCircle(cx, cy, radius, faceColor);
  tft.drawCircle(cx, cy, radius - 1, faceColor);

  for (int tick = 0; tick < 60; tick++) {
    const float angle = (tick * 6 - 90) * DEG_TO_RAD;
    const int outerX = cx + cos(angle) * radius;
    const int outerY = cy + sin(angle) * radius;
    const int inner = tick % 5 == 0 ? radius - 10 : radius - 5;
    const int innerX = cx + cos(angle) * inner;
    const int innerY = cy + sin(angle) * inner;
    tft.drawLine(innerX, innerY, outerX, outerY, tick % 5 == 0 ? handColor : faceColor);
  }

  const float minuteAngle = (info.tm_min * 6 - 90) * DEG_TO_RAD;
  const float hourAngle = (((info.tm_hour % 12) * 30) + (info.tm_min * 0.5f) - 90) * DEG_TO_RAD;
  auto drawThickHand = [](float angle, float length, int halfWidth, uint16_t color, int cx, int cy) {
    const int tipX = cx + cos(angle) * length;
    const int tipY = cy + sin(angle) * length;
    const float sideAngle = angle + HALF_PI;
    for (int offset = -halfWidth; offset <= halfWidth; offset++) {
      const int ox = cos(sideAngle) * offset;
      const int oy = sin(sideAngle) * offset;
      tft.drawLine(cx + ox, cy + oy, tipX + ox, tipY + oy, color);
    }
  };
  drawThickHand(hourAngle, radius * 0.48f, 3, handColor, cx, cy);
  drawThickHand(minuteAngle, radius * 0.72f, 2, uiBlue(), cx, cy);
  tft.fillCircle(cx, cy, 4, uiYellow());
}

void drawAnalogClockAlert(const ServiceDate &svc) {
  const uint16_t bg = analogAlertBackgroundColor();
  tft.fillScreen(bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawCentreString("BINS TONIGHT", tft.width() / 2, 12, 4);
  tft.drawCentreString(putOutLabel(svc), tft.width() / 2, 70, 4);
  tft.setTextColor(uiYellow(), bg);
  tft.drawCentreString("Put out before 6am", tft.width() / 2, 126, 4);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawCentreString(localTimeText("%H:%M") + "  " + shortDate(svc), tft.width() / 2, 190, 4);
}

void drawLargeDashboardLayout(bool detailed) {
  ServiceDate first;
  ServiceDate second;
  orderedMainCollections(first, second);
  tft.fillScreen(TFT_BLACK);

  drawText(10, 8, localTimeText("%H:%M"), uiBlue(), 6);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%A"), tft.width() - 10, 14, 4);
  tft.drawRightString(localTimeText("%d %B"), tft.width() - 10, 48, 2);

  tft.fillRect(0, 92, 10, 92, accentFor(first));
  drawText(20, 92, "Next", TFT_LIGHTGREY, 2);
  drawText(20, 112, first.label, accentFor(first), 4);
  drawText(20, 150, friendlyDay(first), TFT_WHITE, 4);
  tft.setTextColor(uiYellow(), TFT_BLACK);
  tft.drawRightString(countdownText(first), tft.width() - 12, 130, 4);

  if (second.valid) {
    tft.drawFastHLine(20, 194, tft.width() - 40, TFT_DARKGREY);
    drawText(20, 204, "Then " + second.label, TFT_LIGHTGREY, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawRightString(shortDate(second), tft.width() - 12, 204, 2);
  }
  if (detailed) drawModeDot();
}

void drawLargeAnalogClockLayout(bool detailed) {
  ServiceDate first = nextMainCollection();
  tft.fillScreen(TFT_BLACK);
  drawAnalogClockFace(98, 130, 82, TFT_LIGHTGREY, TFT_WHITE);

  tft.setTextColor(uiBlue(), TFT_BLACK);
  tft.drawString(localTimeText("%H:%M"), 204, 30, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(localTimeText("%A"), 204, 94, 4);
  tft.setTextColor(uiYellow(), TFT_BLACK);
  tft.drawString(localTimeText("%d %B"), 204, 128, 4);
  tft.drawFastHLine(204, 168, 104, TFT_DARKGREY);
  drawText(204, 182, "Next", TFT_LIGHTGREY, 2);
  drawText(204, 202, first.label + " " + friendlyDay(first), accentFor(first), 2);
  drawModeDot();
}

#if defined(CYD_TOUCH_ENABLED)
void drawColorCalibrationLayout() {
  tft.fillScreen(TFT_BLACK);
  drawText(8, 4, "Colour test", TFT_WHITE, 4);
  drawText(8, 36, "Tap the swatch that looks " + colorStepName(), config.colorYellow, 2);

  for (int i = 0; i < 16; i++) {
    const int col = i % 4;
    const int row = i / 4;
    const int x = 8 + (col * 78);
    const int y = 62 + (row * 34);
    tft.fillRect(x, y, 68, 24, kColorCandidates[i]);
    tft.drawRect(x, y, 68, 24, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String(i + 1), x + 3, y + 4, 2);
  }

  drawText(8, 206, "R " + hexColor(config.colorRed) + " G " + hexColor(config.colorGreen), TFT_WHITE, 2);
  drawText(160, 206, "B " + hexColor(config.colorBlue) + " Y " + hexColor(config.colorYellow), TFT_WHITE, 2);
  drawModeDot();
}

bool handleColorCalibrationTouch(uint16_t x, uint16_t y) {
  if (config.displayMode != 7 || !largeScreen()) return false;
  for (int i = 0; i < 16; i++) {
    const int col = i % 4;
    const int row = i / 4;
    const int sx = 8 + (col * 78);
    const int sy = 62 + (row * 34);
    if (x >= sx && x <= sx + 68 && y >= sy && y <= sy + 24) {
      storeSelectedColor(kColorCandidates[i]);
      drawColorCalibrationLayout();
      return true;
    }
  }
  return false;
}
#endif

void drawPortraitAlert(const ServiceDate &svc) {
  const uint16_t bg = analogAlertBackgroundColor();
  tft.fillScreen(bg);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawCentreString("PUT BINS OUT", tft.width() / 2, 24, 4);
  tft.drawCentreString(putOutLabel(svc), tft.width() / 2, 86, 4);
  tft.setTextColor(uiYellow(), bg);
  tft.drawCentreString("Tonight", tft.width() / 2, 142, 4);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawCentreString("Before 6am", tft.width() / 2, 190, 4);
  tft.drawCentreString(localTimeText("%H:%M"), tft.width() / 2, 252, 6);
}

void drawPortraitCleanLayout(bool detailed) {
  ServiceDate first = nextMainCollection();
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(uiBlue(), TFT_BLACK);
  tft.drawCentreString(localTimeText("%H:%M"), tft.width() / 2, 24, 6);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString(localTimeText("%A"), tft.width() / 2, 102, 4);
  tft.setTextColor(uiYellow(), TFT_BLACK);
  tft.drawCentreString(localTimeText("%d %B"), tft.width() / 2, 136, 4);
  tft.drawFastHLine(36, 206, tft.width() - 72, TFT_DARKGREY);
  drawText(34, 226, "Next bin", TFT_LIGHTGREY, 2);
  tft.fillRoundRect(tft.width() - 126, 222, 92, 24, 4, accentFor(first));
  tft.setTextColor(TFT_BLACK, accentFor(first));
  tft.drawCentreString(first.label, tft.width() - 80, 226, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(friendlyDay(first), tft.width() - 34, 256, 4);
  if (detailed) drawModeDot();
}

void drawPortraitFocusLayout(bool detailed) {
  ServiceDate first = nextMainCollection();
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 46, accentFor(first));
  tft.setTextColor(TFT_BLACK, accentFor(first));
  tft.drawCentreString("NEXT BIN DAY", tft.width() / 2, 12, 4);
  drawText(18, 66, "Put out", TFT_LIGHTGREY, 2);
  tft.setTextColor(accentFor(first), TFT_BLACK);
  tft.drawCentreString(first.label, tft.width() / 2, 92, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString(friendlyDay(first), tft.width() / 2, 140, 6);
  tft.setTextColor(uiYellow(), TFT_BLACK);
  tft.drawCentreString(conciseDate(first), tft.width() / 2, 216, 4);
  drawText(14, 270, countdownText(first), uiYellow(), 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 14, 264, 4);
  if (detailed) drawModeDot();
}

void drawPortraitCardsLayout(bool detailed) {
  tft.fillScreen(TFT_BLACK);
  drawText(12, 10, localTimeText("%H:%M"), uiBlue(), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%a %d"), tft.width() - 12, 16, 2);

  auto drawPortraitCard = [](int y, const ServiceDate &svc) {
    tft.drawRoundRect(12, y, 216, 82, 5, TFT_DARKGREY);
    tft.fillRect(12, y, 8, 82, accentFor(svc));
    drawText(30, y + 12, svc.label, accentFor(svc), 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(friendlyDay(svc), 30, y + 38, 4);
    tft.setTextColor(uiYellow(), TFT_BLACK);
    tft.drawRightString(conciseDate(svc), 216, y + 58, 2);
  };

  drawPortraitCard(58, bins.refuse);
  drawPortraitCard(158, bins.recycling);
  if (detailed) drawModeDot();
}

void drawPortraitAnalogLayout(bool detailed) {
  ServiceDate first = nextMainCollection();
  tft.fillScreen(TFT_BLACK);
  drawAnalogClockFace(tft.width() / 2, 104, 82, TFT_LIGHTGREY, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString(localTimeText("%A"), tft.width() / 2, 202, 4);
  tft.setTextColor(uiYellow(), TFT_BLACK);
  tft.drawCentreString(localTimeText("%d %B"), tft.width() / 2, 236, 4);
  tft.setTextColor(accentFor(first), TFT_BLACK);
  tft.drawCentreString(first.label + " " + friendlyDay(first), tft.width() / 2, 280, 2);
  if (detailed) drawModeDot();
}

void drawPortraitAgendaLayout(bool detailed) {
  ServiceDate first;
  ServiceDate second;
  orderedMainCollections(first, second);
  tft.fillScreen(TFT_BLACK);
  drawText(12, 10, "Next two", uiBlue(), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 12, 18, 2);

  tft.fillCircle(26, 82, 11, accentFor(first));
  tft.setTextColor(TFT_BLACK, accentFor(first));
  tft.drawCentreString("1", 26, 75, 2);
  drawText(48, 64, first.label, accentFor(first), 4);
  drawText(48, 104, friendlyDay(first), TFT_WHITE, 4);
  tft.setTextColor(uiYellow(), TFT_BLACK);
  tft.drawRightString(conciseDate(first), tft.width() - 14, 112, 2);

  tft.drawFastHLine(18, 166, tft.width() - 36, TFT_DARKGREY);
  tft.fillCircle(26, 206, 11, accentFor(second));
  tft.setTextColor(TFT_BLACK, accentFor(second));
  tft.drawCentreString("2", 26, 199, 2);
  drawText(48, 188, second.label, accentFor(second), 4);
  drawText(48, 228, friendlyDay(second), TFT_WHITE, 4);
  tft.setTextColor(uiYellow(), TFT_BLACK);
  tft.drawRightString(conciseDate(second), tft.width() - 14, 236, 2);
  drawText(18, 282, WiFi.status() == WL_CONNECTED ? "Wi-Fi OK" : "Wi-Fi offline", TFT_LIGHTGREY, 2);
  if (detailed) drawModeDot();
}

void drawNormalLayout(bool detailed) {
  if (portraitLayoutActive()) {
    if (config.displayMode == 0) drawPortraitCleanLayout(detailed);
    if (config.displayMode == 1) drawPortraitFocusLayout(detailed);
    if (config.displayMode == 2) drawPortraitCardsLayout(detailed);
    if (config.displayMode == 3) drawPortraitAnalogLayout(detailed);
    if (config.displayMode == 4) drawPortraitAgendaLayout(detailed);
    return;
  }
  if (config.displayMode == 0) drawFocusLayout(detailed);
  if (config.displayMode == 1) drawStackLayout(detailed);
  if (config.displayMode == 2) drawClockLayout(detailed);
  if (config.displayMode == 3) drawBoldLayout(detailed);
  if (config.displayMode == 4) drawStatusLayout();
  if (largeScreen() && config.displayMode == 5) drawLargeDashboardLayout(detailed);
  if (largeScreen() && config.displayMode == 6) drawLargeAnalogClockLayout(detailed);
#if defined(CYD_TOUCH_ENABLED)
  if (largeScreen() && config.displayMode == 7) drawColorCalibrationLayout();
#endif
}

void drawAlertLayout(const ServiceDate &svc) {
  if (portraitLayoutActive()) {
    drawPortraitAlert(svc);
    return;
  }
  if (config.displayMode == 0) drawFocusAlert(svc);
  if (config.displayMode == 1) drawStackAlert(svc);
  if (config.displayMode == 2) drawTimelineAlert(svc);
  if (config.displayMode == 3) drawBoldAlert(svc);
  if (config.displayMode == 4) drawFocusAlert(svc);
  if (largeScreen() && config.displayMode == 5) drawWideAlert(svc);
  if (largeScreen() && config.displayMode == 6) drawAnalogClockAlert(svc);
  if (largeScreen() && config.displayMode == 7) drawWideAlert(svc);
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
  config.displayMode = (config.displayMode + 1) % displayModeCount();
  saveConfig();
  drawScreen();
}

void toggleLayoutOrientation() {
#if defined(CHEAP_YELLOW_DISPLAY)
  config.portraitLayout = !config.portraitLayout;
  config.displayMode = 0;
  applyDisplayRotation();
  tft.fillScreen(TFT_BLACK);
  saveConfig();
  drawScreen();
#endif
}

void refreshCollections() {
  lastFetchAttemptMs = 0;
  drawCentered(bottomY(19), "Refreshing...", uiYellow(), 2);
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
  if (touched) {
    touchLastX = x;
    touchLastY = y;
  }

  if (touched && !touchLongHandled && millis() - touchPressedAtMs > 900UL) {
    if (touchRightSide) {
      previewAlert = !previewAlert;
      drawScreen();
    } else {
      toggleLayoutOrientation();
    }
    touchLongHandled = true;
  }

  if (!touched && lastTouch && !touchLongHandled) {
    if (handleColorCalibrationTouch(touchLastX, touchLastY)) {
      // Selection handled by the colour calibration page.
    } else if (touchRightSide) {
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
  tft.setTextDatum(TL_DATUM);

  loadConfig();
  applyDisplayRotation();
  tft.fillScreen(TFT_BLACK);
  const bool forcePortal = !readButton(kButtonLeft);
  if (forcePortal) {
    drawCentered(bottomY(24), "Resetting setup", uiYellow(), 2);
    delay(1200);
  }
  connectWifi(forcePortal);
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0/2", "pool.ntp.org", "time.nist.gov");
  drawCentered(88, "Syncing time", uiBlue(), 4);
  waitForClock();

  drawCentered(88, "Fetching bins", uiBlue(), 4);
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
