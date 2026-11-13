#include <Arduino.h>
#include <memory>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <EEPROM.h>
#include <time.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <ElegantOTA.h>

// ============================================================================
// 1. НАЛАШТУВАННЯ ЗА ЗАМОВЧУВАННЯМ
// ============================================================================
const char* DEFAULT_WIFI_SSID     = "2,4/5G";
const char* DEFAULT_WIFI_PASS     = "Sasha1997";

const char* DEFAULT_WORKER_URL    = "https://cube.alexanderbogatov97.workers.dev/";
const char* DEFAULT_ALERTS_TOKEN  = "";

// Координати м. Кам'янець-Подільський (вул. Розвадовського, 5)
const float KAMIANETS_LAT         = 48.6845f;
const float KAMIANETS_LON         = 26.5856f;

// UID локацій у сервісі alerts.in.ua
const int   UID_KAMIANETS_RAION   = 135; // Кам'янець-Подільський район
const int   UID_KHMELNYTSKYI_OBL  = 3;   // Хмельницька область

const char* DEFAULT_QUEUE_TODAY   = "06:00-09:00; 16:00-19:00";
const char* DEFAULT_QUEUE_TOMORROW= "Очікується";

#ifndef TFT_BL_PIN
#define TFT_BL_PIN 5
#endif

// Кольори з урахуванням BGR-порядку матриці ST7789 у GeekMagic SmallTV
#define COL_BG_DARK       0x1861  // Темно-синій фон
#define COL_HEADER_SAFE   0x9302  // Насичений синій (спокійно)
#define COL_HEADER_WARN   0x035A  // Помаранчевий (тривога в області / БПЛА < 35 км)
#define COL_HEADER_ALERT  0x409C  // Яскраво-червоний (ТРИВОГА в районі)
#define COL_CARD_BG       0x28E2  // Фон карток
#define COL_OUTAGE_BG     0x38E3  // Фон панелі світла
#define COL_TEXT_WHITE    0xFFFF  // Білий
#define COL_TEXT_CYAN     0xFFEC  // Блакитний
#define COL_TEXT_YELLOW   0x07FF  // Жовтий
#define COL_TEXT_GREEN    0x47E9  // Зелений
#define COL_TEXT_PINK     0x819F  // Червоно-рожевий (увага / відключення)
#define COL_TEXT_GRAY     0xBDF7  // Світло-сірий

// ============================================================================
// 2. СТРУКТУРИ ЗБЕРЕЖЕННЯ НАЛАШТУВАНЬ (EEPROM) ТА СТАНУ ДАНИХ
// ============================================================================
struct Config {
  uint32_t magic;
  char wifi_ssid[36];
  char wifi_pass[36];
  char worker_url[128];
  char alerts_token[64];
  char schedule_today[64];
  char schedule_tomorrow[64];
  uint8_t brightness;
  uint8_t rotation;
};

const uint32_t CONFIG_MAGIC = 0x534D5457;
Config cfg;

struct WeatherData {
  bool valid = false;
  float tempNow = 0.0f;
  float feelsLike = 0.0f;
  int humidity = 0;
  float windSpeed = 0.0f;
  int codeNow = 0;
  float tempTomorrowMin = 0.0f;
  float tempTomorrowMax = 0.0f;
  int precipProbTomorrow = 0;
  int codeTomorrow = 0;
  unsigned long lastUpdate = 0;
} weather;

struct AlertAndPowerData {
  bool alertRaion = false;
  bool alertOblast = false;
  String alertLevel = "none";
  String threatType = "Немає загроз";
  String threatDirection = "Кам'янець-Под. р-н: спокійно";
  int nearestShahedKm = -1;
  String nearestShahedPlace = "";
  unsigned long alertStartedEpoch = 0;
  String outageToday = DEFAULT_QUEUE_TODAY;
  String outageTomorrow = DEFAULT_QUEUE_TOMORROW;
  unsigned long lastUpdate = 0;
} statusData;

TFT_eSPI tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;
ESP8266WebServer server(80);

