// ============================================================
//  E-INK DASHBOARD + TURN-BASED DINO GAME + AM/PM ALARM
//  - Differential Refresh (Strictly updates changed bits only)
//  - Zero flashing during Alarm editing (Strict Partial Refresh)
//  - Full Screen Scrub ONLY when arriving at Screen 0 or 1-Min Sync
//  - WiFi connects exactly every 60 seconds
//  - 6-Screen UI including standalone Location menu
// ============================================================
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "SparkFun_ENS160.h"
#include <Adafruit_AHTX0.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include "time.h"
#include "esp_sntp.h"
#include <esp_sleep.h>
#include "driver/rtc_io.h"

// ==========================================
// 1. SETTINGS & PINS
// ==========================================
#define BUTTON_PIN   27
#define BUZZER_PIN   26

const char* ssid     = "Shinei Nouzen";
const char* password = "thebeginningistheend";

long gmtOffset_sec      = 6 * 3600;   // Dhaka Time
int  daylightOffset_sec = 0;

const int NUM_SCREENS = 6; // 0=Home, 1=Indoor, 2=Outdoor, 3=Calendar, 4=Alarm, 5=Location

// --- Display refresh behaviour ---
const int  PARTIAL_TOP                 = 0;     
const int  NIGHT_CLEAN_HOUR            = 3;      

// --- Timing ---
const int      WAKE_OFFSET_SEC     = 2;          
const uint32_t SYNC_INTERVAL_SEC   = 10 * 60;         // WiFi + NTP time sync EVERY 1 MINUTE
const uint32_t WEATHER_REFRESH_SEC = 10 * 60;    

const int ALARM_GRACE_MIN    = 3;     
const int ALARM_MAX_RING_SEC = 120;   
const bool ENS160_ALWAYS_ON  = true;

// ==========================================
// 2. RTC MEMORY (Survives deep sleep)
// ==========================================
RTC_DATA_ATTR int      currentScreen    = 0;
RTC_DATA_ATTR bool     rtc_displayReady = false;
RTC_DATA_ATTR int      rtc_lastView     = -1;     
RTC_DATA_ATTR uint32_t rtc_lastSig      = 0;      
RTC_DATA_ATTR int      rtc_lastCleanKey = -1;     

// Alarm
RTC_DATA_ATTR bool   rtc_alarm_on = false;
RTC_DATA_ATTR int    rtc_alarm_h  = 6;            
RTC_DATA_ATTR int    rtc_alarm_m  = 30;
RTC_DATA_ATTR int    rtc_alarm_edit_state = 0;    
RTC_DATA_ATTR time_t rtc_alarm_last_ring = 0;     
RTC_DATA_ATTR bool   rtc_alarm_is_ringing = false;

// Dino game
RTC_DATA_ATTR bool rtc_in_dino_game  = false;
RTC_DATA_ATTR bool rtc_game_auto     = false;     
RTC_DATA_ATTR int  rtc_cactus_x      = 200;
RTC_DATA_ATTR int  rtc_dino_y        = 120;
RTC_DATA_ATTR bool rtc_dino_jumping  = false;
RTC_DATA_ATTR int  rtc_dino_score    = 0;

// Indoor 
RTC_DATA_ATTR bool  rtc_ahtOk   = false;
RTC_DATA_ATTR int   rtc_ensFlag = 4;              
RTC_DATA_ATTR float rtc_temp = 26.9;
RTC_DATA_ATTR float rtc_hum  = 62.0;
RTC_DATA_ATTR int   rtc_aqi  = 2;
RTC_DATA_ATTR int   rtc_eco2 = 640;
RTC_DATA_ATTR int   rtc_tvoc = 120;                  

// Location
RTC_DATA_ATTR char   rtc_city[32]    = "Dhaka";
RTC_DATA_ATTR char   rtc_country[32] = "BD";
RTC_DATA_ATTR float  rtc_lat = 0.0;
RTC_DATA_ATTR float  rtc_lon = 0.0;
RTC_DATA_ATTR bool   rtc_haveLoc = false;
RTC_DATA_ATTR char   rtc_region[32]  = "";        // state / division (ip-api regionName)
RTC_DATA_ATTR char   rtc_tz[32]      = "";        // e.g. Asia/Dhaka
RTC_DATA_ATTR char   rtc_isp[32]     = "";        // internet provider
RTC_DATA_ATTR char   rtc_ip[16]      = "";        // public IP
RTC_DATA_ATTR int32_t rtc_utcOffset  = 0;         // seconds east of UTC
RTC_DATA_ATTR time_t rtc_locFetch    = 0;         // when location was last updated
RTC_DATA_ATTR bool   rtc_locForce    = false;     // double-click on Location = re-locate
const uint32_t LOCATION_REFRESH_SEC = 6UL * 3600; // re-check location every 6 h

// Outdoor air & weather
RTC_DATA_ATTR float  rtc_pm25 = 42.0;
RTC_DATA_ATTR float  rtc_pm10 = 88.0;
RTC_DATA_ATTR float  rtc_out_temp = 31.0;
RTC_DATA_ATTR float  rtc_out_hum = 70.0;
RTC_DATA_ATTR float  rtc_out_feels = 36.0;
RTC_DATA_ATTR float  rtc_out_wind = 3.1;
RTC_DATA_ATTR int    rtc_weather_code = 0;
RTC_DATA_ATTR bool   rtc_haveWeather = false;
RTC_DATA_ATTR bool   rtc_haveAq = false;
RTC_DATA_ATTR time_t rtc_lastWxFetch = 0;
RTC_DATA_ATTR time_t rtc_lastSyncAttempt = 0;

// ==========================================
// 3. DRIVERS & DISPLAY
// ==========================================
SparkFun_ENS160 myENS;
Adafruit_AHTX0 aht;
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(GxEPD2_154_D67(5, 17, 16, 4));

bool ensTouched = false;   

const char* monthNames[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
const char* dayLetters[] = {"S", "M", "T", "W", "T", "F", "S"};

int getDaysInMonth(int month, int year) {
  if (month == 1) {
    if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) return 29;
    return 28;
  }
  if (month == 3 || month == 5 || month == 8 || month == 10) return 30;
  return 31;
}

