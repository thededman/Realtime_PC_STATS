#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>

// If present, uses LilyGO's pin_config.h (power, BL, button pins, etc.)
#if __has_include("pin_config.h")
  #include "pin_config.h"
#endif

// Fallback pins if pin_config.h isn't present
#ifndef PIN_POWER_ON
  #define PIN_POWER_ON 15
#endif
#ifndef PIN_LCD_BL
  #define PIN_LCD_BL 38
#endif
#ifndef TFT_BACKLIGHT_ON
  #define TFT_BACKLIGHT_ON HIGH
#endif
#ifndef PIN_BUTTON_1            // NEXT button
  #define PIN_BUTTON_1 0        // BOOT on many T-Display-S3s
#endif
#ifndef PIN_BUTTON_2            // BACK button
  #define PIN_BUTTON_2 14       // second side button on many units
#endif

TFT_eSPI tft;  // Uses your TFT_eSPI Setup206 mapping

// ---------- Colors ----------
#define COL_BG     TFT_BLACK
#define COL_FRAME  TFT_DARKGREY
#define COL_TEXT   TFT_WHITE
#define COL_CPU    TFT_CYAN
#define COL_GPU    TFT_GREEN
#define COL_DISK   TFT_MAGENTA
#define COL_SPARK  TFT_DARKGREEN

// ---------- Layout ----------
static const int W = 320;
static const int H = 170;
static const int MARGIN    = 8;
static const int TITLE_Y   = 4;   // top-left Y for title
static const int RULE_Y    = 40;  // separator line below title

static const int BAR_H     = 22;
static const int SPARK_H   = 12;
static const int BAR_GAP   = 8;
static const int BARS_TOP  = 52;  // content starts below title/rule

// ---------- Animation / History ----------
static const float   EASE_ALPHA = 0.20f;   // smoothing toward target
static const uint32_t FRAME_MS  = 33;      // ~30 FPS
static const int     HIST_LEN   = 64;      // points per sparkline
static const uint32_t HIST_PUSH_EVERY_MS = 200; // 5 Hz history

// Targets from feeder
static float tgtCPU=0, tgtMEM=0, tgtGPU=0, tgtDSK=0;
static float tgtDiskMBps = -1.0f;  // optional (negative = not provided)

// Temps & free space (sent by feeder)
static float cpuTempF = -999.0f;
static float gpuTempF = -999.0f;
static float freeC_GB = -1.0f;
static float freeD_GB = -1.0f;

// Animated currents
static float curCPU=0, curMEM=0, curGPU=0, curDSK=0;

// History buffers
static float histCPU[HIST_LEN] = {0};
static float histMEM[HIST_LEN] = {0};
static float histGPU[HIST_LEN] = {0};
static float histDSK[HIST_LEN] = {0};
static int   histIndex = 0;
static uint32_t nextHistPushAt = 0;

// Redraw pacing
static uint32_t nextFrameAt = 0;

// Page state: 0=CPU, 1=GPU, 2=DISK
static int page = 0;

// Button debounce
static uint32_t btn1LastChange = 0, btn2LastChange = 0;
static bool lastBtn1 = true, lastBtn2 = true;   // INPUT_PULLUP → released = HIGH
static const uint32_t BTN_DEBOUNCE_MS = 80;

// ---------- Helpers ----------
static inline int clamp100f(float v) {
  if (v < 0)   return 0;
  if (v > 100) return 100;
  return (int)v;
}

// Use TFT_eSPI’s built-in FreeFonts (LOAD_GFXFF must be enabled in User_Setup.h)
static void useTitleFont() {
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold24pt7b);   // big title
}
static void useLabelFont() {
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSans12pt7b);       // labels/info
}

static void drawHeader() {
  tft.fillScreen(COL_BG);

  // Title
  useTitleFont();
  tft.drawString("PC Stats", MARGIN, TITLE_Y);

  // Separator line
  tft.drawFastHLine(0, RULE_Y, W, COL_FRAME);

  // Hint line
  useLabelFont();
  tft.drawString("Buttons: Back / Next to cycle pages", MARGIN, RULE_Y + 2);
}

static void drawBar(int y, int pct, uint16_t color, const char* label, const char* rightHint = nullptr) {
  const int x = MARGIN;
  const int w = W - MARGIN*2;

  // frame
  tft.drawRoundRect(x, y, w, BAR_H, 6, COL_FRAME);

  // fill
  const int fill = (w - 2) * pct / 100;
  if (fill > 0) tft.fillRoundRect(x + 1, y + 1, fill, BAR_H - 2, 5, color);

  // text
  char buf[40];
  snprintf(buf, sizeof(buf), "%s %d%%", label, pct);

  useLabelFont();
  tft.setTextDatum(ML_DATUM);
  tft.drawString(buf, x + 6, y + BAR_H/2 - 6); // tweak -6 to visually center with this font

  if (rightHint && rightHint[0]) {
    tft.setTextDatum(MR_DATUM);
    tft.drawString(rightHint, x + w - 6, y + BAR_H/2 - 6);
  }
}