int lastDrawnMinute = -1;
unsigned long lastWeatherFetch = 0;
unsigned long lastAlertFetch = 0;
bool forceRedraw = true;

// ============================================================================
// 3. ДОПОМІЖНІ ФУНКЦІЇ (UTF-8 ОБРІЗКА ТА ПЕРЕНЕСЕННЯ, EEPROM, ПІДСВІТКА)
// ============================================================================
String utf8Truncate(const String& str, size_t maxChars) {
  size_t charCount = 0;
  size_t byteIndex = 0;
  size_t len = str.length();
  while (byteIndex < len && charCount < maxChars) {
    uint8_t c = (uint8_t)str[byteIndex];
    if ((c & 0x80) == 0) byteIndex += 1;
    else if ((c & 0xE0) == 0xC0) byteIndex += 2;
    else if ((c & 0xF0) == 0xE0) byteIndex += 3;
    else byteIndex += 4;
    charCount++;
  }
  if (byteIndex > len) byteIndex = len;
  return str.substring(0, byteIndex);
}

int printWrappedUtf8(const String& text, int x, int y, int lineHeight, size_t maxCharsPerLine, int maxLines) {
  size_t startByte = 0;
  size_t len = text.length();
  int line = 0;

  while (startByte < len && line < maxLines) {
    size_t curByte = startByte;
    size_t charCount = 0;
    size_t lastSpaceByte = 0;
    bool hasSpace = false;

    while (curByte < len && charCount < maxCharsPerLine) {
      uint8_t c = (uint8_t)text[curByte];
      if (c == ' ') {
        lastSpaceByte = curByte;
        hasSpace = true;
      }
      size_t step = 1;
      if ((c & 0x80) == 0) step = 1;
      else if ((c & 0xE0) == 0xC0) step = 2;
      else if ((c & 0xF0) == 0xE0) step = 3;
      else step = 4;

      if (curByte + step > len) break;
      curByte += step;
      charCount++;
    }

    size_t endByte = curByte;
    size_t nextStart = curByte;

    if (curByte < len && hasSpace && lastSpaceByte > startByte) {
      endByte = lastSpaceByte;
      nextStart = lastSpaceByte + 1;
    }

    u8f.setCursor(x, y + line * lineHeight);
    u8f.print(text.substring(startByte, endByte));

    while (nextStart < len && text[nextStart] == ' ') {
      nextStart++;
    }
    startByte = nextStart;
    line++;
  }
  return line > 0 ? line : 1;
}

void applyBacklight(uint8_t percent) {
  if (percent > 100) percent = 100;
  int pwmVal = map(100 - percent, 0, 100, 0, 255);
  analogWrite(TFT_BL_PIN, pwmVal);
}

void saveConfig() {
  EEPROM.begin(sizeof(Config));
  EEPROM.put(0, cfg);
  EEPROM.commit();
  EEPROM.end();
}

void loadConfig() {
  EEPROM.begin(sizeof(Config));
  EEPROM.get(0, cfg);
  EEPROM.end();

  if (cfg.magic != CONFIG_MAGIC) {
    memset(&cfg, 0, sizeof(Config));
    cfg.magic = CONFIG_MAGIC;
    strncpy(cfg.wifi_ssid, DEFAULT_WIFI_SSID, sizeof(cfg.wifi_ssid) - 1);
    strncpy(cfg.wifi_pass, DEFAULT_WIFI_PASS, sizeof(cfg.wifi_pass) - 1);
    strncpy(cfg.worker_url, DEFAULT_WORKER_URL, sizeof(cfg.worker_url) - 1);
    strncpy(cfg.alerts_token, DEFAULT_ALERTS_TOKEN, sizeof(cfg.alerts_token) - 1);
    strncpy(cfg.schedule_today, DEFAULT_QUEUE_TODAY, sizeof(cfg.schedule_today) - 1);
    strncpy(cfg.schedule_tomorrow, DEFAULT_QUEUE_TOMORROW, sizeof(cfg.schedule_tomorrow) - 1);
    cfg.brightness = 85;
    cfg.rotation = 0;
    saveConfig();
  }
  statusData.outageToday = String(cfg.schedule_today);
  statusData.outageTomorrow = String(cfg.schedule_tomorrow);
}