// US EPA AQI from PM2.5 - 2024 breakpoints ("Good" <= 9.0 ug/m3, was 12.0 before May 2024)
int getUSAQI(float pm25) {
  static const float bp[6][4] = {        // {C_low, C_high, I_low, I_high}
    {0.0, 9.0, 0, 50}, {9.1, 35.4, 51, 100}, {35.5, 55.4, 101, 150},
    {55.5, 125.4, 151, 200}, {125.5, 225.4, 201, 300}, {225.5, 500.4, 301, 500}};
  if (pm25 < 0) pm25 = 0;
  pm25 = floorf(pm25 * 10.0f) / 10.0f;   // EPA: truncate to 0.1 ug/m3
  for (int i = 0; i < 6; i++)
    if (pm25 <= bp[i][1])
      return (int)lroundf((bp[i][3] - bp[i][2]) / (bp[i][1] - bp[i][0]) * (pm25 - bp[i][0]) + bp[i][2]);
  return 500;
}

const char* getAQIStatus(int aqi) {
  if (aqi <= 50) return "Good";
  if (aqi <= 100) return "Moderate";
  if (aqi <= 150) return "Sensitive";
  if (aqi <= 200) return "Unhealthy";
  if (aqi <= 300) return "V.Unhealthy";
  return "Hazardous";
}

const char* getIndoorStatus(int aqi) {
  if (aqi == 1) return "Excellent";
  if (aqi == 2) return "Good";
  if (aqi == 3) return "Moderate";
  if (aqi == 4) return "Poor";
  return "Unhealthy";
}

const char* getWeatherString(int code) {
  if (code <= 1) return "Clear";
  if (code <= 3) return "Partly Cloudy";
  if (code <= 48) return "Haze";
  if (code <= 67) return "Rain";
  if (code <= 77) return "Snow";
  if (code <= 82) return "Showers";
  if (code >= 95) return "Thunderstorm";
  return "Unknown";
}

// ==========================================
// 4. ALARM
// ==========================================
bool alarmIsDue(struct tm &t, bool timeOk, time_t now) {
  if (!rtc_alarm_on || !timeOk) return false;
  int nowMin   = t.tm_hour * 60 + t.tm_min;
  int alarmMin = rtc_alarm_h * 60 + rtc_alarm_m;
  int diff = (nowMin - alarmMin + 1440) % 1440;            
  if (diff >= ALARM_GRACE_MIN) return false;
  if ((now - rtc_alarm_last_ring) < 600) return false;      
  return true;
}

void rearmAlarm(struct tm &t, bool timeOk, time_t now) {
  if (!timeOk) { rtc_alarm_last_ring = 0; return; }
  int nowMin   = t.tm_hour * 60 + t.tm_min;
  int alarmMin = rtc_alarm_h * 60 + rtc_alarm_m;
  int diff = (nowMin - alarmMin + 1440) % 1440;
  rtc_alarm_last_ring = (diff < ALARM_GRACE_MIN) ? now : 0;
}

bool waitOrPressed(unsigned long ms) {                     
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    if (digitalRead(BUTTON_PIN) == LOW) return true;
    delay(5);
  }
  return false;
}

void ringAlarm() {
  rtc_alarm_is_ringing = true;
  unsigned long t0 = millis();
  while (millis() - t0 < (unsigned long)ALARM_MAX_RING_SEC * 1000UL) {
    for (int i = 0; i < 4; i++) {
      digitalWrite(BUZZER_PIN, HIGH);
      if (waitOrPressed(100)) goto end_alarm;
      digitalWrite(BUZZER_PIN, LOW);
      if (waitOrPressed(100)) goto end_alarm;
    }
    if (waitOrPressed(600)) goto end_alarm;
  }
  
end_alarm:
  digitalWrite(BUZZER_PIN, LOW);
  while (digitalRead(BUTTON_PIN) == LOW) delay(10);        
  delay(50);
  rtc_alarm_is_ringing = false;
}

// ==========================================
// 5. INDOOR SENSORS 
// ==========================================
void readIndoorSensors() {
  Wire.begin();
  rtc_ahtOk = false;
  if (aht.begin()) {
    sensors_event_t hum, temp;
    if (aht.getEvent(&hum, &temp)) {
      rtc_temp = temp.temperature;
      rtc_hum  = hum.relative_humidity;
      rtc_ahtOk = true;
    }
  }

  rtc_ensFlag = 4;
  bool ensFound = myENS.begin();
  if (!ensFound) ensFound = myENS.begin(0x52);

  if (ensFound) {
    ensTouched = true;
    rtc_ensFlag = 3;
    if (myENS.getOperatingMode() != SFE_ENS160_STANDARD) {
      myENS.setOperatingMode(SFE_ENS160_STANDARD);
      delay(50);
    }
    if (rtc_ahtOk && rtc_hum > 0.5f && rtc_hum < 99.5f && rtc_temp > -20.0f && rtc_temp < 70.0f) {
      myENS.setTempCompensationCelsius(rtc_temp);
      myENS.setRHCompensationFloat(rtc_hum);
    }

    unsigned long t0 = millis();
    bool ready = false;
    while (millis() - t0 < 3000) {
      if (myENS.checkDataStatus()) { ready = true; break; }
      delay(100);
    }
    if (ready) {
      uint8_t f   = myENS.getFlags();
      rtc_ensFlag = (f <= 3) ? f : 3;     
      rtc_aqi     = myENS.getAQI();
      rtc_eco2    = myENS.getECO2();
      rtc_tvoc    = myENS.getTVOC();      
    }
  }
}

