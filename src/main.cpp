#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <time.h>

constexpr const char *kPortalSsid = "Bins Display Setup";
constexpr const char *kDefaultPostcode = "AL15SR";
constexpr const char *kDefaultUprn = "10001062494";
constexpr const char *kEndpoint =
    "https://gis.stalbans.gov.uk/NoticeBoard9/VeoliaProxy.NoticeBoard.asmx/"
    "GetServicesByUprnAndNoticeBoard";
constexpr const char *kQuickSearchEndpoint = "https://gis.stalbans.gov.uk/NoticeBoard9/quicksearch.asmx";
constexpr uint32_t kFetchIntervalMs = 6UL * 60UL * 60UL * 1000UL;
constexpr int kButtonLeft = 0;
constexpr int kButtonRight = 35;

TFT_eSPI tft;
Preferences prefs;

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
  config.displayMode = prefs.getUChar("displayMode", 0) % 4;

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
  prefs.putUChar("displayMode", config.displayMode % 4);
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
  drawCentered(18, "Bins Display", TFT_CYAN, 4);
  drawCentered(64, "Setup Wi-Fi", TFT_WHITE, 4);
  drawCentered(106, kPortalSsid, TFT_YELLOW, 2);
  drawCentered(136, "Open 192.168.4.1", TFT_LIGHTGREY, 2);
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
  drawText(6, 119, footerText(detailed).substring(0, 34), TFT_LIGHTGREY, 2);
}

void drawModeDot() {
  for (int i = 0; i < 4; i++) {
    tft.fillCircle(206 + (i * 9), 124, 2, i == config.displayMode ? TFT_CYAN : TFT_DARKGREY);
  }
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
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(localTimeText("%H:%M"), 6, 0, 6);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%a %d %b"), tft.width() - 6, 8, 2);

  tft.drawFastHLine(6, 55, tft.width() - 12, accentFor(first));
  drawText(8, 62, first.label, accentFor(first), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(friendlyDay(first), tft.width() - 8, 62, 4);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawRightString(conciseDate(first), tft.width() - 8, 95, 2);
  drawText(8, 95, "Next collection", TFT_LIGHTGREY, 2);

  if (second.valid) {
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(second.label + " " + conciseDate(second), 8, 116, 2);
  }
  if (detailed) drawModeDot();
}

void drawStackAlert(const ServiceDate &svc) {
  tft.fillScreen(TFT_BLACK);
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
  drawText(6, 2, localTimeText("%a %d"), TFT_CYAN, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 8, 7, 4);

  drawText(8, 43, "1", TFT_LIGHTGREY, 2);
  drawText(28, 37, first.label, accentFor(first), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(shortDate(first), tft.width() - 8, 39, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawRightString(countdownText(first), tft.width() - 8, 56, 2);

  tft.drawFastHLine(8, 75, tft.width() - 16, TFT_DARKGREY);
  drawText(8, 88, "2", TFT_LIGHTGREY, 2);
  drawText(28, 82, second.label, accentFor(second), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(shortDate(second), tft.width() - 8, 84, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawRightString(countdownText(second), tft.width() - 8, 101, 2);
  if (detailed) drawModeDot();
}

void drawTimelineAlert(const ServiceDate &svc) {
  tft.fillScreen(TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.drawCentreString("TONIGHT", tft.width() / 2, 6, 4);
  tft.drawCentreString(putOutLabel(svc), tft.width() / 2, 44, 4);
  tft.setTextColor(TFT_YELLOW, TFT_RED);
  tft.drawCentreString("Before 6am", tft.width() / 2, 91, 4);
}

void drawClockLayout(bool detailed) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  const String timeText = localTimeText("%H:%M");
  tft.drawCentreString(timeText == "--" ? "Syncing" : timeText, tft.width() / 2, 6, timeText == "--" ? 4 : 6);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString(localTimeText("%A"), tft.width() / 2, 70, 4);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawCentreString(localTimeText("%d %B"), tft.width() / 2, 101, 4);
  if (detailed) drawModeDot();
}

void drawBoldAlert(const ServiceDate &svc) {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 28, TFT_RED);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.drawCentreString("BINS TONIGHT", tft.width() / 2, 4, 4);
  drawCentered(42, putOutLabel(svc), accentFor(svc), 4);
  drawCentered(76, "Put out", TFT_WHITE, 4);
  drawCentered(105, "before 6am", TFT_YELLOW, 2);
}

void drawBoldLayout(bool detailed) {
  ServiceDate first = nextMainCollection();
  ServiceDate second = first.label == "Refuse" ? bins.recycling : bins.refuse;
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 8, tft.height(), accentFor(first));
  drawText(16, 4, first.label, accentFor(first), 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawRightString(localTimeText("%H:%M"), tft.width() - 8, 6, 4);
  drawCentered(43, shortDate(first), TFT_WHITE, 4);
  drawCentered(78, countdownText(first), TFT_YELLOW, 4);
  if (second.valid) {
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawCentreString("Then " + second.label + " " + conciseDate(second), tft.width() / 2, 112, 2);
  }
  if (detailed) drawModeDot();
}

void drawNormalLayout(bool detailed) {
  if (config.displayMode == 0) drawFocusLayout(detailed);
  if (config.displayMode == 1) drawStackLayout(detailed);
  if (config.displayMode == 2) drawClockLayout(detailed);
  if (config.displayMode == 3) drawBoldLayout(detailed);
}

void drawAlertLayout(const ServiceDate &svc) {
  if (config.displayMode == 0) drawFocusAlert(svc);
  if (config.displayMode == 1) drawStackAlert(svc);
  if (config.displayMode == 2) drawTimelineAlert(svc);
  if (config.displayMode == 3) drawBoldAlert(svc);
}

void drawScreen() {
  ServiceDate alert = previewAlert ? nextMainCollection() : alertCollection();
  if (alert.valid) {
    drawAlertLayout(alert);
    return;
  }
  drawNormalLayout(config.showStatusWhenIdle);
}

void handleButtons() {
  bool left = digitalRead(kButtonLeft);
  bool right = digitalRead(kButtonRight);
  if (!left && lastLeft) {
    config.displayMode = (config.displayMode + 1) % 4;
    saveConfig();
    drawScreen();
  }

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
    lastFetchAttemptMs = 0;
    drawCentered(116, "Refreshing...", TFT_YELLOW, 2);
    fetchBins();
    drawScreen();
  }
  lastLeft = left;
  lastRight = right;
}

void setup() {
  Serial.begin(115200);
  pinMode(kButtonLeft, INPUT_PULLUP);
  pinMode(kButtonRight, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  loadConfig();
  const bool forcePortal = digitalRead(kButtonLeft) == LOW;
  if (forcePortal) {
    drawCentered(190, "Resetting setup", TFT_YELLOW, 2);
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