// ============================================================================
// 4. ПОГОДА WMO ТА АНАЛІЗ ГРАФІКА СВІТЛА (Черга 5.2)
// ============================================================================
String weatherCodeToText(int code) {
  switch (code) {
    case 0:  return "Ясно";
    case 1:
    case 2:  return "Мінл.хмарно";
    case 3:  return "Хмарно";
    case 45:
    case 48: return "Туман";
    case 51:
    case 53:
    case 55: return "Мряка";
    case 56:
    case 57: return "Криж.мряка";
    case 61:
    case 63: return "Дощ";
    case 65: return "Сильн.дощ";
    case 66:
    case 67: return "Криж.дощ";
    case 71:
    case 73:
    case 75: return "Сніг";
    case 77: return "Сніж.крупа";
    case 80:
    case 81:
    case 82: return "Злива";
    case 85:
    case 86: return "Снігопад";
    case 95:
    case 96:
    case 99: return "Гроза";
    default: return "Хмарно";
  }
}

String analyzeOutageWindow(const String& scheduleStr, int curHour, int curMin, bool& isCurrentlyOutageSlot) {
  isCurrentlyOutageSlot = false;
  if (scheduleStr.length() < 5) {
    return "Графік 5.2 не задано";
  }

  int curTotalMin = curHour * 60 + curMin;
  int nextStartMin = -1;
  String nextStartStr = "";
  String currentEndStr = "";

  int len = scheduleStr.length();
  for (int i = 0; i <= len - 11; i++) {
    if (isDigit(scheduleStr[i]) && isDigit(scheduleStr[i + 1]) && scheduleStr[i + 2] == ':' &&
        isDigit(scheduleStr[i + 3]) && isDigit(scheduleStr[i + 4]) &&
        (scheduleStr[i + 5] == '-' || scheduleStr[i + 5] == ' ')) {

      int h1 = scheduleStr.substring(i, i + 2).toInt();
      int m1 = scheduleStr.substring(i + 3, i + 5).toInt();

      int dashPos = i + 5;
      while (dashPos < len && !isDigit(scheduleStr[dashPos])) dashPos++;
      if (dashPos + 4 < len && scheduleStr[dashPos + 2] == ':') {
        int h2 = scheduleStr.substring(dashPos, dashPos + 2).toInt();
        int m2 = scheduleStr.substring(dashPos + 3, dashPos + 5).toInt();

        int startMin = h1 * 60 + m1;
        int endMin   = h2 * 60 + m2;

        if (curTotalMin >= startMin && curTotalMin < endMin) {
          isCurrentlyOutageSlot = true;
          currentEndStr = scheduleStr.substring(dashPos, dashPos + 5);
          break;
        } else if (startMin > curTotalMin) {
          if (nextStartMin == -1 || startMin < nextStartMin) {
            nextStartMin = startMin;
            nextStartStr = scheduleStr.substring(i, i + 5);
          }
        }
        i = dashPos + 4;
      }
    }
  }

  if (isCurrentlyOutageSlot) {
    return "ВІДКЛ (5.2) до " + currentEndStr;
  }
  if (nextStartMin != -1) {
    return "Світло є до " + nextStartStr;
  }
  return "Світло є (відкл. позаду)";
}

