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

const char* DEFAULT_WORKER_URL    = "";
const char* DEFAULT_ALERTS_TOKEN  = "";

// Координати м. Кам'янець-Подільський (вул. Розвадовського, 5)
const float KAMIANETS_LAT         = 48.6845f;
const float KAMIANETS_LON         = 26.5856f;

// UID локацій у сервісі alerts.in.ua
const int   UID_KAMIANETS_RAION   = 135; // Кам'янець-Подільський район
const int   UID_KHMELNYTSKYI_OBL  = 3;   // Хмельницька область

const char* DEFAULT_QUEUE_TODAY   = "06:00-09:00; 16:00-19:00";
const char* DEFAULT_QUEUE_TOMORROW= "Очікується від Хмельницькобленерго";

#ifndef TFT_BL_PIN
#define TFT_BL_PIN 5
#endif

// Кольори RGB565 для екрана 240x240
#define COL_BG_DARK       0x0863
#define COL_HEADER_SAFE   0x0312
#define COL_HEADER_WARN   0xD340
#define COL_HEADER_ALERT  0xE129
#define COL_CARD_BG       0x10E5
#define COL_OUTAGE_BG     0x18E7
#define COL_TEXT_WHITE    0xFFFF
#define COL_TEXT_CYAN     0x67FF
#define COL_TEXT_YELLOW   0xFFE0
#define COL_TEXT_GREEN    0x4FE8
#define COL_TEXT_PINK     0xF993
#define COL_TEXT_GRAY     0x9CD3

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
  String threatDirection = "В районі спокійно";
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

unsigned long lastClockDraw = 0;
unsigned long lastWeatherFetch = 0;
unsigned long lastAlertFetch = 0;
bool forceRedraw = true;