static void drawSparkline(int x, int y, int w, int h, const float* hist, uint16_t color) {
  tft.fillRect(x, y, w, h, COL_BG);
  tft.drawRect(x, y, w, h, COL_FRAME);
  if (w <= 2) return;

  int lastX = -1, lastY = -1;
  for (int px = 1; px < w-1; ++px) {
    float pos = (float)px * (HIST_LEN - 1) / (float)(w - 2);
    int i0 = (int)pos;
    int i1 = (i0 + 1 < HIST_LEN) ? (i0 + 1) : i0;
    float t = pos - i0;

    int base = (histIndex + 1) % HIST_LEN; // oldest at left
    int idx0 = (base + i0) % HIST_LEN;
    int idx1 = (base + i1) % HIST_LEN;

    float v = hist[idx0]*(1.0f - t) + hist[idx1]*t;
    if (v < 0) v = 0; if (v > 100) v = 100;

    int py = y + (h-2) - (int)((h-2) * (v / 100.0f));
    int pxAbs = x + px;

    if (lastX >= 0) tft.drawLine(lastX, lastY, pxAbs, py, color);
    lastX = pxAbs; lastY = py;
  }
}

static void drawCPUPage(int C) {
  tft.fillRect(0, RULE_Y, W, H - RULE_Y, COL_BG);

  int y = BARS_TOP;
  drawBar(y, C, COL_CPU, "CPU");
  drawSparkline(MARGIN, y + BAR_H + 2, W - MARGIN*2, SPARK_H, histCPU, COL_SPARK);
  y += BAR_H + 2 + SPARK_H + BAR_GAP;

  useLabelFont();
  if (cpuTempF > -100.0f) {
    char buf[32];
    snprintf(buf, sizeof(buf), "CPU Temp: %.1f F", cpuTempF);
    tft.drawString(buf, MARGIN, y);
  } else {
    tft.drawString("CPU Temp: N/A", MARGIN, y);
  }
}

static void drawGPUPage(int G) {
  tft.fillRect(0, RULE_Y, W, H - RULE_Y, COL_BG);

  int y = BARS_TOP;
  drawBar(y, G, COL_GPU, "GPU");
  drawSparkline(MARGIN, y + BAR_H + 2, W - MARGIN*2, SPARK_H, histGPU, COL_SPARK);
  y += BAR_H + 2 + SPARK_H + BAR_GAP;

  useLabelFont();
  if (gpuTempF > -100.0f) {
    char buf[32];
    snprintf(buf, sizeof(buf), "GPU Temp: %.1f F", gpuTempF);
    tft.drawString(buf, MARGIN, y);
  } else {
    tft.drawString("GPU Temp: N/A", MARGIN, y);
  }
}

static void drawDISKPage(int D) {
  tft.fillRect(0, RULE_Y, W, H - RULE_Y, COL_BG);

  int y = BARS_TOP;
  char hint[24] = {0};
  if (tgtDiskMBps >= 0.0f) snprintf(hint, sizeof(hint), "%.1f MB/s", tgtDiskMBps);
  drawBar(y, D, COL_DISK, "DISK", (tgtDiskMBps >= 0.0f) ? hint : nullptr);
  drawSparkline(MARGIN, y + BAR_H + 2, W - MARGIN*2, SPARK_H, histDSK, COL_SPARK);
  y += BAR_H + 2 + SPARK_H + BAR_GAP;

  useLabelFont();
  char line1[48], line2[48];
  if (freeC_GB >= 0.0f) snprintf(line1, sizeof(line1), "C: Free %.0f GB", freeC_GB);
  else snprintf(line1, sizeof(line1), "C: Free N/A");
  if (freeD_GB >= 0.0f) snprintf(line2, sizeof(line2), "D: Free %.0f GB", freeD_GB);
  else snprintf(line2, sizeof(line2), "D: Free N/A");
  tft.drawString(line1, MARGIN, y);
  tft.drawString(line2, MARGIN, y + 18);
}

static void pushHistoryIfDue() {
  const uint32_t now = millis();
  if (now < nextHistPushAt) return;
  nextHistPushAt = now + HIST_PUSH_EVERY_MS;

  histCPU[histIndex] = clamp100f(curCPU);
  histMEM[histIndex] = clamp100f(curMEM);
  histGPU[histIndex] = clamp100f(curGPU);
  histDSK[histIndex] = clamp100f(curDSK);
  histIndex = (histIndex + 1) % HIST_LEN;
}