// ============================================================================
// 5. ЗАПИТИ ДО API: ПОГОДА ТА ТРИВОГИ / СВІТЛО
// ============================================================================
void fetchWeatherFromOpenMeteo() {
  if (WiFi.status() != WL_CONNECTED) return;

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  client->setBufferSizes(2048, 512);

  HTTPClient http;
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(7000);

  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(KAMIANETS_LAT, 4) +
               "&longitude=" + String(KAMIANETS_LON, 4) +
               "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m" +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max" +
               "&wind_speed_unit=ms&timezone=Europe%2FKyiv&forecast_days=2";

  if (http.begin(*client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, http.getStream());
      if (!err) {
        weather.tempNow            = doc["current"]["temperature_2m"] | 0.0f;
        weather.feelsLike          = doc["current"]["apparent_temperature"] | 0.0f;
        weather.humidity           = doc["current"]["relative_humidity_2m"] | 0;
        weather.windSpeed          = doc["current"]["wind_speed_10m"] | 0.0f;
        weather.codeNow            = doc["current"]["weather_code"] | 0;

        weather.tempTomorrowMax    = doc["daily"]["temperature_2m_max"][1] | 0.0f;
        weather.tempTomorrowMin    = doc["daily"]["temperature_2m_min"][1] | 0.0f;
        weather.precipProbTomorrow = doc["daily"]["precipitation_probability_max"][1] | 0;
        weather.codeTomorrow       = doc["daily"]["weather_code"][1] | 0;

        weather.valid = true;
        weather.lastUpdate = millis();
        forceRedraw = true;
      }
    }
    http.end();
  }
}

void fetchAlertAndPowerStatus() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (strlen(cfg.worker_url) > 8) {
    std::unique_ptr<BearSSL::WiFiClientSecure> secClient(new BearSSL::WiFiClientSecure);
    secClient->setInsecure();
    secClient->setBufferSizes(2048, 512);

    HTTPClient http;
    http.useHTTP10(true);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(6000);
    String url = String(cfg.worker_url);

    bool begun = false;
    WiFiClient plainClient;
    if (url.startsWith("https://")) {
      begun = http.begin(*secClient, url);
    } else {
      begun = http.begin(plainClient, url);
    }

    if (begun) {
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getStream());
        if (!err) {
          statusData.alertRaion         = doc["alert_raion"] | false;
          statusData.alertOblast        = doc["alert_oblast"] | false;
          statusData.alertLevel         = doc["alert_level"] | "none";
          statusData.threatType         = doc["threat_type"] | "Немає загроз";
          statusData.threatDirection    = doc["threat_dir"] | "Кам'янець-Под. р-н: спокійно";
          statusData.nearestShahedKm    = doc["shahed_km"] | -1;
          statusData.nearestShahedPlace = doc["shahed_place"] | "";
          statusData.alertStartedEpoch  = doc["started_at"] | 0UL;

          const char* qToday = doc["queue_5_2_today"] | "";
          if (strlen(qToday) > 0) {
            statusData.outageToday = String(qToday);
            strncpy(cfg.schedule_today, qToday, sizeof(cfg.schedule_today) - 1);
          }
          const char* qTom = doc["queue_5_2_tomorrow"] | "";
          if (strlen(qTom) > 0) {
            statusData.outageTomorrow = String(qTom);
            strncpy(cfg.schedule_tomorrow, qTom, sizeof(cfg.schedule_tomorrow) - 1);
          }

          statusData.lastUpdate = millis();
          forceRedraw = true;
        }
      }
      http.end();
    }
    return;
  }

  if (strlen(cfg.alerts_token) > 5) {
    std::unique_ptr<BearSSL::WiFiClientSecure> secClient(new BearSSL::WiFiClientSecure);
    secClient->setInsecure();
    secClient->setBufferSizes(1024, 512);

    HTTPClient http;
    http.useHTTP10(true);
    String url = "https://api.alerts.in.ua/v2/iot/active_air_raid_alerts/" +
                 String(UID_KAMIANETS_RAION) + ".json?token=" + String(cfg.alerts_token);
    http.setTimeout(5000);
    if (http.begin(*secClient, url)) {
      int code = http.GET();
      if (code == HTTP_CODE_OK) {
        String payload = http.getString();
        payload.trim();
        bool isRed    = (payload.indexOf('R') >= 0 || payload.indexOf('A') >= 0);
        bool isYellow = (payload.indexOf('Y') >= 0 || payload.indexOf('P') >= 0);

        bool wasAlert = statusData.alertRaion;
        statusData.alertRaion  = isRed;
        statusData.alertOblast = (isRed || isYellow);
        if (isRed) {
          statusData.alertLevel      = "red";
          statusData.threatType      = "ПОВІТРЯНА ТРИВОГА";
          statusData.threatDirection = "Кам'янець-Подільський р-н";
          if (!wasAlert) statusData.alertStartedEpoch = time(nullptr);
        } else if (isYellow) {
          statusData.alertLevel      = "yellow";
          statusData.threatType      = "Попередня загроза";
          statusData.threatDirection = "Тривога в Хмельницькій обл.";
        } else {
          statusData.alertLevel      = "none";
          statusData.threatType      = "Немає загроз";
          statusData.threatDirection = "Кам'янець-Под. р-н: спокійно";
          statusData.alertStartedEpoch = 0;
        }
        statusData.lastUpdate = millis();
        forceRedraw = true;
      }
      http.end();
    }
    return;
  }

  std::unique_ptr<BearSSL::WiFiClientSecure> secClient(new BearSSL::WiFiClientSecure);
  secClient->setInsecure();
  secClient->setBufferSizes(2048, 512);

  HTTPClient http;
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(6000);
  if (http.begin(*secClient, "https://airraid.net.ua/api/?states=1")) {
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
      String payload = http.getString();
      bool wasAlert = statusData.alertRaion;
      int khmPos = payload.indexOf("Хмельницька область");
      bool active = false;
      if (khmPos >= 0) {
        int maxIdx = payload.length();
        if (khmPos + 140 < maxIdx) maxIdx = khmPos + 140;
        String sub = payload.substring(khmPos, maxIdx);
        active = (sub.indexOf("\"alertnow\":true") >= 0 || sub.indexOf("\"alertnow\":1") >= 0);
      }
      statusData.alertRaion  = active;
      statusData.alertOblast = active;
      if (active) {
        statusData.alertLevel      = "red";
        statusData.threatType      = "ПОВІТРЯНА ТРИВОГА";
        statusData.threatDirection = "Хмельницька область";
        if (!wasAlert) statusData.alertStartedEpoch = time(nullptr);
      } else {
        statusData.alertLevel      = "none";
        statusData.threatType      = "Немає загроз";
        statusData.threatDirection = "Хмельниччина: тривог немає";
        statusData.alertStartedEpoch = 0;
      }
      statusData.lastUpdate = millis();
      forceRedraw = true;
    }
    http.end();
  }
}

