#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <WiFi.h>
#include <esp_now.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <FS.h>
#include <HomeSpan.h>

// ============================================================================
// Pin maps — defaults match the CrowPanel 5.79" ESP32-S3 E-Paper board.
// ============================================================================
// Display SPI Pins
#define EPD_CS 45
#define EPD_DC 46
#define EPD_RST 47
#define EPD_BUSY 48
#define EPD_SCK 12
#define EPD_MOSI 11
#define EPD_PWR 7

// microSD SPI Pins (Independent HSPI bus)
#define SD_CS 39
#define SD_SCK 40
#define SD_MOSI 41
#define SD_MISO 38

// Driver class for the 5.79" / 272 x 792 B/W panel.
GxEPD2_BW<GxEPD2_579_GDEY0579T93, GxEPD2_579_GDEY0579T93::HEIGHT> display(
    GxEPD2_579_GDEY0579T93(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// Initialize independent SPI bus for the microSD Card
SPIClass sdSPI(HSPI);

// ============================================================================
// State structures
// ============================================================================
struct DashboardState
{
  int currentGallons;
  int capacityGallons;
  int percentage;
  int dailyUse;
  int daysRemaining;
  float change12h;
  float sensorDistanceCm;
  int batteryLevel;
  String status;         // "RISING" / "FALLING" / "STABLE"
  String updatedAt;      // pre-formatted clock time (e.g., "10:42 AM")
  int trend[7];          // 0-100, oldest -> newest
  int rain[7];           // 0-100, rain height multiplier (mm * 10)
  uint8_t weather[7];    // 0=sun, 1=cloud-outline, 2=rain, 3=partly-cloudy
};

// Global dashboard state (initialized with placeholders, updated in real-time)
DashboardState currentState = {
    /* current        */ 0,
    /* capacity       */ 1600, // Calibrated capacity
    /* percent        */ 0,
    /* dailyUse       */ 45,   // Estimated average daily use
    /* daysRemaining  */ 0,
    /* change12h      */ 0.0f,
    /* sensorDistance */ 153.0f, // Empty by default
    /* battery        */ 100,
    /* status         */ "STABLE",
    /* updatedAt      */ "Waiting...",
    /* trend          */ {0, 0, 0, 0, 0, 0, 0},
    /* rain           */ {0, 0, 0, 0, 0, 0, 0},
    /* weather        */ {0, 0, 0, 0, 0, 0, 0}, // Sun defaults
};

// C++ Struct matching the specified microSD CSV schema
struct DataRecord {
  uint32_t measured_on;          // UNIX Epoch timestamp
  float water_surface_dist_cm;   // raw distance
  uint32_t estimated_gallons;    // whole gallons
  uint8_t cistern_percentage;    // whole percentage
  uint8_t battery_percentage;    // whole battery percentage
  float temp_c;                  // temperature
  uint16_t rain_tips;            // cumulative storm tips
};

// ============================================================================
// HomeSpan HomeKit characteristics pointers
// ============================================================================
SpanCharacteristic *hkPercentageSensor = nullptr;
SpanCharacteristic *hkTempSensor = nullptr;
SpanCharacteristic *hkBatteryLevel = nullptr;
SpanCharacteristic *hkLowBattery = nullptr;

struct CisternHomeKitDevice : SpanAccessory {
  CisternHomeKitDevice() : SpanAccessory() {
    new Service::AccessoryInformation();
    new Characteristic::Identify();
    new Characteristic::Manufacturer("DIY");
    new Characteristic::Model("ESP32 Cistern Monitor");
    new Characteristic::SerialNumber("CISTERN-01");
    new Characteristic::Name("Cistern Monitor");
    
    // We expose water level percentage (0-100%) as a Relative Humidity sensor
    new Service::HumiditySensor();
    hkPercentageSensor = new Characteristic::CurrentRelativeHumidity(0);
    new Characteristic::Name("Water Level");
    
    // Cistern temperature sensor (DS18B20 telemetry)
    new Service::TemperatureSensor();
    hkTempSensor = new Characteristic::CurrentTemperature(0.0);
    new Characteristic::Name("Cistern Temperature");
    
    // Battery Status
    new Service::BatteryService();
    hkBatteryLevel = new Characteristic::BatteryLevel(100);
    new Characteristic::ChargingState(2); // Not charging / solar buffered
    hkLowBattery = new Characteristic::StatusLowBattery(0); // 0 = normal, 1 = low
  }
};

// ============================================================================
// microSD Logging & History Management
// ============================================================================
void logDataToSD(const DataRecord &record) {
  File file = SD.open("/cistern_history.csv", FILE_APPEND);
  if (!file) {
    // If append fails, attempt to create a new file with headers
    file = SD.open("/cistern_history.csv", FILE_WRITE);
    if (file) {
      file.println("measured_on,water_surface_dist_cm,estimated_gallons,cistern_percentage,battery_percentage,temp_c,rain_tips");
    }
  }
  
  if (file) {
    file.print(record.measured_on);
    file.print(",");
    file.print(record.water_surface_dist_cm, 1);
    file.print(",");
    file.print(record.estimated_gallons);
    file.print(",");
    file.print(record.cistern_percentage);
    file.print(",");
    file.print(record.battery_percentage);
    file.print(",");
    file.print(record.temp_c, 1);
    file.print(",");
    file.println(record.rain_tips);
    file.close();
    Serial.println("Data successfully written to microSD CSV.");
  } else {
    Serial.println("CRITICAL: Failed to write data to microSD card!");
  }
}

// Parses the CSV file to load the last 7 days of midnight aggregates
void loadTrendDataFromSD() {
  File file = SD.open("/cistern_history.csv", FILE_READ);
  if (!file) {
    Serial.println("No history file found on microSD, keeping placeholders.");
    return;
  }
  
  // Calculate midnight offsets for the last 7 days
  time_t nowTime = time(nullptr);
  struct tm nowInfo;
  if (!localtime_r(&nowTime, &nowInfo)) {
    Serial.println("Unable to fetch local time for trend logic.");
    file.close();
    return;
  }
  
  struct tm tempTm = nowInfo;
  tempTm.tm_hour = 0;
  tempTm.tm_min = 0;
  tempTm.tm_sec = 0;
  time_t todayMidnight = mktime(&tempTm);
  
  time_t midnights[7];
  for (int i = 0; i < 7; i++) {
    midnights[i] = todayMidnight - (6 - i) * 24 * 3600;
  }
  
  // Skip the CSV header line
  if (file.available()) {
    file.readStringUntil('\n');
  }
  
  int latestPercent[7] = {0};
  float dailyRainTips[7] = {0.0f};
  bool dayHasData[7] = {false};
  
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() == 0) continue;
    
    uint32_t timestamp = 0;
    float dist = 0.0f;
    uint32_t gallons = 0;
    int percent = 0;
    int battery = 0;
    float temp = 0.0f;
    int tips = 0;
    
    // Parse the row
    int parsed = sscanf(line.c_str(), "%u,%f,%u,%d,%d,%f,%d", &timestamp, &dist, &gallons, &percent, &battery, &temp, &tips);
    if (parsed >= 7) {
      for (int i = 0; i < 7; i++) {
        time_t dayStart = midnights[i];
        time_t dayEnd = dayStart + 24 * 3600;
        if (timestamp >= dayStart && timestamp < dayEnd) {
          latestPercent[i] = percent;
          dailyRainTips[i] += tips;
          dayHasData[i] = true;
          break;
        }
      }
    }
  }
  file.close();
  
  // Populate our dashboard arrays
  for (int i = 0; i < 7; i++) {
    if (dayHasData[i]) {
      currentState.trend[i] = latestPercent[i];
      // Store rainfall in mm * 10 (e.g. 0.2794mm per tip)
      currentState.rain[i] = (int)(dailyRainTips[i] * 0.2794f * 10.0f);
      
      // Basic weather rule: if it rained > 1mm, show a rain cloud, otherwise sun/clouds
      float rainMm = dailyRainTips[i] * 0.2794f;
      if (rainMm > 2.0f) {
        currentState.weather[i] = 2; // Rain cloud
      } else if (rainMm > 0.1f) {
        currentState.weather[i] = 3; // Partly cloudy
      } else {
        currentState.weather[i] = 0; // Sun
      }
    }
  }
  Serial.println("Trend data reloaded successfully.");
}