static void animateAndDrawIfDue() {
  const uint32_t now = millis();
  if (now < nextFrameAt) return;
  nextFrameAt = now + FRAME_MS;

  curCPU += (tgtCPU - curCPU) * EASE_ALPHA;
  curMEM += (tgtMEM - curMEM) * EASE_ALPHA;
  curGPU += (tgtGPU - curGPU) * EASE_ALPHA;
  curDSK += (tgtDSK - curDSK) * EASE_ALPHA;

  pushHistoryIfDue();

  static int lastC=-1, lastM=-1, lastG=-1, lastD=-1;
  int C = clamp100f(curCPU);
  int M = clamp100f(curMEM);
  int G = clamp100f(curGPU);
  int D = clamp100f(curDSK);

  bool changed = false;
  if (C != lastC) { lastC=C; changed=true; }
  if (M != lastM) { lastM=M; changed=true; }
  if (G != lastG) { lastG=G; changed=true; }
  if (D != lastD) { lastD=D; changed=true; }

  if (!changed) return;

  switch (page) {
    case 0: drawCPUPage(C); break;
    case 1: drawGPUPage(G); break;
    default: drawDISKPage(D); break;
  }
}

static void handleButtons() {
  uint32_t now = millis();

  // NEXT button (PIN_BUTTON_1)
  bool b1 = digitalRead(PIN_BUTTON_1);
  if (b1 != lastBtn1 && (now - btn1LastChange) > BTN_DEBOUNCE_MS) {
    btn1LastChange = now;
    lastBtn1 = b1;
    if (b1 == LOW) { // pressed
      page = (page + 1) % 3;
      switch (page) {
        case 0: drawCPUPage(clamp100f(curCPU)); break;
        case 1: drawGPUPage(clamp100f(curGPU)); break;
        case 2: drawDISKPage(clamp100f(curDSK)); break;
      }
    }
  }

  // BACK button (PIN_BUTTON_2)
  bool b2 = digitalRead(PIN_BUTTON_2);
  if (b2 != lastBtn2 && (now - btn2LastChange) > BTN_DEBOUNCE_MS) {
    btn2LastChange = now;
    lastBtn2 = b2;
    if (b2 == LOW) { // pressed
      page = (page + 3 - 1) % 3;   // wrap backwards
      switch (page) {
        case 0: drawCPUPage(clamp100f(curCPU)); break;
        case 1: drawGPUPage(clamp100f(curGPU)); break;
        case 2: drawDISKPage(clamp100f(curDSK)); break;
      }
    }
  }
}

// Parse lines like:
// cpu,mem,gpu,diskPct,diskMBps,cpuTempF,gpuTempF,freeC_GB,freeD_GB
static void parseLine(char* s) {
  const int MAXTOK = 9;
  char* tok[MAXTOK] = {0};
  int ntok = 0;

  for (char* p = s; *p && ntok < MAXTOK; ++p) {
    if (ntok == 0) tok[ntok++] = p;
    if (*p == ',') { *p = 0; if (*(p+1)) tok[ntok++] = p+1; }
  }
  if (ntok >= 1) tgtCPU = atof(tok[0]);
  if (ntok >= 2) tgtMEM = atof(tok[1]);
  if (ntok >= 3) tgtGPU = atof(tok[2]);
  if (ntok >= 4) tgtDSK = atof(tok[3]);
  if (ntok >= 5) tgtDiskMBps = atof(tok[4]); else tgtDiskMBps = -1.0f;
  if (ntok >= 6) cpuTempF = atof(tok[5]);    else cpuTempF = -999.0f;
  if (ntok >= 7) gpuTempF = atof(tok[6]);    else gpuTempF = -999.0f;
  if (ntok >= 8) freeC_GB = atof(tok[7]);    else freeC_GB = -1.0f;
  if (ntok >= 9) freeD_GB = atof(tok[8]);    else freeD_GB = -1.0f;
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_POWER_ON, OUTPUT); digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(PIN_LCD_BL,   OUTPUT); digitalWrite(PIN_LCD_BL, TFT_BACKLIGHT_ON);

  pinMode(PIN_BUTTON_1, INPUT_PULLUP);
  pinMode(PIN_BUTTON_2, INPUT_PULLUP);
  lastBtn1 = digitalRead(PIN_BUTTON_1);
  lastBtn2 = digitalRead(PIN_BUTTON_2);

  tft.init();
  tft.setRotation(1);

  drawHeader();

  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 1500) { delay(10); }
  delay(100);

  drawCPUPage(0);
}

void loop() {
  // Read serial lines
  static char    buf[160];
  static size_t  n = 0;

  while (Serial.available()) {
    int ch = Serial.read();
    if (ch < 0) break;
    if (ch == '\r') continue;

    if (ch == '\n') {
      buf[n] = 0;
      parseLine(buf);
      n = 0;
    } else {
      if (n < sizeof(buf) - 1) buf[n++] = (char)ch;
      else n = 0; // overflow guard
    }
  }

  handleButtons();
  animateAndDrawIfDue();
  delay(1);
}