// ============================================================================
// 6. ВІДМАЛЬОВУВАННЯ ЕКРАНА 240x240
// ============================================================================
void drawScreen() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  tft.fillScreen(COL_BG_DARK);
  u8f.setFontMode(1);

  // --- 1. ВЕРХНІЙ БЛОК: ПОГОДА + КВАДРАТНИЙ ГОДИННИК СПРАВА (y = 3..75, висота 72px) ---
  tft.fillRoundRect(4, 3, 232, 72, 6, COL_CARD_BG);

  uint16_t clockBoxBg = COL_HEADER_SAFE;
  if (statusData.alertRaion) clockBoxBg = COL_HEADER_ALERT;
  else if (statusData.alertOblast || statusData.nearestShahedKm > 0) clockBoxBg = COL_HEADER_WARN;

  tft.fillRoundRect(184, 7, 47, 64, 6, clockBoxBg);
  char hhBuf[4];
  char mmBuf[4];
  snprintf(hhBuf, sizeof(hhBuf), "%02d", timeinfo.tm_hour);
  snprintf(mmBuf, sizeof(mmBuf), "%02d", timeinfo.tm_min);

  u8f.setFont(u8g2_font_inr24_mn);
  u8f.setForegroundColor(COL_TEXT_WHITE);
  u8f.setCursor(192, 35);
  u8f.print(hhBuf);
  u8f.setCursor(192, 65);
  u8f.print(mmBuf);

  u8f.setFont(u8g2_font_8x13_t_cyrillic);
  if (weather.valid) {
    u8f.setForegroundColor(COL_TEXT_YELLOW);
    u8f.setCursor(10, 21);
    char wNowBuf[40];
    snprintf(wNowBuf, sizeof(wNowBuf), "Зараз:%+.0f°(відч%+.0f°)", weather.tempNow, weather.feelsLike);
    u8f.print(utf8Truncate(String(wNowBuf), 21));

    u8f.setForegroundColor(COL_TEXT_CYAN);
    u8f.setCursor(10, 43);
    String condNow = weatherCodeToText(weather.codeNow) + " " + String(weather.humidity) +
                     "% " + String(weather.windSpeed, 0) + "м/с";
    u8f.print(utf8Truncate(condNow, 21));

    u8f.setForegroundColor(COL_TEXT_WHITE);
    u8f.setCursor(10, 65);
    char wTomBuf[48];
    snprintf(wTomBuf, sizeof(wTomBuf), "Завтра:%+.0f..%+.0f°(%d%%)",
             weather.tempTomorrowMin, weather.tempTomorrowMax,
             weather.precipProbTomorrow);
    u8f.print(utf8Truncate(String(wTomBuf), 21));
  } else {
    u8f.setForegroundColor(COL_TEXT_GRAY);
    u8f.setCursor(10, 43);
    u8f.print("Оновлення погоди...");
  }

  // --- 2. СЕРЕДНІЙ БЛОК: ТРИВОГИ ТА БПЛА (y = 79..133, висота 54px) ---
  uint16_t alertCardBg = COL_CARD_BG;
  if (statusData.alertRaion) {
    alertCardBg = COL_HEADER_ALERT;
  } else if (statusData.alertOblast || statusData.nearestShahedKm > 0) {
    alertCardBg = COL_HEADER_WARN;
  }

  tft.fillRoundRect(4, 79, 232, 54, 6, alertCardBg);
  u8f.setFont(u8g2_font_8x13_t_cyrillic);

  if (statusData.alertRaion) {
    unsigned long durMins = 0;
    if (statusData.alertStartedEpoch > 100000 && (unsigned long)now >= statusData.alertStartedEpoch) {
      durMins = ((unsigned long)now - statusData.alertStartedEpoch) / 60;
    }
    String alertTitle = "ТРИВОГА! (" + String(durMins) + " хв)";
    u8f.setForegroundColor(COL_TEXT_WHITE);
    u8f.setCursor(10, 96);
    u8f.print(utf8Truncate(alertTitle, 27));

    printWrappedUtf8(statusData.threatDirection, 10, 113, 16, 27, 2);
  } else if (statusData.nearestShahedKm > 0) {
    String distLine = "БПЛА: ~" + String(statusData.nearestShahedKm) + " км";
    if (statusData.nearestShahedPlace.length() > 0) {
      distLine += " (" + statusData.nearestShahedPlace + ")";
    }
    u8f.setForegroundColor(COL_TEXT_WHITE);
    u8f.setCursor(10, 96);
    u8f.print(utf8Truncate(distLine, 27));

    printWrappedUtf8(statusData.threatDirection, 10, 113, 16, 27, 2);
  } else if (statusData.alertOblast) {
    u8f.setForegroundColor(COL_TEXT_WHITE);
    u8f.setCursor(10, 96);
    u8f.print("Тривога в області");

    printWrappedUtf8(statusData.threatDirection, 10, 113, 16, 27, 2);
  } else {
    u8f.setForegroundColor(COL_TEXT_GREEN);
    u8f.setCursor(10, 96);
    u8f.print("Район: без тривог і БПЛА");

    u8f.setForegroundColor(COL_TEXT_GRAY);
    printWrappedUtf8(statusData.threatDirection, 10, 113, 16, 27, 2);
  }

  // --- 3. НИЖНІЙ БЛОК: ГРАФІК СВІТЛА 5.2 (y = 137..237, висота 100px) ---
  tft.fillRoundRect(4, 137, 232, 100, 6, COL_OUTAGE_BG);

  bool inOutageSlot = false;
  String powerHeadline = analyzeOutageWindow(statusData.outageToday, timeinfo.tm_hour, timeinfo.tm_min, inOutageSlot);

  u8f.setFont(u8g2_font_8x13_t_cyrillic);
  u8f.setForegroundColor(inOutageSlot ? COL_TEXT_PINK : COL_TEXT_GREEN);
  u8f.setCursor(10, 154);
  u8f.print(utf8Truncate(powerHeadline, 27));

  u8f.setForegroundColor(COL_TEXT_WHITE);
  String lineToday = "Сьогодні: " + statusData.outageToday;
  int linesToday = printWrappedUtf8(lineToday, 10, 173, 17, 27, 2);

  int yTom = 173 + linesToday * 17 + 2;
  u8f.setForegroundColor(COL_TEXT_GRAY);
  String lineTom = "Завтра: " + statusData.outageTomorrow;
  printWrappedUtf8(lineTom, 10, yTom, 17, 27, 2);
}

