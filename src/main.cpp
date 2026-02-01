#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <math.h>
#include "Free_Fonts.h"   // Bodmer free fonts
#include "weather_integration.h"
#include "config_portal.h"

// M5Stack Core3 PC Monitor Dashboard + WiFi Web Server + Weather Mode
// - Modes: CPU, GPU, DISK, WEATHER (cycle with right/left buttons)
// - Smooth bar animations + 60-sample sparkline
// - Parses CSV from feeder GUI (USB serial) at 115200:
//   cpu,mem,gpu,diskPct,diskMBps,cpuTempF,gpuTempF,freeC_GB,freeD_GB
// - WiFi server:
//   GET /       -> live HTML dashboard (auto-refresh via JS)
//   GET /metrics -> JSON {cpu, mem, gpu, diskPct, diskMBps, cpuTempF, gpuTempF, freeC, freeD}
//   GET /ip     -> plain text IP

// WiFi credentials now come from config portal (stored in NVS)

// Full-screen sprite for flicker-free rendering (M5Unified)
LGFX_Sprite gfx(&M5.Display); // off-screen framebuffer (RGB565)

// Touch swipe tracking
int touchStartX = -1;
bool touchActive = false;
static const int SWIPE_THRESHOLD = 50;

// Long press detection for setup mode
uint32_t touchStartTime = 0;
bool longPressTriggered = false;
static const uint32_t LONG_PRESS_MS = 3000;  // 3 seconds

// Serial
static const unsigned long BAUD = 115200;

// Screen modes
enum Mode { MODE_CPU = 0, MODE_GPU = 1, MODE_DISK = 2, MODE_WEATHER = 3 };
volatile Mode gMode = MODE_CPU;

// Latest stats from feeder
struct Stats {
  float cpu = 0, mem = 0, gpu = 0;
  float diskPct = 0, diskMBps = 0;
  float cpuTempF = -999, gpuTempF = -999;
  float freeC = -1, freeD = -1;
  float indoorTempF = -999;
  uint32_t lastUpdateMs = 0;  // millis() when last serial data received
} cur;

// History buffers for sparkline (60 samples)
static const int HIST_N = 60;
float histCPU[HIST_N] = {0};
float histGPU[HIST_N] = {0};
float histDISK[HIST_N] = {0};
int histIdx = 0;

// Bar animation
float barTarget = 0.0f; // 0..100
float barValue  = 0.0f; // 0..100 (displayed)
uint32_t lastAnim = 0;


// Canvas (Landscape rotation - M5Stack Core3)
static const int W = 320;
static const int H = 240;

// Colors
uint16_t bg     = TFT_BLACK;
uint16_t fg     = TFT_WHITE;
uint16_t accent = TFT_CYAN;

// Web server
WebServer server(80);
String ipText = "WiFi...";

// Forward decl
void setBarTargetFromMode();
void render();
void enterSetupMode();

// ------------------- Mode navigation -------------------
void nextMode() {
  gMode = (Mode)((gMode + 1) % 4);  // 4 modes now
}

void prevMode() {
  gMode = (Mode)((gMode + 3) % 4);  // (mode - 1 + 4) % 4
}

// ------------------- Touch swipe handling -------------------
void handleTouch() {
  M5.update();
  auto touch = M5.Touch.getDetail();

  // Long press detection
  if (touch.isPressed()) {
    if (touchStartTime == 0) {
      touchStartTime = millis();
      touchStartX = touch.x;
      touchActive = true;
    } else if (!longPressTriggered && millis() - touchStartTime > LONG_PRESS_MS) {
      longPressTriggered = true;
      enterSetupMode();
      return;
    }
  }

  if (touch.wasReleased()) {
    // Only handle swipe if not a long press
    if (!longPressTriggered && touchActive) {
      int deltaX = touch.x - touchStartX;
      if (deltaX > SWIPE_THRESHOLD) {
        prevMode();  // Swipe right = previous mode
        setBarTargetFromMode();
      } else if (deltaX < -SWIPE_THRESHOLD) {
        nextMode();  // Swipe left = next mode
        setBarTargetFromMode();
      }
    }
    touchActive = false;
    touchStartX = -1;
    touchStartTime = 0;
    longPressTriggered = false;
  }
}