// ============================================================================
// Drawing helpers
// ============================================================================
static void drawCenteredText(const char *s, int16_t x0, int16_t x1, int16_t y)
{
  int16_t bx, by;
  uint16_t bw, bh;
  display.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  int16_t cx = x0 + (x1 - x0 - (int16_t)bw) / 2 - bx;
  display.setCursor(cx, y);
  display.print(s);
}

static void drawSun(int cx, int cy)
{
  display.drawCircle(cx, cy, 5, GxEPD_BLACK);
  for (int k = 0; k < 8; k++)
  {
    float a = k * (PI / 4.0f);
    int x1 = cx + (int)(7 * cos(a));
    int y1 = cy + (int)(7 * sin(a));
    int x2 = cx + (int)(11 * cos(a));
    int y2 = cy + (int)(11 * sin(a));
    display.drawLine(x1, y1, x2, y2, GxEPD_BLACK);
  }
}

static void drawCloudOutline(int cx, int cy)
{
  display.drawCircle(cx - 6, cy + 1, 5, GxEPD_BLACK);
  display.drawCircle(cx + 1, cy - 3, 7, GxEPD_BLACK);
  display.drawCircle(cx + 9, cy + 1, 5, GxEPD_BLACK);
  display.drawLine(cx - 10, cy + 6, cx + 12, cy + 6, GxEPD_BLACK);
  display.fillRect(cx - 8, cy - 2, 18, 8, GxEPD_WHITE);
  display.drawLine(cx - 10, cy + 6, cx + 12, cy + 6, GxEPD_BLACK);
  display.drawCircle(cx - 6, cy + 1, 5, GxEPD_BLACK);
  display.drawCircle(cx + 1, cy - 3, 7, GxEPD_BLACK);
  display.drawCircle(cx + 9, cy + 1, 5, GxEPD_BLACK);
}