// ==========================================
// 6. NETWORK & APIS
// ==========================================
void fetchLocationAndWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;

  time_t nowL = time(nullptr);
  bool locStale = (nowL > 1700000000) && (rtc_locFetch == 0 || (nowL - rtc_locFetch) >= (time_t)LOCATION_REFRESH_SEC);
  if (!rtc_haveLoc || locStale || rtc_locForce) {
    // ip-api free endpoint is HTTP only; ask for every field the Location screen shows
    http.begin("http://ip-api.com/json/?fields=status,message,country,countryCode,regionName,city,lat,lon,timezone,offset,isp,query");
    http.setTimeout(5000);
    if (http.GET() == 200) {
      DynamicJsonDocument doc(1024);
      if (!deserializeJson(doc, http.getString())) {
        const char* st = doc["status"] | "fail";
        if (strcmp(st, "success") == 0) {          // BUG FIX: previously a "fail" reply was stored as a valid location (0,0)
          strlcpy(rtc_city,    doc["city"]       | "Unknown", sizeof(rtc_city));
          strlcpy(rtc_country, doc["country"]    | "Unknown", sizeof(rtc_country));
          strlcpy(rtc_region,  doc["regionName"] | "",        sizeof(rtc_region));
          strlcpy(rtc_tz,      doc["timezone"]   | "",        sizeof(rtc_tz));
          strlcpy(rtc_isp,     doc["isp"]        | "",        sizeof(rtc_isp));
          strlcpy(rtc_ip,      doc["query"]      | "",        sizeof(rtc_ip));
          rtc_utcOffset = doc["offset"] | 0;
          rtc_lat = doc["lat"] | 0.0f; rtc_lon = doc["lon"] | 0.0f;
          rtc_haveLoc = true;
          rtc_locFetch = nowL;
          Serial.printf("Location: %s, %s, %s (%.4f, %.4f)\n", rtc_city, rtc_region, rtc_country, rtc_lat, rtc_lon);
        } else {
          Serial.printf("ip-api failed: %s\n", (const char*)(doc["message"] | "?"));
        }
      }
    }
    http.end();
    rtc_locForce = false;
  }

  if (!rtc_haveLoc) return;

  time_t now = time(nullptr);
  bool timeOk = (now > 1700000000);
  
  bool needWx = (!rtc_haveWeather || !rtc_haveAq) || (timeOk && (now - rtc_lastWxFetch) >= (time_t)WEATHER_REFRESH_SEC);
  if (!needWx) return;

  String coords = "latitude=" + String(rtc_lat, 4) + "&longitude=" + String(rtc_lon, 4);

  http.begin("http://api.open-meteo.com/v1/forecast?" + coords + "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m");
  if (http.GET() == 200) {
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, http.getString())) {
      rtc_out_temp = doc["current"]["temperature_2m"] | 0.0f;
      rtc_out_hum = doc["current"]["relative_humidity_2m"] | 0.0f;
      rtc_out_feels = doc["current"]["apparent_temperature"] | 0.0f;
      rtc_out_wind = doc["current"]["wind_speed_10m"] | 0.0f;
      rtc_weather_code = doc["current"]["weather_code"] | 0;
      rtc_haveWeather = true;
    }
  }
  http.end();

  http.begin("http://air-quality-api.open-meteo.com/v1/air-quality?" + coords + "&current=pm10,pm2_5");
  if (http.GET() == 200) {
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, http.getString())) {
      rtc_pm25 = doc["current"]["pm2_5"] | 0.0f;
      rtc_pm10 = doc["current"]["pm10"]  | 0.0f;
      rtc_haveAq = true;
    }
  }
  http.end();

  if (timeOk) rtc_lastWxFetch = now;
}

