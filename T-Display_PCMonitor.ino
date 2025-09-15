#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <string.h>

// --- Use LilyGO's pin_config.h if present ---
#if __has_include("pin_config.h")
  #include "pin_config.h"
#endif

// Fallbacks if pin_config.h isn't present
#ifndef PIN_POWER_ON
  #define PIN_POWER_ON 15
#endif
#ifndef PIN_LCD_BL
  #define PIN_LCD_BL 38
#endif
#ifndef TFT_BACKLIGHT_ON
  #define TFT_BACKLIGHT_ON HIGH
#endif

TFT_eSPI tft;  // Uses your TFT_eSPI Setup206 mapping

// ---------- Colors ----------
#define COL_BG     TFT_BLACK
#define COL_FRAME  TFT_DARKGREY
#define COL_TEXT   TFT_WHITE
#define COL_CPU    TFT_CYAN
#define COL_MEM    TFT_ORANGE
#define COL_GPU    TFT_GREEN
#define COL_SPARK  TFT_DARKGREEN   // sparkline color; tweak per metric if you like

// ---------- Layout ----------
static const int W = 320;
static const int H = 170;
static const int MARGIN    = 8;
static const int TITLE_Y   = 6;

static const int BAR_H     = 22;  // a bit smaller to make room for sparklines
static const int SPARK_H   = 12;
static const int BAR_GAP   = 8;
static const int BARS_TOP  = 44;  // area starts below title + rule

// Total per metric block = BAR_H + 2 + SPARK_H + BAR_GAP
// With 3 metrics this fits comfortably in 170px height.

// ---------- Animation / History ----------
static const float   EASE_ALPHA = 0.20f;   // smoothing toward target
static const uint32_t FRAME_MS  = 33;      // ~30 FPS
static const int     HIST_LEN   = 64;      // points per sparkline
static const uint32_t HIST_PUSH_EVERY_MS = 200; // sample history 5 Hz

// Targets from feeder
static float tgtCPU=0, tgtMEM=0, tgtGPU=0;

// Animated currents
static float curCPU=0, curMEM=0, curGPU=0;

// History buffers
static float histCPU[HIST_LEN] = {0};
static float histMEM[HIST_LEN] = {0};
static float histGPU[HIST_LEN] = {0};
static int   histIndex = 0;
static uint32_t nextHistPushAt = 0;

// Redraw pacing
static uint32_t nextFrameAt = 0;

// ---------- Helpers ----------
static inline int clamp100f(float v) {
  if (v < 0)   return 0;
  if (v > 100) return 100;
  return (int)v;
}

static void drawHeader() {
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("RealTime PC Stats", MARGIN, TITLE_Y, 4);
  tft.drawFastHLine(0, 36, W, COL_FRAME);
  tft.setTextFont(2);
  tft.drawString("Send: cpu,mem,gpu @115200", MARGIN, 38);
}

// Draw the filled bar and its label/value
static void drawBar(int y, int pct, uint16_t color, const char* label, const char* rightHint = nullptr) {
  const int x = MARGIN;
  const int w = W - MARGIN*2;

  // frame
  tft.drawRoundRect(x, y, w, BAR_H, 6, COL_FRAME);

  // fill
  const int fill = (w - 2) * pct / 100;
  if (fill > 0) tft.fillRoundRect(x + 1, y + 1, fill, BAR_H - 2, 5, color);

  // left text
  char buf[40];
  snprintf(buf, sizeof(buf), "%s %d%%", label, pct);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(buf, x + 6, y + BAR_H/2, 2);

  // optional right-side hint (e.g. a temperature reading)
  if (rightHint && rightHint[0]) {
    tft.setTextDatum(MR_DATUM);
    tft.drawString(rightHint, x + w - 6, y + BAR_H/2, 2);
  }
}