static void drawCloudFilled(int cx, int cy)
{
  display.fillCircle(cx - 6, cy + 1, 5, GxEPD_BLACK);
  display.fillCircle(cx + 1, cy - 3, 7, GxEPD_BLACK);
  display.fillCircle(cx + 9, cy + 1, 5, GxEPD_BLACK);
  display.fillRect(cx - 8, cy + 1, 19, 6, GxEPD_BLACK);
}

static void drawRainCloud(int cx, int cy)
{
  drawCloudFilled(cx, cy);
  for (int i = -1; i <= 1; i++)
  {
    int rx = cx + i * 5;
    display.drawLine(rx, cy + 9, rx - 2, cy + 14, GxEPD_BLACK);
    display.drawLine(rx, cy + 11, rx - 2, cy + 15, GxEPD_BLACK);
  }
}

static void drawWeatherIcon(uint8_t code, int cx, int cy)
{
  switch (code)
  {
  case 0:
    drawSun(cx, cy);
    break;
  case 1:
    drawCloudOutline(cx, cy);
    break;
  case 2:
    drawRainCloud(cx, cy);
    break;
  case 3:
    drawSun(cx - 5, cy - 3);
    drawCloudOutline(cx + 3, cy + 3);
    break;
  }
}

// ============================================================================
// drawDashboard — Renders E-Ink layout based on currentState
// ============================================================================
void drawDashboard(const DashboardState &s)
{
  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    // ---------- LEFT PANEL: Tank Shape (x = 0 .. 250) ----------
    const int tankX = 60, tankY = 22;
    const int tankW = 130, tankH = 220;

    // Small fitting nubs on top/bottom
    display.fillRect(tankX + tankW / 2 - 9, tankY - 7, 18, 7, GxEPD_BLACK);
    display.fillRect(tankX + tankW / 2 - 9, tankY + tankH, 18, 7, GxEPD_BLACK);

    // Outer tank round borders
    display.drawRoundRect(tankX, tankY, tankW, tankH, 14, GxEPD_BLACK);
    display.drawRoundRect(tankX + 1, tankY + 1, tankW - 2, tankH - 2, 13, GxEPD_BLACK);

    // Water level rectangle
    const int fillH = (s.percentage * (tankH - 6)) / 100;
    const int waterY = tankY + tankH - 3 - fillH;
    display.fillRect(tankX + 3, waterY, tankW - 6, fillH, GxEPD_BLACK);

    // Percentage tick marks
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    const int tickX = tankX + tankW + 6;
    for (int i = 0; i <= 4; i++)
    {
      int ty = tankY + tankH - (i * tankH / 4);
      display.drawLine(tickX, ty, tickX + 7, ty, GxEPD_BLACK);
      char buf[6];
      snprintf(buf, sizeof(buf), "%d", i * 25);
      display.setCursor(tickX + 11, ty + 4);
      display.print(buf);
    }

    // Centered percentage number (inverts color if covered by water)
    const int numberCenterY = tankY + tankH / 2 + 8;
    const uint16_t numColor = (numberCenterY >= waterY) ? GxEPD_WHITE : GxEPD_BLACK;
    display.setTextColor(numColor);
    display.setFont(&FreeSansBold24pt7b);
    char pct[4];
    snprintf(pct, sizeof(pct), "%d", s.percentage);
    drawCenteredText(pct, tankX, tankX + tankW, numberCenterY);

    display.setFont(&FreeSansBold9pt7b);
    drawCenteredText("PERCENT", tankX, tankX + tankW, numberCenterY + 26);
    display.setTextColor(GxEPD_BLACK);

    // ---------- DIVIDER ----------
    display.drawLine(255, 12, 255, 260, GxEPD_BLACK);

    // ---------- CENTER COLUMN: Stats Block (x = 260 .. 470) ----------
    const int cX = 270;
    int y = 26;

    display.setFont(&FreeSans9pt7b);
    display.setCursor(cX, y);
    display.print("STATUS");

    y += 24;
    // Status triangle indicator
    if (s.status == "RISING") {
      display.fillTriangle(cX, y, cX + 14, y, cX + 7, y - 14, GxEPD_BLACK);
    } else if (s.status == "FALLING") {
      display.fillTriangle(cX, y - 14, cX + 14, y - 14, cX + 7, y, GxEPD_BLACK);
    } else {
      display.fillRect(cX, y - 9, 14, 4, GxEPD_BLACK); // Stable bar
    }
    
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(cX + 22, y);
    display.print(s.status);

    y += 18;
    display.setFont(&FreeSans9pt7b);
    display.setCursor(cX, y);
    display.print(s.change12h >= 0 ? "+" : "");
    display.print(s.change12h, 1);
    display.print("% last 12h");

    y += 6;
    display.drawLine(cX, y, cX + 180, y, GxEPD_BLACK);

    auto block = [&](const char *label, const String &value, bool big)
    {
      y += 14;
      display.setFont(&FreeSans9pt7b);
      display.setCursor(cX, y);
      display.print(label);
      y += big ? 22 : 18;
      display.setFont(big ? &FreeSansBold12pt7b : &FreeSansBold9pt7b);
      display.setCursor(cX, y);
      display.print(value);
    };

    block("CAPACITY", String(s.capacityGallons) + " gal", false);
    block("CURRENT", String(s.currentGallons) + " gal", false);
    block("DAILY USE", "~" + String(s.dailyUse) + " gal/day", false);
    block("DAYS REMAINING", s.daysRemaining > 0 ? "~" + String(s.daysRemaining) + " days" : "Empty", true);

    // ---------- DIVIDER ----------
    display.drawLine(475, 12, 475, 260, GxEPD_BLACK);

    // ---------- RIGHT PANEL: Weather Row + 7-Day Chart (x = 480 .. 792) ----------
    const int rX0 = 488, rX1 = 786;
    const int rW = rX1 - rX0;

    // Weather forecast titles
    const char *days[7] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
    
    // We calculate which weekday names to draw based on current time
    time_t nowTime = time(nullptr);
    struct tm nowInfo;
    int currentDayOfWeek = 0; // Mon=0, Sun=6
    if (localtime_r(&nowTime, &nowInfo)) {
      currentDayOfWeek = (nowInfo.tm_wday + 6) % 7; // Convert tm_wday (Sun=0, Mon=1) to Mon=0, Sun=6
    }
    
    for (int i = 0; i < 7; i++)
    {
      int slotW = rW / 7;
      int ix = rX0 + i * slotW + slotW / 2;
      drawWeatherIcon(s.weather[i], ix, 24);
      display.setFont(&FreeSans9pt7b);
      int16_t bx, by;
      uint16_t bw, bh;
      
      // Draw weekday names rolling backwards from today (Index 6 is today)
      int dayIndex = (currentDayOfWeek - (6 - i) + 7) % 7;
      display.getTextBounds(days[dayIndex], 0, 0, &bx, &by, &bw, &bh);
      display.setCursor(ix - bw / 2, 58);
      display.print(days[dayIndex]);
    }

    // Chart frame
    const int gX0 = rX0 + 22;
    const int gX1 = rX1 - 4;
    const int gY0 = 78;
    const int gY1 = 220;
    const int gH = gY1 - gY0;

    // Y-Axis tick dots
    display.setFont(&FreeSans9pt7b);
    for (int i = 0; i <= 4; i++)
    {
      int gy = gY1 - (i * gH / 4);
      for (int dx = gX0; dx <= gX1; dx += 4)
        display.drawPixel(dx, gy, GxEPD_BLACK);
      char buf[6];
      snprintf(buf, sizeof(buf), "%d", i * 25);
      display.setCursor(rX0, gy + 4);
      display.print(buf);
    }

    // Rain Bars (Drawn behind the line chart)
    int maxRain = 1;
    for (int i = 0; i < 7; i++)
      if (s.rain[i] > maxRain)
        maxRain = s.rain[i];
    const int barW = 22;
    for (int i = 0; i < 7; i++)
    {
      if (s.rain[i] <= 0)
        continue;
      int bxC = gX0 + i * (gX1 - gX0) / 6;
      int bh = (s.rain[i] * gH) / (maxRain * 2); // Bars cap at half chart height
      int bx0 = bxC - barW / 2;
      display.drawRect(bx0, gY1 - bh, barW, bh, GxEPD_BLACK);
      for (int hy = gY1 - bh + 3; hy < gY1 - 1; hy += 3)
        display.drawLine(bx0 + 2, hy, bx0 + barW - 2, hy, GxEPD_BLACK);
    }

    // Water level trend lines (3px thickness via offset drawing)
    auto pt = [&](int i, int v, int &px, int &py)
    {
      px = gX0 + i * (gX1 - gX0) / 6;
      py = gY1 - (v * gH) / 100;
    };
    for (int i = 0; i < 6; i++)
    {
      int x1, y1, x2, y2;
      pt(i, s.trend[i], x1, y1);
      pt(i + 1, s.trend[i + 1], x2, y2);
      for (int k = -1; k <= 1; k++)
        display.drawLine(x1, y1 + k, x2, y2 + k, GxEPD_BLACK);
    }

    display.setFont(&FreeSans9pt7b);
    drawCenteredText("Level Trend & Rain mm", rX0, rX1, gY1 + 16);

    // ---------- FOOTER DATA ----------
    // Center Column Footer
    display.setFont(&FreeSans9pt7b);
    display.setCursor(cX, 252);
    display.print("SENSOR: ");
    display.print(s.sensorDistanceCm, 1);
    display.print("cm");

    display.setCursor(cX, 266);
    display.print("UPDATED ");
    display.print(s.updatedAt);

    // Right Column Footer
    display.setCursor(rX0, 266);
    display.print("Battery Level: ");
    display.print(s.batteryLevel);
    display.print("%");

    const char *espLabel = "ESP32 LINKED";
    int16_t bx, by;
    uint16_t bw, bh;
    display.getTextBounds(espLabel, 0, 0, &bx, &by, &bw, &bh);
    int espX = rX1 - bw - 12;
    display.fillCircle(espX - 4, 262, 3, GxEPD_BLACK);
    display.setCursor(espX + 4, 266);
    display.print(espLabel);

  } while (display.nextPage());
}