// ------------------- Animation -------------------
void animateBar() {
  uint32_t now = millis();
  float dt = (now - lastAnim) / 1000.0f;
  if (dt < 0) dt = 0;
  if (dt > 0.05f) dt = 0.05f;
  lastAnim = now;

  const float speed = 7.0f; // easing speed
  barValue += (barTarget - barValue) * (1.0f - expf(-speed * dt));
}

// ------------------- Sparkline -------------------
void drawSparkline(int x, int y, int w, int h, const float *hist) {
  gfx.fillRect(x, y, w, h, bg);

  float mn = 1e9, mx = -1e9;
  for (int i = 0; i < HIST_N; ++i) {
    float v = hist[i];
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  if (mx <= mn) mx = mn + 1.0f;

  int px = x, py = y + h - 1;
  for (int i = 0; i < HIST_N; ++i) {
    int idx = (histIdx + i) % HIST_N;
    float v = hist[idx];
    float norm = (v - mn) / (mx - mn);  // 0..1
    int yy = y + h - 1 - int(norm * (h - 1));
    int xx = x + (i * (w - 1)) / (HIST_N - 1);
    if (i > 0) gfx.drawLine(px, py, xx, yy, accent);
    px = xx;
    py = yy;
  }
}

// ------------------- Formatting helpers -------------------
String fmtPct(float v) {
  if (v < 0) return "N/A";
  char b[16];
  snprintf(b, sizeof(b), "%.0f%%", v);
  return String(b);
}

String fmtTempF(float v) {
  if (v < -100) return "-";
  char b[16];
  // Free fonts only cover standard ASCII so we skip the degree symbol to avoid crashes.
  snprintf(b, sizeof(b), "%.0fF", v);
  return String(b);
}

String fmtMBps(float v) {
  char b[16];
  snprintf(b, sizeof(b), "%.1f MB/s", v);
  return String(b);
}

String fmtGB(float v) {
  if (v < 0) return "N/A";
  char b[16];
  snprintf(b, sizeof(b), "%.0f GB", v);
  return String(b);
}

// ------------------- Draw a frame into the sprite -------------------
void drawBar(const char *title, float valuePct, const char *valueText) {
  gfx.fillSprite(bg);

  // Title
  gfx.setTextColor(fg, bg);
  gfx.setTextDatum(TL_DATUM);
  gfx.setFreeFont(&FreeSansBold12pt7b);
  gfx.drawString(title, 10, 8);

  // IP status (bottom-left)
  gfx.setFreeFont(&FreeSans12pt7b);
  gfx.setTextDatum(BL_DATUM);
  gfx.drawString(ipText, 10, H - 2);

  // Main bar
  int barX = 10;
  int barY = 50;
  int barW = W - 20;
  int barH = 36;

  gfx.drawRect(barX, barY, barW, barH, fg);
  int fillW = int((valuePct / 100.0f) * (barW - 2));
  if (fillW < 0) fillW = 0;
  gfx.fillRect(barX + 1, barY + 1, fillW, barH - 2, accent);

  // Large value
  gfx.setFreeFont(&FreeSansBold18pt7b);
  gfx.setTextDatum(TR_DATUM);
  gfx.drawString(valueText, W - 10, 8);

  // Sparkline
  int spX = 10;
  int spW = W - 20;
  int spY = barY + barH + 10;
  int spH = 40;

  const float *hist =
      (gMode == MODE_CPU) ? histCPU :
      (gMode == MODE_GPU) ? histGPU :
      histDISK;

  drawSparkline(spX, spY, spW, spH, hist);

  // Push the entire sprite once (flicker-free)
  gfx.pushSprite(0, 0);
}

void render() {
  if (gMode == MODE_CPU) {
    String title = "CPU " + fmtPct(cur.cpu) + " | MEM " + fmtPct(cur.mem) +
                   " " + fmtTempF(cur.cpuTempF);
    drawBar(title.c_str(), barValue, fmtPct(cur.cpu).c_str());
  } else if (gMode == MODE_GPU) {
    String title = "GPU " + fmtPct(cur.gpu) + " | " + fmtTempF(cur.gpuTempF);
    drawBar(title.c_str(), barValue, fmtPct(cur.gpu).c_str());
  } else { // MODE_DISK
    String title = "DISK " + fmtPct(cur.diskPct) + " | " + fmtMBps(cur.diskMBps) +
                   " | C:" + fmtGB(cur.freeC) + " D:" + fmtGB(cur.freeD);
    drawBar(title.c_str(), barValue, fmtPct(cur.diskPct).c_str());
  }
}

void setBarTargetFromMode() {
  if (gMode == MODE_CPU)      barTarget = cur.cpu;
  else if (gMode == MODE_GPU) barTarget = cur.gpu;
  else if (gMode == MODE_DISK) barTarget = cur.diskPct;
  else barTarget = 0; // MODE_WEATHER does not use bar graph

  if (barTarget < 0)   barTarget = 0;
  if (barTarget > 100) barTarget = 100;
}

// ------------------- CSV parser -------------------
String serialBuf;

bool parseCSVLine(const String &line) {
  float vals[16] = {0};
  int idx = 0, start = 0;
  for (int i = 0; i <= line.length(); ++i) {
    if (i == line.length() || line[i] == ',') {
      if (idx < 16) {
        vals[idx] = line.substring(start, i).toFloat();
      }
      idx++;
      start = i + 1;
    }
  }
  if (idx < 9) return false;

  cur.cpu      = vals[0];
  cur.mem      = vals[1];
  cur.gpu      = vals[2];
  cur.diskPct  = vals[3];
  cur.diskMBps = vals[4];
  cur.cpuTempF = vals[5];
  cur.gpuTempF = vals[6];
  cur.freeC    = vals[7];
  cur.freeD    = vals[8];
  cur.indoorTempF = (idx >= 10) ? vals[9] : cur.cpuTempF;
  cur.lastUpdateMs = millis();
  return true;
}

inline void assignOrNull(JsonVariant target, float value, float invalidThreshold = -1000.0f) {
  if (isnan(value) || value <= invalidThreshold) {
    target.set(nullptr);
  } else {
    target.set(value);
  }
}

// ------------------- Web server -------------------
const char *PAGE_INDEX = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <title>ESP32 PC Stats</title>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <style>
    body { font-family: system-ui, sans-serif; background:#111; color:#eee; margin:0; }
    .wrap { max-width:960px; margin:0 auto; padding:1.5rem; }
    h1 { margin:0 0 0.4em 0; text-align:center; }
    .grid { display:flex; flex-wrap:wrap; justify-content:center; gap:1rem; margin-top:1rem; }
    .card { background:#1c1c1c; padding:1rem 1.5rem; border-radius:0.8rem; min-width:140px; box-shadow:0 12px 28px rgba(0,0,0,0.35); }
    .label { font-size:0.8rem; text-transform:uppercase; color:#aaa; letter-spacing:0.08em; }
    .value { font-size:1.4rem; margin-top:0.2rem; }
    .weather-section { margin-top:2rem; background:#181818; border-radius:1rem; padding:1.25rem; box-shadow:0 22px 40px rgba(0,0,0,0.35); }
    .weather-header { display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:0.5rem; }
    .weather-location { font-size:0.95rem; color:#aaa; }
    .weather-current { display:flex; justify-content:space-between; flex-wrap:wrap; gap:1.2rem; margin-top:1rem; }
    .current-temp { font-size:3rem; font-weight:600; }
    .current-desc { font-size:1.1rem; color:#ccc; }
    .current-meta { color:#aaa; margin-top:0.2rem; }
    .weather-extra { color:#888; font-size:0.9rem; align-self:flex-end; }
    .forecast-grid { display:flex; flex-wrap:wrap; gap:0.9rem; margin-top:1.2rem; }
    .forecast-card { flex:1 1 120px; background:#222; border-radius:0.8rem; padding:0.75rem; text-align:center; }
    .forecast-day { font-weight:600; margin-bottom:0.2rem; }
    .forecast-temp { font-size:1.2rem; }
    .forecast-desc { font-size:0.85rem; color:#bbb; margin-top:0.2rem; }
    @media (max-width:640px) {
      .weather-current { flex-direction:column; align-items:flex-start; }
      .card { min-width:125px; }
    }
  </style>
  <script>
    function formatNumber(value, decimals) {
      if (value === null || value === undefined || isNaN(value)) return '-';
      return Number(value).toFixed(decimals);
    }
    function formatTemp(value) {
      if (value === null || value === undefined || isNaN(value)) return '--';
      return Math.round(value) + '°F';
    }
    function formatPercent(value) {
      if (value === null || value === undefined || isNaN(value)) return '--';
      return Math.round(value) + '%';
    }
    function formatWind(value) {
      if (value === null || value === undefined || isNaN(value)) return '--';
      return Math.round(value) + ' mph';
    }
    function setText(id, text) {
      const el = document.getElementById(id);
      if (el) el.textContent = text;
    }
    function applyStats(data) {
      const keys = [
        ['cpu', 0], ['mem', 0], ['gpu', 0], ['diskPct', 0],
        ['diskMBps', 2], ['cpuTempF', 0], ['gpuTempF', 0],
        ['freeC', 0], ['freeD', 0]
      ];
      keys.forEach(([key, decimals]) => {
        const el = document.getElementById(key);
        if (!el) return;
        const val = data[key];
        el.textContent = (val === null || val === undefined || isNaN(val))
          ? '-'
          : Number(val).toFixed(decimals);
      });
    }
    function applyWeather(weather, forecast) {
      if (!weather) return;
      setText('weatherLocation', weather.location || 'Weather');
      setText('weatherTemp', formatTemp(weather.temperature));
      setText('weatherDesc', weather.description || '—');
      const hiLo = `${formatTemp(weather.tempMax)} / ${formatTemp(weather.tempMin)}`;
      const feels = formatTemp(weather.feelsLike);
      const humidity = formatPercent(weather.humidity);
      const wind = formatWind(weather.windSpeed);
      setText('weatherMeta', `High / Low ${hiLo} • Feels ${feels} • Hum ${humidity} • Wind ${wind}`);
      let updatedText = '—';
      if (weather.updated) {
        const offset = weather.timezoneOffset || 0;
        const dt = new Date((weather.updated + offset) * 1000);
        updatedText = dt.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
      }
      const status = weather.ok ? 'Updated' : 'Offline';
      setText('weatherExtra', `${status} @ ${updatedText}`);
      const slots = [null, null, null];
      if (Array.isArray(forecast)) {
        forecast.forEach(item => {
          if (!item) return;
          const slot = (typeof item.slot === 'number') ? item.slot : forecast.indexOf(item);
          if (slot >= 0 && slot < 3) {
            slots[slot] = item.valid ? item : null;
          }
        });
      }
      for (let i = 0; i < 3; i++) {
        const data = slots[i];
        setText(`forecast${i}Day`, data?.label || '--');
        setText(`forecast${i}Hi`, formatTemp(data?.high));
        setText(`forecast${i}Lo`, formatTemp(data?.low));
        setText(`forecast${i}Desc`, data?.description || '--');
      }
    }
    async function refresh() {
      try {
        const response = await fetch('/metrics');
        const json = await response.json();
        applyStats(json);
        applyWeather(json.weather, json.forecast);
        // Update data status indicator
        const statusEl = document.getElementById('dataStatus');
        if (statusEl) {
          const ageMs = json.dataAgeMs;
          const uptime = json.uptimeMs;
          if (ageMs < 0) {
            statusEl.textContent = 'No serial data received yet (uptime: ' + Math.floor(uptime/1000) + 's)';
            statusEl.style.color = '#f44';
          } else if (ageMs > 10000) {
            statusEl.textContent = 'Serial data stale: ' + Math.floor(ageMs/1000) + 's ago';
            statusEl.style.color = '#fa0';
          } else {
            statusEl.textContent = 'Serial data: ' + (ageMs < 1000 ? 'live' : Math.floor(ageMs/1000) + 's ago');
            statusEl.style.color = '#0f0';
          }
        }
      } catch (err) {
        console.error(err);
        const statusEl = document.getElementById('dataStatus');
        if (statusEl) {
          statusEl.textContent = 'Fetch error: ' + err.message;
          statusEl.style.color = '#f44';
        }
      }
    }
    setInterval(refresh, 2000);
    window.onload = refresh;
  </script>
</head>
<body>
  <main class="wrap">
    <h1>ESP32 PC Stats</h1>
    <div class="status-bar" style="text-align:center;margin-bottom:0.8rem;font-size:0.85rem;color:#888;">
      <span id="dataStatus">Waiting for data...</span>
    </div>
    <div class="grid">
      <div class="card"><div class="label">CPU %</div><div id="cpu" class="value">-</div></div>
      <div class="card"><div class="label">MEM %</div><div id="mem" class="value">-</div></div>
      <div class="card"><div class="label">GPU %</div><div id="gpu" class="value">-</div></div>
      <div class="card"><div class="label">Disk %</div><div id="diskPct" class="value">-</div></div>
      <div class="card"><div class="label">Disk MB/s</div><div id="diskMBps" class="value">-</div></div>
      <div class="card"><div class="label">CPU &deg;F</div><div id="cpuTempF" class="value">-</div></div>
      <div class="card"><div class="label">GPU &deg;F</div><div id="gpuTempF" class="value">-</div></div>
      <div class="card"><div class="label">Free C (GB)</div><div id="freeC" class="value">-</div></div>
      <div class="card"><div class="label">Free D (GB)</div><div id="freeD" class="value">-</div></div>
    </div>
    <section class="weather-section">
      <div class="weather-header">
        <h2>Weather</h2>
        <div class="weather-location" id="weatherLocation">Fetching...</div>
      </div>
      <div class="weather-current">
        <div>
          <div class="current-temp" id="weatherTemp">--</div>
          <div class="current-desc" id="weatherDesc">--</div>
          <div class="current-meta" id="weatherMeta">--</div>
        </div>
        <div class="weather-extra" id="weatherExtra">Waiting for data...</div>
      </div>
      <div class="forecast-grid">
        <div class="forecast-card">
          <div class="forecast-day" id="forecast0Day">--</div>
          <div class="forecast-temp"><span id="forecast0Hi">--</span> / <span id="forecast0Lo">--</span></div>
          <div class="forecast-desc" id="forecast0Desc">--</div>
        </div>
        <div class="forecast-card">
          <div class="forecast-day" id="forecast1Day">--</div>
          <div class="forecast-temp"><span id="forecast1Hi">--</span> / <span id="forecast1Lo">--</span></div>
          <div class="forecast-desc" id="forecast1Desc">--</div>
        </div>
        <div class="forecast-card">
          <div class="forecast-day" id="forecast2Day">--</div>
          <div class="forecast-temp"><span id="forecast2Hi">--</span> / <span id="forecast2Lo">--</span></div>
          <div class="forecast-desc" id="forecast2Desc">--</div>
        </div>
      </div>
    </section>
  </main>
</body>
</html>
)HTML";

void handleIndex() {
  server.send(200, "text/html", PAGE_INDEX);
}

void handleIP() {
  server.send(200, "text/plain", ipText);
}

void handleMetrics() {
  JsonDocument doc;
  doc["cpu"] = cur.cpu;
  doc["mem"] = cur.mem;
  doc["gpu"] = cur.gpu;
  doc["diskPct"] = cur.diskPct;
  doc["diskMBps"] = cur.diskMBps;
  if (isnan(cur.cpuTempF) || cur.cpuTempF < -100) doc["cpuTempF"] = nullptr; else doc["cpuTempF"] = cur.cpuTempF;
  if (isnan(cur.gpuTempF) || cur.gpuTempF < -100) doc["gpuTempF"] = nullptr; else doc["gpuTempF"] = cur.gpuTempF;
  if (isnan(cur.freeC) || cur.freeC < 0) doc["freeC"] = nullptr; else doc["freeC"] = cur.freeC;
  if (isnan(cur.freeD) || cur.freeD < 0) doc["freeD"] = nullptr; else doc["freeD"] = cur.freeD;

  // Include timestamp of last serial data for debugging
  doc["dataAgeMs"] = (cur.lastUpdateMs > 0) ? (millis() - cur.lastUpdateMs) : -1;
  doc["uptimeMs"] = millis();

  const WeatherData &w = display.getWeatherData();
  const WeatherDisplayState &ws = display.getDisplayState();
  JsonObject weather = doc["weather"].to<JsonObject>();
  weather["location"] = w.location;
  weather["description"] = w.description;
  weather["icon"] = w.icon;
  if (isnan(w.temperature)) weather["temperature"] = nullptr; else weather["temperature"] = w.temperature;
  if (isnan(w.feelsLike)) weather["feelsLike"] = nullptr; else weather["feelsLike"] = w.feelsLike;
  if (isnan(w.tempMin)) weather["tempMin"] = nullptr; else weather["tempMin"] = w.tempMin;
  if (isnan(w.tempMax)) weather["tempMax"] = nullptr; else weather["tempMax"] = w.tempMax;
  if (isnan(w.humidity) || w.humidity < 0) weather["humidity"] = nullptr; else weather["humidity"] = w.humidity;
  if (isnan(w.windSpeed) || w.windSpeed < 0) weather["windSpeed"] = nullptr; else weather["windSpeed"] = w.windSpeed;
  weather["updated"] = w.lastUpdateEpoch;
  weather["timezoneOffset"] = w.timezoneOffset;
  weather["ok"] = ws.lastFetchOk;
  weather["connected"] = ws.isConnected;

  JsonArray forecast = doc["forecast"].to<JsonArray>();
  for (int i = 0; i < 3; ++i) {
    JsonObject day = forecast.add<JsonObject>();
    day["slot"] = i;
    const WeatherForecast &f = w.forecast[i];
    day["valid"] = f.valid;
    if (!f.valid) continue;
    day["label"] = f.label;
    day["description"] = f.description;
    day["icon"] = f.icon;
    day["timestamp"] = f.timestamp;
    if (isnan(f.tempMax)) day["high"] = nullptr; else day["high"] = f.tempMax;
    if (isnan(f.tempMin)) day["low"] = nullptr; else day["low"] = f.tempMin;
  }

  String payload;
  serializeJson(doc, payload);
  server.send(200, "application/json", payload);
}

// ------------------- Enter Setup Mode -------------------
void enterSetupMode() {
  // Show entering setup message
  gfx.fillSprite(bg);
  gfx.setTextColor(TFT_YELLOW, bg);
  gfx.setTextDatum(MC_DATUM);
  gfx.setFreeFont(&FreeSansBold12pt7b);
  gfx.drawString("Entering Setup...", W / 2, H / 2);
  gfx.pushSprite(0, 0);
  delay(500);

  // Stop current web server if running
  server.stop();

  // Start captive portal
  configPortalStart();
}

// ------------------- WiFi connect -------------------
bool wifiConnect() {
  String ssid = getConfigWifiSSID();
  String pass = getConfigWifiPass();

  if (ssid.isEmpty()) {
    return false;  // No credentials, need setup
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  // Show a small connecting screen via sprite (no flicker)
  gfx.setTextColor(fg, bg);
  gfx.setTextDatum(MC_DATUM);
  gfx.setFreeFont(&FreeSansBold12pt7b);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    gfx.fillSprite(bg);
    gfx.drawString("Connecting WiFi...", W / 2, H / 2 - 24);
    gfx.setFreeFont(&FreeSans12pt7b);
    gfx.drawString(ssid, W / 2, H / 2 + 8);
    gfx.setTextColor(TFT_DARKGREY, bg);
    gfx.drawString(String("Status: ") + WiFi.status(), W / 2, H / 2 + 40);
    gfx.setTextColor(fg, bg);
    gfx.pushSprite(0, 0);
    delay(250);
    yield();
  }

  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    ipText = ip.toString();

    server.on("/", handleIndex);
    server.on("/metrics", handleMetrics);
    server.on("/ip", handleIP);
    server.begin();
    return true;
  } else {
    ipText = "WiFi: not connected";
    return false;
  }
}

// ------------------- Setup / Loop -------------------
void setup() {
  // Initialize M5Stack Core3
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1); // Landscape 320x240

  Serial.begin(BAUD);

  // Create full-screen sprite (~ 320*240*2B = ~154 KB)
  gfx.setColorDepth(16); // RGB565
  gfx.createSprite(W, H);

  lastAnim = millis();

  // Initial splash (sprite)
  gfx.fillSprite(bg);
  gfx.setTextColor(fg, bg);
  gfx.setTextDatum(MC_DATUM);
  gfx.setFreeFont(&FreeSansBold12pt7b);
  gfx.drawString("PC Monitor", W / 2, H / 2 - 10);
  gfx.pushSprite(0, 0);
  delay(400);

  // Initialize config portal and check for saved config
  configPortalInit();

  // Check if we have saved configuration
  if (!configPortalCheck()) {
    // No config - start setup portal
    Serial.println("No config found, starting setup portal...");
    configPortalStart();
    return;  // Loop will handle portal
  }

  // Try to connect to WiFi with saved credentials
  if (!wifiConnect()) {
    // WiFi failed - start setup portal
    Serial.println("WiFi connection failed, starting setup portal...");
    configPortalStart();
    return;  // Loop will handle portal
  }

  // Init weather subsystem AFTER WiFi is up
  weatherInit();

  // Start in CPU mode
  setBarTargetFromMode();
}

void loop() {
  // If in setup mode, handle portal only
  if (isInSetupMode()) {
    configPortalLoop();
    return;
  }

  // Serve HTTP if connected
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
  }

  // Touch swipe for mode navigation (includes long-press detection)
  handleTouch();

  // Serial CSV input (PC stats)
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      if (parseCSVLine(serialBuf)) {
        histCPU[histIdx]  = cur.cpu;
        histGPU[histIdx]  = cur.gpu;
        histDISK[histIdx] = cur.diskPct;
        histIdx = (histIdx + 1) % HIST_N;
        setBarTargetFromMode();
      }
      serialBuf = "";
    } else if (c != '\r') {
      serialBuf += c;
      if (serialBuf.length() > 200)
        serialBuf.remove(0, serialBuf.length() - 200);
    }
  }

  // Mode-specific drawing
  if (gMode == MODE_WEATHER) {
    // Hand off the display to the weather engine (includes weather update)
    weatherStep();
  } else {
    // ~30 FPS pacing for PC stats modes
    static uint32_t nextFrame = 0;
    uint32_t now = millis();
    if (now >= nextFrame) {
      nextFrame = now + 33; // ~30 FPS
      animateBar();
      render();
    }
    // Still update weather data in background for web portal
    weatherUpdateOnly();
  }
}