// ============================================================================
// 7. ВЕБ-ІНТЕРФЕЙС НАЛАШТУВАНЬ + ELEGANT OTA
// ============================================================================
void handleRoot() {
  String html = F("<!DOCTYPE html><html lang='uk'><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>Кубик Кам'янець-Подільський (Черга 5.2)</title>"
                  "<style>body{font-family:sans-serif;background:#0f172a;color:#f8fafc;max-width:520px;margin:16px auto;padding:16px}"
                  ".card{background:#1e293b;padding:16px;border-radius:12px;margin-bottom:16px}"
                  "input{width:100%;padding:8px;margin:6px 0 12px;border-radius:6px;border:1px solid #475569;background:#0f172a;color:#fff;box-sizing:border-box}"
                  "button,.btn{display:inline-block;background:#2563eb;color:#fff;padding:10px 16px;border:none;border-radius:8px;text-decoration:none;cursor:pointer;font-weight:bold}"
                  ".ota{background:#059669;margin-top:8px}</style></head><body>"
                  "<h2>Кубик: Кам'янець-Подільський</h2>"
                  "<div class='card'><p><b>Адреса:</b> вул. Розвадовського, 5 (Черга 5.2)</p>");

  html += "<p><b>Погода зараз:</b> " + String(weather.tempNow, 1) + " &deg;C (" + weatherCodeToText(weather.codeNow) +
          ", вітер " + String(weather.windSpeed, 1) + " м/с)</p>";
  html += "<p><b>Тривога (UID 135):</b> " + String(statusData.alertRaion ? "АКТИВНА" : "Немає") +
          " (" + statusData.threatDirection + ")</p>";
  html += "<p><a class='btn ota' href='/update'>Прошивка по повітрю (ElegantOTA)</a></p></div>";

  html += F("<form class='card' method='POST' action='/save'>"
            "<h3>Налаштування джерел та графіка 5.2</h3>"
            "<label>Графік черги 5.2 на сьогодні:</label>");
  html += "<input name='s_today' value='" + String(cfg.schedule_today) + "'>";

  html += F("<label>Графік черги 5.2 на завтра:</label>");
  html += "<input name='s_tom' value='" + String(cfg.schedule_tomorrow) + "'>";

  html += F("<label>URL вашого Cloudflare Worker:</label>");
  html += "<input name='w_url' value='" + String(cfg.worker_url) + "' placeholder='https://cube.alexanderbogatov97.workers.dev/'>";

  html += F("<label>Токен alerts.in.ua (необов'язково):</label>");
  html += "<input name='a_tok' value='" + String(cfg.alerts_token) + "'>";

  html += F("<label>Яскравість екрана (10..100%):</label>");
  html += "<input type='number' name='bright' min='10' max='100' value='" + String(cfg.brightness) + "'>";

  html += F("<label>Поворот екрана (0, 1, 2 або 3):</label>");
  html += "<input type='number' name='rot' min='0' max='3' value='" + String(cfg.rotation) + "'>";

  html += F("<label>Wi-Fi мережа (SSID):</label>");
  html += "<input name='ssid' value='" + String(cfg.wifi_ssid) + "'>";
  html += F("<label>Пароль Wi-Fi (залиште порожнім, щоб не змінювати):</label>"
            "<input type='password' name='pass' placeholder='********'>"
            "<button type='submit'>Зберегти та оновити екран</button></form></body></html>");

  server.send(200, "text/html; charset=utf-8", html);
}