// ============================================================================
// ESP-NOW Receive Callback
// ============================================================================
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  const uint8_t *mac_addr = recv_info->src_addr;
#else
void onDataRecv(const uint8_t *mac_addr, const uint8_t *data, int len) {
#endif
  char incomingMsg[len + 1];
  memcpy(incomingMsg, data, len);
  incomingMsg[len] = '\0';
  
  Serial.print("ESP-NOW Packet Received: ");
  Serial.println(incomingMsg);
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, incomingMsg);
  if (error) {
    Serial.print("JSON Parse Error: ");
    Serial.println(error.c_str());
    return;
  }
  
  // Extract parsed JSON payload attributes
  float distance = doc["distance"] | -1.0f;
  float batteryVal = doc["battery"] | -1.0f;
  float temp = doc["temp"] | -127.0f;
  int rainTips = doc["rain"] | 0;
  
  if (distance < 0.0f) {
    Serial.println("Skipping invalid/empty distance reading.");
    return;
  }
  
  // 1. Calculate percentage based on 153cm (empty) and 25cm (full) depth boundaries
  float waterDepth = 153.0f - distance;
  int percent = (int)((waterDepth / 128.0f) * 100.0f);
  if (percent > 100) percent = 100;
  if (percent < 0) percent = 0;
  
  // 2. Set Status (Compare with last logged reading)
  if (percent > currentState.percentage) {
    currentState.status = "RISING";
  } else if (percent < currentState.percentage) {
    currentState.status = "FALLING";
  } else {
    currentState.status = "STABLE";
  }
  
  // 3. Compute running aggregates
  currentState.percentage = percent;
  currentState.sensorDistanceCm = distance;
  currentState.batteryLevel = (int)batteryVal;
  currentState.currentGallons = (int)((percent / 100.0f) * 1600.0f);
  currentState.daysRemaining = currentState.currentGallons / currentState.dailyUse;
  
  // 4. Update timezone clock stamp
  time_t nowTime = time(nullptr);
  struct tm timeInfo;
  char timeBuf[16];
  if (getLocalTime(&timeInfo)) {
    strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", &timeInfo);
    currentState.updatedAt = timeBuf;
  } else {
    currentState.updatedAt = "NTP Error";
  }
  
  // 5. Append telemetry to the microSD CSV
  DataRecord rec;
  rec.measured_on = (uint32_t)nowTime;
  rec.water_surface_dist_cm = distance;
  rec.estimated_gallons = currentState.currentGallons;
  rec.cistern_percentage = currentState.percentage;
  rec.battery_percentage = currentState.batteryLevel;
  rec.temp_c = temp;
  rec.rain_tips = (uint16_t)rainTips;
  logDataToSD(rec);
  
  // 6. Reload trend matrices from microSD
  loadTrendDataFromSD();
  
  // 7. Update display with active changes
  Serial.println("Pushed new frame to e-paper display.");
  drawDashboard(currentState);
  display.hibernate();
  
  // 8. Update HomeSpan HomeKit Characteristics
  if (hkPercentageSensor) {
    hkPercentageSensor->setVal(currentState.percentage);
  }
  if (hkTempSensor && temp > -100.0f) {
    hkTempSensor->setVal(temp);
  }
  if (hkBatteryLevel) {
    hkBatteryLevel->setVal(currentState.batteryLevel);
    if (currentState.batteryLevel < 20) {
      hkLowBattery->setVal(1); // Trigger Low Battery indicator in Apple Home
    } else {
      hkLowBattery->setVal(0);
    }
  }
}