void updateFromNetwork() {
  rtc_lastSyncAttempt = time(nullptr);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 20) { delay(500); wifiTimeout++; }

  if (WiFi.status() == WL_CONNECTED) {
    if (rtc_in_dino_game && rtc_game_auto) { rtc_in_dino_game = false; rtc_game_auto = false; }  

    configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov");
    int ntpRetry = 0;
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && ntpRetry < 20) { delay(250); ntpRetry++; }
    fetchLocationAndWeather();
  } else if (!rtc_in_dino_game) {
    rtc_in_dino_game = true;
    rtc_game_auto = true;
    rtc_dino_score = 0;
    rtc_cactus_x = 200;
    rtc_dino_y = 120;
    rtc_dino_jumping = false;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ==========================================
// 7. SIGNATURE HASHING
// ==========================================
uint32_t hashInt(uint32_t h, int32_t v) { h ^= (uint32_t)v; h *= 16777619u; return h; }

uint32_t viewSignature(struct tm &t) {
  uint32_t h = 2166136261u;
  if (rtc_in_dino_game) {
    h = hashInt(h, 99);
    h = hashInt(h, rtc_dino_score); h = hashInt(h, rtc_cactus_x); h = hashInt(h, rtc_dino_y);
    return h;
  }
  h = hashInt(h, currentScreen);
  switch (currentScreen) {
    case 0:
      h = hashInt(h, t.tm_hour); h = hashInt(h, t.tm_min); h = hashInt(h, t.tm_mday);
      h = hashInt(h, rtc_aqi); h = hashInt(h, (int)lroundf(rtc_pm25 * 10));
      h = hashInt(h, (int)lroundf(rtc_temp * 10)); h = hashInt(h, (int)lroundf(rtc_out_temp * 10));
      break;
    case 1:
      h = hashInt(h, rtc_ahtOk); h = hashInt(h, (int)lroundf(rtc_temp * 10)); h = hashInt(h, (int)lroundf(rtc_hum * 10));
      h = hashInt(h, rtc_ensFlag); h = hashInt(h, rtc_eco2); h = hashInt(h, rtc_aqi); h = hashInt(h, rtc_tvoc);                                  
      break;
    case 2:
      h = hashInt(h, rtc_haveAq); h = hashInt(h, (int)lroundf(rtc_pm25 * 10)); h = hashInt(h, (int)lroundf(rtc_pm10 * 10));
      h = hashInt(h, rtc_haveWeather); h = hashInt(h, rtc_weather_code); h = hashInt(h, (int)lroundf(rtc_out_temp * 10));
      break;
    case 3:
      h = hashInt(h, t.tm_year); h = hashInt(h, t.tm_mon); h = hashInt(h, t.tm_mday);
      break;
    case 4:
      h = hashInt(h, rtc_alarm_on); h = hashInt(h, rtc_alarm_h); h = hashInt(h, rtc_alarm_m); h = hashInt(h, rtc_alarm_edit_state);
      h = hashInt(h, rtc_alarm_is_ringing);
      break;
    case 5:
      for (const char* c = rtc_city; *c; c++) h = hashInt(h, *c);
      for (const char* c = rtc_region; *c; c++) h = hashInt(h, *c);
      for (const char* c = rtc_country; *c; c++) h = hashInt(h, *c);
      for (const char* c = rtc_ip; *c; c++) h = hashInt(h, *c);
      h = hashInt(h, (int)lroundf(rtc_lat * 1000)); h = hashInt(h, (int)lroundf(rtc_lon * 1000));
      h = hashInt(h, rtc_haveLoc); h = hashInt(h, (int32_t)rtc_locFetch); h = hashInt(h, t.tm_hour); h = hashInt(h, t.tm_min);
      break;
  }
  return h;
}

// ==========================================
// 8. DRAWING UI HELPERS
// ==========================================
// Width in pixels of a string in the current font
int textW(const char* str) {
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  return (int)w;
}
// Print str at (x,y); if wider than maxW, cut it and add ".."
void printFit(const char* str, int x, int y, int maxW) {
  char buf[48]; strlcpy(buf, str, sizeof(buf));
  if (textW(buf) > maxW) {
    int n = strlen(buf);
    while (n > 0) { buf[--n] = 0; char t[52]; snprintf(t, sizeof(t), "%s..", buf); if (textW(t) <= maxW) { strlcpy(buf, t, sizeof(buf)); break; } }
  }
  display.setCursor(x, y); display.print(buf);
}
// Print centred horizontally inside [x0, x0+w)
void printCentered(const char* str, int x0, int w, int y) {
  int tw = textW(str); int x = x0 + (w - tw) / 2; if (x < x0) x = x0;
  printFit(str, x, y, w);
}

void drawInvertedTopBar(const char* title, struct tm &t, bool isCalendar = false) {
  display.fillRect(0, 0, 200, 22, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setFont(&FreeSansBold9pt7b);
  
  if (isCalendar) {
    char mbuf[30]; strftime(mbuf, sizeof(mbuf), "%B %Y", &t);
    int len = strlen(mbuf);
    display.setCursor(100 - (len*5), 15); display.print(mbuf);
  } else {
    display.setCursor(5, 15); display.print(title);
    
    int t_h12 = t.tm_hour % 12; if (t_h12 == 0) t_h12 = 12;
    char tbuf[10]; snprintf(tbuf, sizeof(tbuf), "%02d:%02d", t_h12, t.tm_min);
    display.setCursor(140, 15); display.print(tbuf);
  }
  display.setTextColor(GxEPD_BLACK);
}

void drawBell(int x, int y) {
  display.fillRoundRect(x+2, y, 6, 6, 2, GxEPD_BLACK); 
  display.fillRect(x+1, y+5, 8, 4, GxEPD_BLACK);       
  display.fillRect(x, y+9, 10, 2, GxEPD_BLACK);        
  display.fillRect(x+4, y+11, 2, 2, GxEPD_BLACK);      
}

void drawWeatherIcon(int x, int y, int code) {
  if (code <= 1) { // Clear
    display.drawCircle(x+15, y+10, 8, GxEPD_BLACK);
    display.drawLine(x+15, y, x+15, y+4, GxEPD_BLACK); display.drawLine(x+15, y+16, x+15, y+20, GxEPD_BLACK);
    display.drawLine(x+5, y+10, x+9, y+10, GxEPD_BLACK); display.drawLine(x+21, y+10, x+25, y+10, GxEPD_BLACK);
    display.drawLine(x+8, y+3, x+10, y+5, GxEPD_BLACK); display.drawLine(x+20, y+15, x+22, y+17, GxEPD_BLACK);
    display.drawLine(x+22, y+3, x+20, y+5, GxEPD_BLACK); display.drawLine(x+10, y+15, x+8, y+17, GxEPD_BLACK);
  } else if (code <= 48) { // Cloud/Haze
    display.fillCircle(x+10, y+12, 6, GxEPD_BLACK); display.fillCircle(x+18, y+10, 8, GxEPD_BLACK); display.fillCircle(x+26, y+13, 5, GxEPD_BLACK);
    if(code >= 45) { display.drawFastHLine(x+5, y+22, 22, GxEPD_BLACK); display.drawFastHLine(x+7, y+25, 18, GxEPD_BLACK); }
  } else { // Rain/Storm
    display.fillCircle(x+10, y+8, 6, GxEPD_BLACK); display.fillCircle(x+18, y+6, 8, GxEPD_BLACK); display.fillCircle(x+26, y+9, 5, GxEPD_BLACK);
    display.drawLine(x+10, y+15, x+6, y+22, GxEPD_BLACK); display.drawLine(x+18, y+15, x+14, y+22, GxEPD_BLACK); display.drawLine(x+26, y+15, x+22, y+22, GxEPD_BLACK);
  }
}

// Dino drawing functions
void drawDino(int x, int y) {
  display.fillRect(x+12, y, 14, 10, GxEPD_BLACK); display.fillRect(x+14, y+2, 2, 2, GxEPD_WHITE); display.fillRect(x+12, y+10, 8, 4, GxEPD_BLACK); display.fillRect(x, y+14, 20, 12, GxEPD_BLACK); display.fillRect(x-4, y+16, 6, 4, GxEPD_BLACK); display.fillRect(x+20, y+16, 6, 2, GxEPD_BLACK); display.fillRect(x+4, y+26, 4, 6, GxEPD_BLACK); display.fillRect(x+12, y+26, 4, 6, GxEPD_BLACK);
}
void drawCactus(int x, int y) {
  display.fillRect(x+6, y, 6, 24, GxEPD_BLACK); display.fillRect(x, y+6, 4, 10, GxEPD_BLACK); display.fillRect(x+14, y+4, 4, 10, GxEPD_BLACK); display.fillRect(x+4, y+16, 2, 2, GxEPD_BLACK); display.fillRect(x+12, y+14, 2, 2, GxEPD_BLACK);
}

const char* rhLabel(float rh) {
  if (rh >= 99.5f) return "CHECK";     
  if (rh <  30.0f) return "DRY";
  if (rh <= 50.0f) return "IDEAL";
  if (rh <= 60.0f) return "OK";
  return "HIGH";
}

void drawScreen(int screen, struct tm &timeinfo) {
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setTextWrap(false); // GLOBALLY PREVENT TEXT WRAPPING OVERFLOWS

  // --- DINO GAME SCREEN ---
  if (rtc_in_dino_game) {
    display.setFont(&FreeSansBold9pt7b); display.setCursor(5, 18); display.print("Dino Run"); display.drawFastHLine(0, 25, 200, GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b); display.setCursor(60, 50); display.print("Score: "); display.print(rtc_dino_score);
    display.drawFastHLine(0, 150, 200, GxEPD_BLACK); drawDino(20, rtc_dino_y); drawCactus(rtc_cactus_x, 126);
    display.setFont(NULL); display.setTextSize(1); display.setCursor(15, 168); display.print("Short Press: Run Forward"); display.setCursor(15, 181); display.print("Long: Jump | Extra Long: Exit");
    return;
  }

  switch (screen) {
    case 0: { // 1. HOME
      char dateStr[30]; strftime(dateStr, sizeof(dateStr), "%a %d %b %Y", &timeinfo);
      display.setFont(&FreeSansBold9pt7b); display.setCursor(5, 15); display.print(dateStr);
      
      // 12-Hour Big Time & AM/PM
      int h12_home = timeinfo.tm_hour % 12; if (h12_home == 0) h12_home = 12;
      char tbuf[10]; snprintf(tbuf, sizeof(tbuf), "%02d:%02d", h12_home, timeinfo.tm_min);
      
      display.setFont(&FreeSansBold24pt7b); 
      display.setCursor(5, 58); // Shifted hard left to ensure perfect fit
      display.print(tbuf);
      
      display.setFont(&FreeSansBold12pt7b);
      display.setCursor(145, 58); // AM/PM securely placed on the right
      display.print(timeinfo.tm_hour >= 12 ? "PM" : "AM");
      
      display.drawFastHLine(0, 68, 200, GxEPD_BLACK);
      
      display.setFont(NULL); display.setTextSize(1);
      display.setCursor(5, 73); display.print("INDOOR");
      display.setCursor(105, 73); display.print("OUTDOOR");
      
      display.setFont(&FreeSansBold9pt7b);
      display.setCursor(5, 93); display.print("AQI "); display.print(rtc_aqi);
      display.setCursor(105, 93); display.print("AQI "); display.print(getUSAQI(rtc_pm25));
      
      display.setFont(NULL); display.setTextSize(1);
      display.setCursor(5, 102); display.print(getIndoorStatus(rtc_aqi));
      display.setCursor(105, 102); display.print(getAQIStatus(getUSAQI(rtc_pm25)));
      
      display.drawFastVLine(100, 68, 45, GxEPD_BLACK);
      display.drawFastHLine(0, 113, 200, GxEPD_BLACK);
      
      drawWeatherIcon(5, 118, rtc_weather_code);
      display.setFont(&FreeSansBold12pt7b);
      display.setCursor(45, 135); display.print((int)rtc_out_temp); display.print("C "); 
      display.setFont(&FreeSansBold9pt7b); display.print(getWeatherString(rtc_weather_code));
      
      display.setFont(NULL); display.setTextSize(1);
      display.setCursor(45, 145); display.print("In "); display.print(rtc_temp, 1); display.print("C  "); display.print((int)rtc_hum); display.print("%");
      
      display.drawFastHLine(0, 155, 200, GxEPD_BLACK);
      
      drawBell(8, 163);
      display.setFont(&FreeSansBold9pt7b); display.setCursor(25, 173); 
      int disp_h = rtc_alarm_h % 12; if (disp_h == 0) disp_h = 12;
      char abuf[16]; snprintf(abuf, sizeof(abuf), "%02d:%02d %s %s", disp_h, rtc_alarm_m, rtc_alarm_h >= 12 ? "PM" : "AM", rtc_alarm_on?"ON":"OFF");
      display.print(abuf);
      
      if (getUSAQI(rtc_pm25) > 100 || rtc_aqi > 3) {
        display.setCursor(150, 173); display.print("! AIR");
      }
      
      display.setFont(NULL); display.setTextSize(1); 
      display.setCursor(85, 188); display.print("Home");
      break;
    }
    
    case 1: { // 2. INDOOR AIR 
      drawInvertedTopBar("INDOOR AIR", timeinfo);
      
      display.setFont(NULL); display.setTextSize(1); display.setCursor(70, 35); display.print("AQI (UBA 1-5)");
      display.setFont(&FreeSansBold24pt7b); display.setCursor(10, 68); display.print(rtc_aqi);
      display.setFont(&FreeSansBold12pt7b); display.setCursor(55, 63); display.print(getIndoorStatus(rtc_aqi));
      
      for (int i=0; i<5; i++) {
        if (i < rtc_aqi) display.fillRect(10 + (i*32), 75, 28, 6, GxEPD_BLACK);
        else display.drawRect(10 + (i*32), 75, 28, 6, GxEPD_BLACK);
      }
      display.drawFastHLine(0, 90, 200, GxEPD_BLACK);
      
      display.setFont(&FreeSans9pt7b);
      display.setCursor(10, 109); display.print("eCO2");  display.setFont(&FreeSansBold12pt7b); display.setCursor(95, 109); display.print(rtc_eco2); display.print(" ppm"); display.setFont(&FreeSans9pt7b);
      display.setCursor(10, 129); display.print("TVOC");  display.setFont(&FreeSansBold12pt7b); display.setCursor(105, 129); display.print(rtc_tvoc); display.print(" ppb"); display.setFont(&FreeSans9pt7b);
      display.setCursor(10, 149); display.print("Temperature");  display.setFont(&FreeSansBold12pt7b); display.setCursor(130, 149); display.print(rtc_temp, 1); display.print(" C"); display.setFont(&FreeSans9pt7b);
      display.setCursor(10, 169); display.print("Humidity");   display.setFont(&FreeSansBold12pt7b); display.setCursor(140, 169); display.print((int)rtc_hum); display.print(" %");
      
      display.setFont(&FreeSansBold9pt7b);
      display.drawFastHLine(0, 176, 200, GxEPD_BLACK);
      printCentered(rtc_aqi <= 2 ? "Air is fine" : "Ventilate Room", 0, 200, 194);
      break;
    }

    case 2: { // 3. OUTDOOR & WEATHER
      drawInvertedTopBar("OUTDOOR", timeinfo);
      int us_aqi = getUSAQI(rtc_pm25);
      
      display.setFont(NULL); display.setTextSize(1); display.setCursor(110, 35); display.print("US AQI");
      display.setFont(&FreeSansBold24pt7b); display.setCursor(5, 68); display.print(us_aqi);
      
      display.setFont(&FreeSansBold9pt7b); display.setCursor(80, 60); display.print(getAQIStatus(us_aqi));
      
      display.setFont(NULL); display.setTextSize(1); display.setCursor(5, 75); display.print("PM2.5 "); display.print((int)rtc_pm25); display.setCursor(70, 75); display.print("PM10 "); display.print((int)rtc_pm10);
      display.drawFastHLine(0, 85, 200, GxEPD_BLACK);
      
      drawWeatherIcon(5, 95, rtc_weather_code);
      display.setFont(&FreeSansBold24pt7b); display.setCursor(65, 125); display.print((int)rtc_out_temp); display.print(" C"); 
      display.setFont(&FreeSansBold12pt7b); display.setCursor(65, 145); display.print(getWeatherString(rtc_weather_code));
      
      display.drawFastHLine(0, 155, 200, GxEPD_BLACK);
      
      display.setFont(NULL); display.setTextSize(1);
      display.setCursor(5, 165); display.print("Feels "); display.print((int)rtc_out_feels); display.print("C");
      display.setCursor(110, 165); display.print("RH "); display.print((int)rtc_out_hum); display.print("%");
      
      display.setCursor(5, 185); display.print(rtc_city); display.print("   wind "); display.print(rtc_out_wind, 1); display.print(" m/s");
      
      break;
    }

    case 3: { // 4. CALENDAR
      drawInvertedTopBar("", timeinfo, true);
      
      display.setFont(&FreeSansBold9pt7b);
      for (int i = 0; i < 7; i++) { display.setCursor(10 + (i * 27), 45); display.print(dayLetters[i]); }
      display.drawFastHLine(0, 52, 200, GxEPD_BLACK);
      
      display.setFont(NULL); display.setTextSize(1);
      int cYear = timeinfo.tm_year + 1900; int cMon = timeinfo.tm_mon; int tDate = timeinfo.tm_mday;
      int startDay = (timeinfo.tm_wday - ((tDate - 1) % 7) + 7) % 7;
      int totDays  = getDaysInMonth(cMon, cYear);
      
      for (int day = 1; day <= totDays; day++) {
        int cell = (day - 1) + startDay; int cx = 10 + ((cell % 7) * 27); int cy = 65 + ((cell / 7) * 20);
        if (day == tDate) {
          display.fillRect(cx - 4, cy - 3, 18, 14, GxEPD_BLACK);
          display.setTextColor(GxEPD_WHITE); display.setCursor(day < 10 ? cx + 2 : cx - 1, cy); display.print(day);
          display.setTextColor(GxEPD_BLACK);
        } else {
          display.setCursor(day < 10 ? cx + 2 : cx - 1, cy); display.print(day);
        }
      }
      
      break;
    }

    case 4: { // 5. ALARM SETTINGS
      drawInvertedTopBar("ALARM", timeinfo);
      
      int disp_h = rtc_alarm_h % 12; if (disp_h == 0) disp_h = 12;
      const char* am_pm = (rtc_alarm_h >= 12) ? "PM" : "AM";
      
      if (rtc_alarm_is_ringing) {
        display.setFont(&FreeSansBold12pt7b); 
        display.setCursor(45, 60); 
        display.print("WAKE UP!");
      }
      
      display.setFont(&FreeSansBold24pt7b); 
      
      if (rtc_alarm_edit_state == 2) { display.setTextColor(GxEPD_WHITE); display.fillRect(5, 75, 65, 50, GxEPD_BLACK); }
      display.setCursor(10, 115); if (disp_h < 10) display.print("0"); display.print(disp_h); display.setTextColor(GxEPD_BLACK);
      
      display.setCursor(75, 110); display.print(":");
      
      if (rtc_alarm_edit_state == 3) { display.setTextColor(GxEPD_WHITE); display.fillRect(90, 75, 65, 50, GxEPD_BLACK); }
      display.setCursor(95, 115); if (rtc_alarm_m < 10) display.print("0"); display.print(rtc_alarm_m); display.setTextColor(GxEPD_BLACK);

      display.setFont(&FreeSansBold12pt7b);
      if (rtc_alarm_edit_state == 4) { display.setTextColor(GxEPD_WHITE); display.fillRect(158, 90, 40, 25, GxEPD_BLACK); }
      display.setCursor(160, 110); display.print(am_pm); display.setTextColor(GxEPD_BLACK);

      display.setFont(&FreeSansBold18pt7b);
      if (rtc_alarm_edit_state == 1) { display.setTextColor(GxEPD_WHITE); display.fillRect(65, 135, 70, 35, GxEPD_BLACK); }
      display.setCursor(rtc_alarm_on ? 75 : 65, 162); display.print(rtc_alarm_on ? "ON" : "OFF"); display.setTextColor(GxEPD_BLACK);
      
      drawBell(160, 145);

      display.setFont(NULL); display.setTextSize(1);
      if (rtc_alarm_edit_state == 0) { 
        printCentered("Hold: edit   Tap: next screen", 0, 200, 184); 
      } 
      else { 
        printCentered("Tap: change   Hold: next field", 0, 200, 184); 
      }
      break;
    }
    
    case 5: { // 6. LOCATION  (everything on one 200x200 screen)
      drawInvertedTopBar("LOCATION", timeinfo);

      // --- City: biggest font that fits 190 px ---
      const GFXfont* cityFonts[] = { &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b };
      for (int i = 0; i < 3; i++) { display.setFont(cityFonts[i]); if (textW(rtc_city) <= 190 || i == 2) break; }
      printFit(rtc_city, 5, 52, 190);

      // --- Region / division ---
      display.setFont(&FreeSans9pt7b);
      printFit(rtc_region[0] ? rtc_region : "-", 5, 72, 190);

      // --- Country ---
      display.setFont(&FreeSansBold9pt7b);
      printFit(rtc_country, 5, 92, 190);

      // --- Coordinates box ---
      display.drawFastHLine(0, 99, 200, GxEPD_BLACK);
      display.drawFastVLine(100, 99, 32, GxEPD_BLACK);
      display.setFont(NULL); display.setTextSize(1);
      display.setCursor(5, 103);   display.print("LATITUDE");
      display.setCursor(105, 103); display.print("LONGITUDE");
      char cbuf[20];
      display.setFont(&FreeSansBold9pt7b);
      snprintf(cbuf, sizeof(cbuf), "%.4f %c", fabs(rtc_lat), rtc_lat >= 0 ? 'N' : 'S'); printFit(cbuf, 5, 126, 92);
      snprintf(cbuf, sizeof(cbuf), "%.4f %c", fabs(rtc_lon), rtc_lon >= 0 ? 'E' : 'W'); printFit(cbuf, 105, 126, 92);
      display.drawFastHLine(0, 131, 200, GxEPD_BLACK);

      // --- Time zone, IP, ISP (small font, 10 px rows) ---
      display.setFont(NULL); display.setTextSize(1);
      char line[48];
      int offH = rtc_utcOffset / 3600, offM = abs(rtc_utcOffset % 3600) / 60;
      snprintf(line, sizeof(line), "TZ  %s (UTC%+d:%02d)", rtc_tz[0] ? rtc_tz : "-", offH, offM);
      printFit(line, 5, 136, 190);
      snprintf(line, sizeof(line), "IP  %s", rtc_ip[0] ? rtc_ip : "-");
      printFit(line, 5, 147, 190);
      snprintf(line, sizeof(line), "ISP %s", rtc_isp[0] ? rtc_isp : "-");
      printFit(line, 5, 158, 190);

      // --- Status footer ---
      display.drawFastHLine(0, 170, 200, GxEPD_BLACK);
      if (rtc_haveLoc && rtc_locFetch > 1700000000) {
        struct tm lt; localtime_r(&rtc_locFetch, &lt);
        int h12 = lt.tm_hour % 12; if (h12 == 0) h12 = 12;
        snprintf(line, sizeof(line), "Online - updated %d:%02d %s", h12, lt.tm_min, lt.tm_hour >= 12 ? "PM" : "AM");
      } else if (rtc_haveLoc) {
        snprintf(line, sizeof(line), "Online - via IP lookup");
      } else {
        snprintf(line, sizeof(line), "Offline - default city");
      }
      printCentered(line, 0, 200, 175);
      printCentered("2x tap: re-locate   6/6", 0, 200, 188);
      break;
    }
  }
}

// ==========================================
// 9. SETUP & LOGIC
// ==========================================
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  setenv("TZ", "UTC-6", 1); tzset();

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool isButtonWake = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);

  // --- BUTTON PRESS DETECTION ---
  bool isShortPress = false;
  bool isLongPress = false;
  bool isExtraLongPress = false;
  bool isDoubleClick = false;
  bool isTripleClick = false;
  
  if (isButtonWake) {
    unsigned long pressStart = millis();
    while (digitalRead(BUTTON_PIN) == LOW && (millis() - pressStart < 2600)) { delay(10); }
    unsigned long duration = millis() - pressStart;
    
    if (duration > 2000) { isExtraLongPress = true; isLongPress = true; } 
    else if (duration > 600) { isLongPress = true; } 
    else {
      if (rtc_in_dino_game) {
        isShortPress = true;
      } else {
        int clicks = 1;
        unsigned long waitStart = millis();
        // Wait up to 300ms for a follow-up click
        while(millis() - waitStart < 300) {
          if (digitalRead(BUTTON_PIN) == LOW) {
            clicks++;
            while (digitalRead(BUTTON_PIN) == LOW) delay(10); 
            waitStart = millis(); 
            if (clicks == 3) break;
          }
          delay(10);
        }
        
        if (clicks == 3) isTripleClick = true;
        else if (clicks == 2) isDoubleClick = true;
        else isShortPress = true;
      }
    }
  }

  struct tm timeinfo;
  bool timeOk = getLocalTime(&timeinfo, 100);
  time_t now = time(nullptr);
  bool wifiUpdated = false;

  // --- NETWORK SYNC ---
  if (!isButtonWake) {
    bool syncDue = (rtc_lastSyncAttempt == 0) || ((now - rtc_lastSyncAttempt) >= (time_t)(SYNC_INTERVAL_SEC - 5));
    if (syncDue) {
      updateFromNetwork();
      timeOk = getLocalTime(&timeinfo, 100);    
      now = time(nullptr);
      wifiUpdated = true;         
    }
  }

  // --- Double-click on the Location screen = look up location again now ---
  if (isButtonWake && isDoubleClick && currentScreen == 5 && !rtc_in_dino_game) {
    rtc_locForce = true;
    updateFromNetwork();
    timeOk = getLocalTime(&timeinfo, 100);
    now = time(nullptr);
    wifiUpdated = true;
  }

  // --- ALARM CHECK ---
  bool alarmHandled = false;
  if (alarmIsDue(timeinfo, timeOk, now) && currentScreen != 4) {
    currentScreen = 4;                       
    display.init(115200, !rtc_displayReady, 2, false);   
    display.setRotation(1); display.setTextColor(GxEPD_BLACK);
    display.setFullWindow(); 
    rtc_alarm_is_ringing = true;
    display.firstPage(); do { drawScreen(currentScreen, timeinfo); } while (display.nextPage());
    
    ringAlarm();          
    
    rtc_alarm_last_ring = time(nullptr);
    alarmHandled = true;
    rtc_in_dino_game = false; rtc_game_auto = false;
    currentScreen = 0;                       
  }

  bool userAction = isButtonWake && !alarmHandled;
  bool forceFullRefresh = isDoubleClick;

  // ====================================================
  // STATE MACHINE LOGIC
  // ====================================================
  if (userAction) {
    if (rtc_in_dino_game) {
      if (isExtraLongPress) {
        rtc_in_dino_game = false; rtc_game_auto = false; currentScreen = 0;
      } else {
        rtc_cactus_x -= 25;
        if (isLongPress) { rtc_dino_y = 80; rtc_dino_jumping = true; } 
        else { rtc_dino_y = 120; rtc_dino_jumping = false; } 
        if (rtc_cactus_x < 40 && rtc_cactus_x > -2 && !rtc_dino_jumping) {
          rtc_dino_score = 0; rtc_cactus_x = 200; 
        } 
        else if (rtc_cactus_x < -20) {
          rtc_dino_score++;
          rtc_cactus_x = 200 + ((rtc_dino_score * 47) % 100);
        }
      }
    }
    else if (currentScreen == 4 && !isDoubleClick) {
      if (isTripleClick) {
        rtc_alarm_edit_state = 0;
        currentScreen = (currentScreen + 1) % NUM_SCREENS;   // BUG FIX: was 0 (Home) -> Location never shown
      } else if (isLongPress) {
        rtc_alarm_edit_state = (rtc_alarm_edit_state + 1) % 5;
      } else if (isShortPress) {
        if (rtc_alarm_edit_state == 1) { rtc_alarm_on = !rtc_alarm_on; }
        else if (rtc_alarm_edit_state == 2) {
          int h12 = rtc_alarm_h % 12; if (h12 == 0) h12 = 12;
          bool is_pm = (rtc_alarm_h >= 12);
          h12 = (h12 % 12) + 1;                                      
          rtc_alarm_h = (h12 == 12 ? 0 : h12) + (is_pm ? 12 : 0);    
        }
        else if (rtc_alarm_edit_state == 3) { rtc_alarm_m = (rtc_alarm_m + 1) % 60; }
        else if (rtc_alarm_edit_state == 4) { rtc_alarm_h = (rtc_alarm_h + 12) % 24; }
        else { currentScreen = (currentScreen + 1) % NUM_SCREENS; }   // BUG FIX: was 0 -> Location (5) unreachable
        if (rtc_alarm_edit_state > 0) rearmAlarm(timeinfo, timeOk, now);
      }
    }
    else if (!isDoubleClick) {
      if (isLongPress && currentScreen == 0) {
        rtc_in_dino_game = true; rtc_game_auto = false;
        rtc_dino_score = 0; rtc_cactus_x = 200; rtc_dino_y = 120; rtc_dino_jumping = false;
      } else if (isShortPress || isTripleClick) {
        currentScreen = (currentScreen + 1) % NUM_SCREENS;
      }
    }
  }

  // ====================================================
  // REFRESH LOGIC 
  // ====================================================
  int  viewId      = rtc_in_dino_game ? 99 : currentScreen;
  bool viewChanged = (viewId != rtc_lastView);

  if (viewId == 0 && (!userAction || viewChanged)) readIndoorSensors();

  bool firstDraw  = !rtc_displayReady;
  bool nightClean = false;
  int  dayKey = timeinfo.tm_year * 400 + timeinfo.tm_yday;
  if (NIGHT_CLEAN_HOUR >= 0 && !userAction && timeOk && timeinfo.tm_hour == NIGHT_CLEAN_HOUR && rtc_lastCleanKey != dayKey) {
    nightClean = true;
  }

  uint32_t sig = viewSignature(timeinfo);
  
  bool needDraw = firstDraw || nightClean || viewChanged || (sig != rtc_lastSig) || forceFullRefresh || wifiUpdated;

  bool fullRefresh = firstDraw || nightClean || forceFullRefresh || wifiUpdated;
  
  if (needDraw && viewChanged && viewId == 0) fullRefresh = true;   

  if (needDraw) {
    display.init(115200, !rtc_displayReady, 2, false);   
    display.setRotation(1);
    display.setTextColor(GxEPD_BLACK);

    if (fullRefresh)      display.setFullWindow(); 
    else if (viewChanged) display.setPartialWindow(0, 0, display.width(), display.height());               
    else                  display.setPartialWindow(0, PARTIAL_TOP, display.width(), display.height() - PARTIAL_TOP); 

    display.firstPage();
    do { drawScreen(currentScreen, timeinfo); } while (display.nextPage());
    display.powerOff();

    if (nightClean && timeOk) rtc_lastCleanKey = dayKey;
    rtc_displayReady = true;
    rtc_lastView = viewId;
    rtc_lastSig  = sig;
  }

  if (!ENS160_ALWAYS_ON && ensTouched) { myENS.setOperatingMode(SFE_ENS160_DEEP_SLEEP); }

  // --- SLEEP ---
  unsigned long tRel = millis();
  while (digitalRead(BUTTON_PIN) == LOW && millis() - tRel < 3000) delay(10);
  delay(30);

  esp_sleep_enable_ext0_wakeup(GPIO_NUM_27, 0);
  rtc_gpio_pullup_en(GPIO_NUM_27);
  rtc_gpio_pulldown_dis(GPIO_NUM_27);

  time_t nowS = time(nullptr);
  uint32_t sleepSec = 60;
  if (nowS > 1700000000) sleepSec = 60 - (uint32_t)(nowS % 60) + WAKE_OFFSET_SEC;
  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);

  esp_deep_sleep_start();
}

void loop() { }