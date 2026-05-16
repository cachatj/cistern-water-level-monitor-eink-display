#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

// ============================================================================
// Pin map — defaults match the CrowPanel 5.79" ESP32-S3 E-Paper board.
// Verify against your board's schematic before powering up.
// If you're wiring a bare ESP32-S3 dev kit to a Good Display GDEY0579T93,
// pick any free GPIOs and update these defines.
// ============================================================================
#define EPD_CS 45
#define EPD_DC 46
#define EPD_RST 47
#define EPD_BUSY 48
#define EPD_SCK 12
#define EPD_MOSI 11
// CrowPanel boards gate display power through a GPIO. If your hardware
// doesn't have this, comment out the pinMode/digitalWrite in setup().
#define EPD_PWR 7

// Driver class for the 5.79" / 272 x 792 B/W panel.
// Requires GxEPD2 >= 1.5.4 (your platformio.ini pins ^1.6.9 — fine).
// Full-buffer instance: 272 * 792 / 8 ≈ 27 KB, comfortable on ESP32-S3.
GxEPD2_BW<GxEPD2_579_GDEY0579T93, GxEPD2_579_GDEY0579T93::HEIGHT> display(
    GxEPD2_579_GDEY0579T93(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ============================================================================
// State the dashboard renders. For now this is hardcoded; later it will be
// populated from the level sensor, an RTC, a weather feed, etc.
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
  const char *status;    // "RISING" / "FALLING" / "STABLE"
  const char *updatedAt; // pre-formatted clock time
  int trend[7];          // 0-100, oldest -> newest
  int rain[7];           // 0-100, arbitrary scale (mm * 10, etc.)
  uint8_t weather[7];    // 0=sun, 1=cloud-outline, 2=rain, 3=partly-cloudy
};

static const DashboardState dummy = {
    /* current        */ 1332,
    /* capacity       */ 1825,
    /* percent        */ 73,
    /* dailyUse       */ 45,
    /* daysRemaining  */ 29,
    /* change12h      */ 4.2f,
    /* sensorDistance */ 42.1f,
    /* battery        */ 95,
    /* status         */ "RISING",
    /* updatedAt      */ "10:42 AM",
    /* trend          */ {50, 56, 60, 76, 90, 87, 78},
    /* rain           */ {0, 0, 30, 95, 0, 0, 0},
    /* weather        */ {0, 1, 2, 2, 3, 0, 3},
};

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
  // Mask the inside cusps with a thin white sliver so the outline reads clean.
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
// drawDashboard — renders the whole 792x272 frame in one full update.
// ============================================================================
void drawDashboard(const DashboardState &s)
{
  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    // ---------- LEFT PANEL: tank (x = 0 .. 250) ----------
    const int tankX = 60, tankY = 22;
    const int tankW = 130, tankH = 220;

    // top/bottom fittings (small nubs)
    display.fillRect(tankX + tankW / 2 - 9, tankY - 7, 18, 7, GxEPD_BLACK);
    display.fillRect(tankX + tankW / 2 - 9, tankY + tankH, 18, 7, GxEPD_BLACK);

    // double-stroke outline for a heavier line weight
    display.drawRoundRect(tankX, tankY, tankW, tankH, 14, GxEPD_BLACK);
    display.drawRoundRect(tankX + 1, tankY + 1, tankW - 2, tankH - 2, 13, GxEPD_BLACK);

    // water fill from the bottom
    const int fillH = (s.percentage * (tankH - 6)) / 100;
    const int waterY = tankY + tankH - 3 - fillH;
    display.fillRect(tankX + 3, waterY, tankW - 6, fillH, GxEPD_BLACK);

    // tick marks (0, 25, 50, 75, 100) on the right of the tank
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

    // big "73" — colour flips depending on whether the centre falls in water
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

    // ---------- CENTER COLUMN: stats (x = 260 .. 470) ----------
    const int cX = 270;
    int y = 26;

    display.setFont(&FreeSans9pt7b);
    display.setCursor(cX, y);
    display.print("STATUS");

    y += 24;
    display.fillTriangle(cX, y, cX + 14, y, cX + 7, y - 14, GxEPD_BLACK);
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
    block("DAYS REMAINING", "~" + String(s.daysRemaining) + " days", true);

    // ---------- DIVIDER ----------
    display.drawLine(475, 12, 475, 260, GxEPD_BLACK);

    // ---------- RIGHT PANEL: weather + chart (x = 480 .. 792) ----------
    const int rX0 = 488, rX1 = 786;
    const int rW = rX1 - rX0;

    // weather row
    const char *days[7] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
    for (int i = 0; i < 7; i++)
    {
      int slotW = rW / 7;
      int ix = rX0 + i * slotW + slotW / 2;
      drawWeatherIcon(s.weather[i], ix, 24);
      display.setFont(&FreeSans9pt7b);
      int16_t bx, by;
      uint16_t bw, bh;
      display.getTextBounds(days[i], 0, 0, &bx, &by, &bw, &bh);
      display.setCursor(ix - bw / 2, 58);
      display.print(days[i]);
    }

    // chart frame
    const int gX0 = rX0 + 22;
    const int gX1 = rX1 - 4;
    const int gY0 = 78;
    const int gY1 = 220;
    const int gH = gY1 - gY0;

    // dotted grid + y-axis labels
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

    // rain bars (drawn first so the trend line stays on top)
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
      int bh = (s.rain[i] * gH) / (maxRain * 2); // bars max half the chart
      int bx0 = bxC - barW / 2;
      display.drawRect(bx0, gY1 - bh, barW, bh, GxEPD_BLACK);
      for (int hy = gY1 - bh + 3; hy < gY1 - 1; hy += 3)
        display.drawLine(bx0 + 2, hy, bx0 + barW - 2, hy, GxEPD_BLACK);
    }

    // trend line (3px thick by stacking)
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

    // chart caption
    display.setFont(&FreeSans9pt7b);
    drawCenteredText("Rain Measured", rX0, rX1, gY1 + 16);

    // ---------- BOTTOM STRIP ----------
    // center-column footer (sensor + clock)
    display.setFont(&FreeSans9pt7b);
    display.setCursor(cX, 252);
    display.print("SENSOR: ");
    display.print(s.sensorDistanceCm, 1);
    display.print("cm");

    display.setCursor(cX, 266);
    display.print("UPDATED ");
    display.print(s.updatedAt);

    // right-panel footer (battery + connection)
    display.setCursor(rX0, 266);
    display.print("Battery Level: ");
    display.print(s.batteryLevel);
    display.print("%");

    const char *esp = "ESP32 CONNECTED";
    int16_t bx, by;
    uint16_t bw, bh;
    display.getTextBounds(esp, 0, 0, &bx, &by, &bw, &bh);
    int espX = rX1 - bw - 12;
    display.fillCircle(espX - 4, 262, 3, GxEPD_BLACK);
    display.setCursor(espX + 4, 266);
    display.print(esp);

  } while (display.nextPage());
}

// ============================================================================
// Arduino entry points
// ============================================================================
void setup()
{
  Serial.begin(115200);
  delay(50);
  Serial.println("\nCistern display booting...");

  // Power the panel (CrowPanel-style boards only — remove for bare wiring).
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, HIGH);
  delay(10);

  SPI.end();
  SPI.begin(EPD_SCK, /*MISO*/ -1, EPD_MOSI, EPD_CS);

  display.init(115200, /*initial*/ true, /*reset ms*/ 50, /*pulldown rst*/ false);
  // The GDEY0579T93 is landscape-native in GxEPD2 (WIDTH=792, HEIGHT=272).
  // Use 0 for cable-on-left, 2 for cable-on-right (i.e. flip if upside down).
  display.setRotation(0); // landscape: 792 x 272

  drawDashboard(dummy);

  display.hibernate();
  Serial.println("Frame pushed. Sleeping.");
}

void loop()
{
  // Dashboard is drawn once in setup(); add a wake-on-timer refresh later.
  delay(1000);
}