void handleSave() {
  if (server.hasArg("s_today")) {
    strncpy(cfg.schedule_today, server.arg("s_today").c_str(), sizeof(cfg.schedule_today) - 1);
    statusData.outageToday = String(cfg.schedule_today);
  }
  if (server.hasArg("s_tom")) {
    strncpy(cfg.schedule_tomorrow, server.arg("s_tom").c_str(), sizeof(cfg.schedule_tomorrow) - 1);
    statusData.outageTomorrow = String(cfg.schedule_tomorrow);
  }
  if (server.hasArg("w_url")) {
    strncpy(cfg.worker_url, server.arg("w_url").c_str(), sizeof(cfg.worker_url) - 1);
  }
  if (server.hasArg("a_tok")) {
    strncpy(cfg.alerts_token, server.arg("a_tok").c_str(), sizeof(cfg.alerts_token) - 1);
  }
  if (server.hasArg("bright")) {
    cfg.brightness = (uint8_t)constrain(server.arg("bright").toInt(), 10, 100);
    applyBacklight(cfg.brightness);
  }
  if (server.hasArg("rot")) {
    cfg.rotation = (uint8_t)constrain(server.arg("rot").toInt(), 0, 3);
    tft.setRotation(cfg.rotation);
  }
  if (server.hasArg("ssid") && server.arg("ssid").length() > 0) {
    strncpy(cfg.wifi_ssid, server.arg("ssid").c_str(), sizeof(cfg.wifi_ssid) - 1);
  }
  if (server.hasArg("pass") && server.arg("pass").length() > 0) {
    strncpy(cfg.wifi_pass, server.arg("pass").c_str(), sizeof(cfg.wifi_pass) - 1);
  }

  saveConfig();
  fetchWeatherFromOpenMeteo();
  fetchAlertAndPowerStatus();
  forceRedraw = true;

  server.sendHeader("Location", "/");
  server.send(303);
}