// ============================================================================
// Arduino main entry loops
// ============================================================================
void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println("\nCistern Display Node Booting...");

  // Power on display panel
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, HIGH);
  delay(10);

  // Initialize E-Paper SPI bus
  SPI.end();
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  
  display.init(115200, /*initial*/ true, /*reset ms*/ 50, /*pulldown rst*/ false);
  display.setRotation(0); // Landscape native mode

  // Initialize microSD Card on independent HSPI SPI lines
  Serial.println("Mounting microSD card...");
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI)) {
    Serial.println("microSD initialization failed! Running in offline mode.");
  } else {
    Serial.println("microSD mounted successfully.");
    // Load historical trend points on boot
    loadTrendDataFromSD();
  }

  // Draw initial dashboard frame
  drawDashboard(currentState);
  display.hibernate();

  // Connect to Local Wi-Fi Router
  Serial.println("Connecting to Wi-Fi Network...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin("Forest-Enclave-IoT", "c@nn@b!skiss420");
  
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi Connected! IP: " + WiFi.localIP().toString());
    // Synchronize clock against NTP with Eastern Standard / Daylight timerules
    configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
    
    // Wait brief interval to confirm NTP sync
    struct tm ntpTime;
    int syncChecks = 0;
    while (!getLocalTime(&ntpTime) && syncChecks < 10) {
      delay(500);
      Serial.println("Waiting for NTP sync...");
      syncChecks++;
    }
    
    if (getLocalTime(&ntpTime)) {
      Serial.println("NTP Clock sync complete.");
      char bootTimeBuf[16];
      strftime(bootTimeBuf, sizeof(bootTimeBuf), "%I:%M %p", &ntpTime);
      currentState.updatedAt = bootTimeBuf;
    }
  } else {
    Serial.println("\nWi-Fi timeout. Standard offline mode active.");
  }

  // Set up ESP-NOW Receiver
  if (esp_now_init() == ESP_OK) {
    Serial.println("ESP-NOW initialized successfully.");
    esp_now_register_recv_cb(onDataRecv);
  } else {
    Serial.println("ESP-NOW setup failed!");
  }

  // Initialize HomeSpan Access Server
  homeSpan.begin(Category::Sensors, "Cistern Gateway");
  new CisternHomeKitDevice();
}

void loop()
{
  // Continuously poll HomeSpan to respond to Apple HomeKit client requests
  homeSpan.poll();
  delay(10);
}