// Simple sparkline renderer: scales 0..100 into 0..SPARK_H
static void drawSparkline(int x, int y, int w, int h, const float* hist, uint16_t color) {
  // clear area
  tft.fillRect(x, y, w, h, COL_BG);
  // frame (optional)
  tft.drawRect(x, y, w, h, COL_FRAME);

  // draw line across width by connecting history points
  // We remap HIST_LEN samples into w-2 pixels.
  if (w <= 2) return;

  int lastX = -1, lastY = -1;
  for (int px = 1; px < w-1; ++px) {
    // position in circular buffer from oldest to newest left->right
    // oldest at left, newest at right
    float pos = (float)px * (HIST_LEN - 1) / (float)(w - 2);
    int i0 = (int)pos;
    int i1 = (i0 + 1 < HIST_LEN) ? (i0 + 1) : i0;
    float t = pos - i0;

    // Convert logical indices to circular buffer indices
    int base = (histIndex + 1) % HIST_LEN; // oldest
    int idx0 = (base + i0) % HIST_LEN;
    int idx1 = (base + i1) % HIST_LEN;

    float v = hist[idx0] * (1.0f - t) + hist[idx1] * t;
    if (v < 0) v = 0; if (v > 100) v = 100;

    int py = y + (h-2) - (int)((h-2) * (v / 100.0f));  // invert so higher = higher
    int pxAbs = x + px;

    if (lastX >= 0) {
      tft.drawLine(lastX, lastY, pxAbs, py, color);
    }
    lastX = pxAbs;
    lastY = py;
  }
}

static void drawAll(int cpu, int mem, int gpu) {
  // Clear the metric area below the rule
  tft.fillRect(0, 36, W, H - 36, COL_BG);

  int y = BARS_TOP;

  // CPU
  drawBar(y, cpu, COL_CPU, "CPU");
  drawSparkline(MARGIN, y + BAR_H + 2, W - MARGIN*2, SPARK_H, histCPU, COL_SPARK);
  y += BAR_H + 2 + SPARK_H + BAR_GAP;

  // MEM
  drawBar(y, mem, COL_MEM, "MEM");
  drawSparkline(MARGIN, y + BAR_H + 2, W - MARGIN*2, SPARK_H, histMEM, COL_SPARK);
  y += BAR_H + 2 + SPARK_H + BAR_GAP;

  // GPU
  drawBar(y, gpu, COL_GPU, "GPU");
  drawSparkline(MARGIN, y + BAR_H + 2, W - MARGIN*2, SPARK_H, histGPU, COL_SPARK);
}

static void pushHistoryIfDue() {
  const uint32_t now = millis();
  if (now < nextHistPushAt) return;
  nextHistPushAt = now + HIST_PUSH_EVERY_MS;

  // push latest currents (clamped) into circular buffers
  histCPU[histIndex] = clamp100f(curCPU);
  histMEM[histIndex] = clamp100f(curMEM);
  histGPU[histIndex] = clamp100f(curGPU);
  histIndex = (histIndex + 1) % HIST_LEN;
}

static void animateAndDrawIfDue() {
  const uint32_t now = millis();
  if (now < nextFrameAt) return;
  nextFrameAt = now + FRAME_MS;

  // Ease toward targets
  curCPU += (tgtCPU - curCPU) * EASE_ALPHA;
  curMEM += (tgtMEM - curMEM) * EASE_ALPHA;
  curGPU += (tgtGPU - curGPU) * EASE_ALPHA;

  // Periodically push to history
  pushHistoryIfDue();

  // Redraw only if bars changed visibly (save work)
  static int lastC=-1, lastM=-1, lastG=-1;
  int C = clamp100f(curCPU);
  int M = clamp100f(curMEM);
  int G = clamp100f(curGPU);

  if (C != lastC || M != lastM || G != lastG) {
    drawAll(C, M, G);
    lastC=C; lastM=M; lastG=G;
  }
}

// Parse lines like: cpu,mem,gpu
static void parseLine(char* s) {
  const int MAXTOK = 3;
  char* tok[MAXTOK] = {0};
  int ntok = 0;

  if (!s) return;

  char* part = strtok(s, ",");
  while (part && ntok < MAXTOK) {
    tok[ntok++] = part;
    part = strtok(nullptr, ",");
  }

  if (ntok >= 3) {
    tgtCPU = atof(tok[0]);
    tgtMEM = atof(tok[1]);
    tgtGPU = atof(tok[2]);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_POWER_ON, OUTPUT); digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(PIN_LCD_BL,   OUTPUT); digitalWrite(PIN_LCD_BL, TFT_BACKLIGHT_ON);

  tft.init();
  tft.setRotation(1);
  drawHeader();

  // settle USB CDC (helps avoid S3 USB hiccups)
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 1500) { delay(10); }
  delay(100);
}

void loop() {
  // Robust line reader
  static char    buf[96];
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

  animateAndDrawIfDue();
  delay(1);
}