// ============================================================================
// 8. SETUP ТА LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  loadConfig();

  pinMode(TFT_BL_PIN, OUTPUT);
  applyBacklight(cfg.brightness);

  tft.init();
  tft.setRotation(cfg.rotation);
  tft.fillScreen(COL_BG_DARK);

  u8f.begin(tft);
  u8f.setFontMode(1);
  u8f.setFont(u8g2_font_8x13_t_cyrillic);
  u8f.setForegroundColor(COL_TEXT_WHITE);
  u8f.setCursor(12, 40);
  u8f.print("Підключення до 2,4/5G...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("Kamianets-Cube-OTA");
  }

  configTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "europe.pool.ntp.org", "pool.ntp.org");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  ElegantOTA.begin(&server);
  server.begin();

  fetchWeatherFromOpenMeteo();
  fetchAlertAndPowerStatus();
  drawScreen();
}

void loop() {
  server.handleClient();
  ElegantOTA.loop();

  unsigned long nowMs = millis();

  if (nowMs - lastAlertFetch >= 20000UL) {
    lastAlertFetch = nowMs;
    fetchAlertAndPowerStatus();
  }

  if (nowMs - lastWeatherFetch >= 900000UL || !weather.valid) {
    if (nowMs - lastWeatherFetch >= 30000UL) {
      lastWeatherFetch = nowMs;
      fetchWeatherFromOpenMeteo();
    }
  }

  time_t curNow = time(nullptr);
  struct tm curTm;
  localtime_r(&curNow, &curTm);

  if (forceRedraw || curTm.tm_min != lastDrawnMinute) {
    lastDrawnMinute = curTm.tm_min;
    forceRedraw = false;
    drawScreen();
  }
}