// ============================================================================
// 3. ДОПОМІЖНІ ФУНКЦІЇ (UTF-8 ОБРІЗКА, EEPROM, ПІДСВІТКА)
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
    case 2:  return "Мінлива хмарн.";
    case 3:  return "Хмарно";
    case 45:
    case 48: return "Туман";
    case 51:
    case 53:
    case 55: return "Мряка";
    case 56:
    case 57: return "Крижана мряка";
    case 61:
    case 63: return "Дощ";
    case 65: return "Сильний дощ";
    case 66:
    case 67: return "Крижаний дощ";
    case 71:
    case 73:
    case 75: return "Сніг";
    case 77: return "Снігова крупа";
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

  WiFiClient client;
  HTTPClient http;

  String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(KAMIANETS_LAT, 4) +
               "&longitude=" + String(KAMIANETS_LON, 4) +
               "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m" +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max" +
               "&timezone=Europe%2FKyiv&forecast_days=2";

  http.setTimeout(5000);
  if (http.begin(client, url)) {
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

  // Варіант А: Cloudflare Worker
  if (strlen(cfg.worker_url) > 8) {
    std::unique_ptr<BearSSL::WiFiClientSecure> secClient(new BearSSL::WiFiClientSecure);
    secClient->setInsecure();
    secClient->setBufferSizes(2048, 512);

    HTTPClient http;
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
          statusData.threatDirection    = doc["threat_dir"] | "В районі спокійно";
          statusData.nearestShahedKm    = doc["shahed_km"] | -1;
          statusData.nearestShahedPlace = doc["shahed_place"] | "";
          statusData.alertStartedEpoch  = doc["started_at"] | 0UL;

          const char* qToday = doc["queue_5_2_today"];
          if (qToday && strlen(qToday) > 0) {
            statusData.outageToday = String(qToday);
            strncpy(cfg.schedule_today, qToday, sizeof(cfg.schedule_today) - 1);
          }
          const char* qTom = doc["queue_5_2_tomorrow"];
          if (qTom && strlen(qTom) > 0) {
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

  // Варіант Б: Прямий запит до alerts.in.ua з токеном
  if (strlen(cfg.alerts_token) > 5) {
    std::unique_ptr<BearSSL::WiFiClientSecure> secClient(new BearSSL::WiFiClientSecure);
    secClient->setInsecure();
    secClient->setBufferSizes(1024, 512);

    HTTPClient http;
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
          if (!wasAlert) {
            statusData.alertStartedEpoch = time(nullptr);
          }
        } else if (isYellow) {
          statusData.alertLevel      = "yellow";
          statusData.threatType      = "Попередня загроза";
          statusData.threatDirection = "Постріли / БПЛА в області";
        } else {
          statusData.alertLevel      = "none";
          statusData.threatType      = "Немає загроз";
          statusData.threatDirection = "Кам'янець-Под. р-н: тихо";
          statusData.alertStartedEpoch = 0;
        }
        statusData.lastUpdate = millis();
        forceRedraw = true;
      }
      http.end();
    }
    return;
  }

  // Варіант В: Відкритий API Повітряних тривог Ubilling (без ключів)
  WiFiClient plainClient;
  HTTPClient http;
  http.setTimeout(5000);
  if (http.begin(plainClient, "http://airraid.net.ua/api/?states=1")) {
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
      String payload = http.getString();
      bool wasAlert = statusData.alertRaion;
      int khmPos = payload.indexOf("Хмельницька область");
      bool active = false;
      if (khmPos >= 0) {
        String sub = payload.substring(khmPos, min((int)payload.length(), khmPos + 140));
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
        statusData.threatDirection = "Хмельниччина: спокійно";
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

  char timeHHMM[8];
  snprintf(timeHHMM, sizeof(timeHHMM), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

  uint16_t headerColor = COL_HEADER_SAFE;
  if (statusData.alertRaion) {
    headerColor = COL_HEADER_ALERT;
  } else if (statusData.alertOblast || statusData.nearestShahedKm > 0) {
    headerColor = COL_HEADER_WARN;
  }

  // --- 1. ВЕРХНЯ ШАПКА ---
  tft.fillRect(0, 0, 240, 68, headerColor);

  u8f.setFontMode(1);
  u8f.setForegroundColor(COL_TEXT_WHITE);
  u8f.setFont(u8g2_font_7x13B_t_cyrillic);

  if (statusData.alertRaion) {
    u8f.setCursor(8, 16);
    u8f.print("ТРИВОГА • Кам'янець");
    u8f.setCursor(192, 16);
    u8f.print(timeHHMM);

    unsigned long durSec = 0;
    if (statusData.alertStartedEpoch > 100000 && (unsigned long)now >= statusData.alertStartedEpoch) {
      durSec = (unsigned long)now - statusData.alertStartedEpoch;
    }
    char durBuf[12];
    unsigned int mins = durSec / 60;
    unsigned int secs = durSec % 60;
    if (mins >= 60) {
      snprintf(durBuf, sizeof(durBuf), "%02u:%02u:%02u", mins / 60, mins % 60, secs);
    } else {
      snprintf(durBuf, sizeof(durBuf), "%02u:%02u", mins, secs);
    }
    u8f.setFont(u8g2_font_inr24_tn);
    u8f.setForegroundColor(COL_TEXT_CYAN);
    u8f.setCursor(8, 56);
    u8f.print(durBuf);
  } else {
    u8f.setCursor(8, 16);
    if (statusData.nearestShahedKm > 0) {
      u8f.print("УВАГА • БПЛА в регіоні");
    } else {
      u8f.print("Кам'янець-Подільський");
    }

    char fullTime[12];
    snprintf(fullTime, sizeof(fullTime), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    u8f.setFont(u8g2_font_inr24_tn);
    u8f.setForegroundColor(COL_TEXT_WHITE);
    u8f.setCursor(8, 56);
    u8f.print(fullTime);
  }

  // --- 2. СЕРЕДНІЙ БЛОК ---
  tft.fillRect(0, 68, 240, 116, COL_BG_DARK);

  tft.fillRoundRect(4, 72, 232, 46, 5, COL_CARD_BG);
  u8f.setFont(u8g2_font_7x13B_t_cyrillic);

  if (statusData.alertRaion || statusData.nearestShahedKm > 0) {
    u8f.setForegroundColor(COL_TEXT_PINK);
    u8f.setCursor(10, 88);
    if (statusData.nearestShahedKm > 0) {
      String distLine = "Шахед/ціль: ~" + String(statusData.nearestShahedKm) + " км";
      if (statusData.nearestShahedPlace.length() > 0) {
        distLine += " (" + statusData.nearestShahedPlace + ")";
      }
      u8f.print(utf8Truncate(distLine, 30));
    } else {
      u8f.print(utf8Truncate(statusData.threatType, 30));
    }

    u8f.setFont(u8g2_font_6x13_t_cyrillic);
    u8f.setForegroundColor(COL_TEXT_WHITE);
    u8f.setCursor(10, 108);
    u8f.print(utf8Truncate(statusData.threatDirection, 35));
  } else {
    u8f.setForegroundColor(COL_TEXT_GREEN);
    u8f.setCursor(10, 88);
    u8f.print("Район: без тривог і БПЛА");

    u8f.setFont(u8g2_font_6x13_t_cyrillic);
    u8f.setForegroundColor(COL_TEXT_GRAY);
    u8f.setCursor(10, 108);
    u8f.print("Розвадовського, 5 • Черга 5.2");
  }

  tft.fillRoundRect(4, 122, 232, 58, 5, COL_CARD_BG);
  if (weather.valid) {
    u8f.setFont(u8g2_font_7x13B_t_cyrillic);
    u8f.setForegroundColor(COL_TEXT_YELLOW);
    u8f.setCursor(10, 139);
    char wNowBuf[48];
    snprintf(wNowBuf, sizeof(wNowBuf), "Зараз: %+.0f°C (відч. %+.0f°)", weather.tempNow, weather.feelsLike);
    u8f.print(wNowBuf);

    u8f.setFont(u8g2_font_6x13_t_cyrillic);
    u8f.setForegroundColor(COL_TEXT_CYAN);
    u8f.setCursor(10, 155);
    String condNow = weatherCodeToText(weather.codeNow) + " • Вол:" + String(weather.humidity) +
                     "% • Вітер:" + String(weather.windSpeed, 0) + "м/с";
    u8f.print(utf8Truncate(condNow, 36));

    u8f.setForegroundColor(COL_TEXT_WHITE);
    u8f.setCursor(10, 173);
    char wTomBuf[64];
    snprintf(wTomBuf, sizeof(wTomBuf), "Завтра: %+.0f..%+.0f°C, %s (%d%%)",
             weather.tempTomorrowMin, weather.tempTomorrowMax,
             weatherCodeToText(weather.code
