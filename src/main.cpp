#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>

// T-Display-S3 PC Monitor Dashboard + WiFi Web Server (Sprite Double-Buffered, ~30 FPS)
// - Modes: CPU, GPU, DISK (cycle with right/left buttons)
// - Smooth bar animations + 60-sample sparkline
// - Parses CSV from feeder GUI (USB serial) at 115200:
//   cpu,mem,gpu,diskPct,diskMBps,cpuTempF,gpuTempF,freeC_GB,freeD_GB\n
// - WiFi server:
//     GET /         -> live HTML dashboard (auto-refresh via JS)
//     GET /metrics  -> JSON {cpu, mem, gpu, diskPct, diskMBps, cpuTempF, gpuTempF, freeC, freeD}
//     GET /ip       -> plain text IP

// ---------------------- WiFi CONFIG ----------------------
// <<< EDIT THESE >>>
const char *WIFI_SSID = "YourWifiName";
const char *WIFI_PASS = "YourWifiPassword";
// ---------------------------------------------------------

// TFT and full-screen sprite for flicker-free rendering
TFT_eSPI tft;
TFT_eSprite gfx = TFT_eSprite(&tft);  // off-screen framebuffer (RGB565)

// Pins / IO
static const int PIN_LCD_POWER = 15;
static const int BTN_BACK = 0;  // left/back (active LOW)
static const int BTN_NEXT = 14; // right/next (active LOW)

// Serial
static const unsigned long BAUD = 115200;

// Screen modes
enum Mode
{
    MODE_CPU = 0,
    MODE_GPU = 1,
    MODE_DISK = 2
};
volatile Mode gMode = MODE_CPU;

// Latest stats from feeder
struct Stats
{
    float cpu = 0, mem = 0, gpu = 0;
    float diskPct = 0, diskMBps = 0;
    float cpuTempF = -999, gpuTempF = -999;
    float freeC = -1, freeD = -1;
} cur;

// History buffers for sparkline (60 samples)
static const int HIST_N = 60;
float histCPU[HIST_N] = {0};
float histGPU[HIST_N] = {0};
float histDISK[HIST_N] = {0};
int histIdx = 0;

// Bar animation
float barTarget = 0.0f; // 0..100
float barValue = 0.0f;  // 0..100 (displayed)
uint32_t lastAnim = 0;

// Debounce
uint32_t lastBtnTime = 0;

// Canvas (Landscape rotation)
static const int W = 320;
static const int H = 170;

// Colors
uint16_t bg = TFT_BLACK;
uint16_t fg = TFT_WHITE;
uint16_t accent = TFT_CYAN;

// Web server
WebServer server(80);
String ipText = "WiFi.";

// Forward decl
void setBarTargetFromMode();
void render();

// ------------------- Button helpers -------------------
bool buttonPressed(int pin)
{
    if (digitalRead(pin) == LOW)
    {
        uint32_t now = millis();
        if (now - lastBtnTime > 220)
        {
            lastBtnTime = now;
            return true;
        }
    }
    return false;
}
void nextMode()
{
    gMode = (Mode)((gMode + 1) % 3);
}
void prevMode()
{
    gMode = (Mode)((gMode + 2) % 3);
}

// ------------------- Animation -------------------
void animateBar()
{
    uint32_t now = millis();
    float dt = (now - lastAnim) / 1000.0f;
    if (dt < 0)
        dt = 0;
    if (dt > 0.05f)
        dt = 0.05f;
    lastAnim = now;

    const float speed = 7.0f; // easing speed
    barValue += (barTarget - barValue) * (1.0f - expf(-speed * dt));
}

// ------------------- Sparkline -------------------
void drawSparkline(int x, int y, int w, int h, const float *hist)
{
    gfx.fillRect(x, y, w, h, bg);

    float mn = 1e9, mx = -1e9;
    for (int i = 0; i < HIST_N; ++i)
    {
        float v = hist[i];
        if (v < mn)
            mn = v;
        if (v > mx)
            mx = v;
    }
    if (mx <= mn)
        mx = mn + 1.0f;

    int px = x, py = y + h - 1;
    for (int i = 0; i < HIST_N; ++i)
    {
        int idx = (histIdx + i) % HIST_N;
        float v = hist[idx];
        float norm = (v - mn) / (mx - mn); // 0..1
        int yy = y + h - 1 - int(norm * (h - 1));
        int xx = x + (i * (w - 1)) / (HIST_N - 1);
        if (i > 0)
            gfx.drawLine(px, py, xx, yy, accent);
        px = xx;
        py = yy;
    }
}

// ------------------- Formatting -------------------
String fmtPct(float v)
{
    if (v < 0)
        return "N/A";
    char b[16];
    snprintf(b, sizeof(b), "%.0f%%", v);
    return String(b);
}
String fmtTempF(float v)
{
    if (v < -100)
        return "-";
    char b[16];
    // Free fonts only cover standard ASCII so we skip the degree symbol to avoid crashes.
    snprintf(b, sizeof(b), "%.0fF", v);
    return String(b);
}
String fmtMBps(float v)
{
    char b[16];
    snprintf(b, sizeof(b), "%.1f MB/s", v);
    return String(b);
}
String fmtGB(float v)
{
    if (v < 0)
        return "N/A";
    char b[16];
    snprintf(b, sizeof(b), "%.0f GB", v);
    return String(b);
}

// ------------------- Draw a frame into the sprite -------------------
void drawBar(const char *title, float valuePct, const char *valueText)
{
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
    if (fillW < 0)
        fillW = 0;
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

    const float *hist = (gMode == MODE_CPU) ? histCPU : (gMode == MODE_GPU) ? histGPU
                                                                            : histDISK;
    drawSparkline(spX, spY, spW, spH, hist);

    // Push the entire sprite once (flicker-free)
    gfx.pushSprite(0, 0);
}

void render()
{
    if (gMode == MODE_CPU)
    {
        String title = "CPU  " + fmtPct(cur.cpu) + "   |   MEM " + fmtPct(cur.mem) + "   " + fmtTempF(cur.cpuTempF);
        drawBar(title.c_str(), barValue, fmtPct(cur.cpu).c_str());
    }
    else if (gMode == MODE_GPU)
    {
        String title = "GPU  " + fmtPct(cur.gpu) + "   |   " + fmtTempF(cur.gpuTempF);
        drawBar(title.c_str(), barValue, fmtPct(cur.gpu).c_str());
    }
    else
    {
        String title = "DISK  " + fmtPct(cur.diskPct) + "   |   " + fmtMBps(cur.diskMBps) + "   |   C:" + fmtGB(cur.freeC) + "  D:" + fmtGB(cur.freeD);
        drawBar(title.c_str(), barValue, fmtPct(cur.diskPct).c_str());
    }
}

void setBarTargetFromMode()
{
    if (gMode == MODE_CPU)
        barTarget = cur.cpu;
    else if (gMode == MODE_GPU)
        barTarget = cur.gpu;
    else
        barTarget = cur.diskPct;
    if (barTarget < 0)
        barTarget = 0;
    if (barTarget > 100)
        barTarget = 100;
}

// ------------------- CSV parser -------------------
String serialBuf;

bool parseCSVLine(const String &line)
{
    float vals[9] = {0};
    int idx = 0, start = 0;
    for (int i = 0; i <= line.length(); ++i)
    {
        if (i == line.length() || line[i] == ',')
        {
            if (idx < 9)
                vals[idx++] = line.substring(start, i).toFloat();
            start = i + 1;
        }
    }
    if (idx < 9)
        return false;
    cur.cpu = vals[0];
    cur.mem = vals[1];
    cur.gpu = vals[2];
    cur.diskPct = vals[3];
    cur.diskMBps = vals[4];
    cur.cpuTempF = vals[5];
    cur.gpuTempF = vals[6];
    cur.freeC = vals[7];
    cur.freeD = vals[8];
    return true;
}

// ------------------- Web server -------------------
const char *PAGE_INDEX =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>ESP32 PC Stats</title>"
    "<style>"
    "body{font-family:system-ui,Segoe UI,Arial;padding:16px;background:#111;color:#eee}"
    ".row{display:flex;gap:16px;flex-wrap:wrap}"
    ".card{background:#1a1a1a;border-radius:12px;padding:16px;min-width:260px;flex:1}"
    ".title{font-weight:600;margin:0 0 8px 0}"
    ".big{font-size:32px;margin:4px 0 0 0}"
    ".mono{font-family:ui-monospace,Consolas,monospace;color:#9ae6b4}"
    "small{opacity:.7}"
    "</style>"
    "</head><body>"
    "<h2>ESP32 PC Stats</h2>"
    "<div id='ip'><small>IP: ...</small></div>"
    "<div class='row'>"
    "  <div class='card'><div class='title'>CPU</div><div class='big'><span id='cpu'>..</span> <small>| MEM <span id='mem'>..</span></small></div>"
    "      <div><small>CPU Temp: <span id='cpuT'>..</span></small></div></div>"
    "  <div class='card'><div class='title'>GPU</div><div class='big'><span id='gpu'>..</span></div>"
    "      <div><small>GPU Temp: <span id='gpuT'>..</span></small></div></div>"
    "  <div class='card'><div class='title'>DISK</div><div class='big'><span id='disk'>..</span></div>"
    "      <div><small>Throughput: <span id='mbps'>..</span></small></div>"
    "      <div><small>Free: C <span class='mono' id='c'>..</span> / D <span class='mono' id='d'>..</span></small></div></div>"
    "</div>"
    "<script>"
    "async function tick(){"
    "  try{"
    "    const j=await (await fetch('/metrics',{cache:'no-store'})).json();"
    "    cpu.textContent = j.cpu.toFixed(0)+'%';"
    "    mem.textContent = j.mem.toFixed(0)+'%';"
    "    cpuT.textContent = (j.cpuTempF<-100?'-':Math.round(j.cpuTempF)+'\\u00B0F');"
    "    gpu.textContent = j.gpu.toFixed(0)+'%';"
    "    gpuT.textContent = (j.gpuTempF<-100?'-':Math.round(j.gpuTempF)+'\\u00B0F');"
    "    disk.textContent = j.diskPct.toFixed(0)+'%';"
    "    mbps.textContent = j.diskMBps.toFixed(1)+' MB/s';"
    "    c.textContent = (j.freeC<0?'N/A':Math.round(j.freeC)+' GB');"
    "    d.textContent = (j.freeD<0?'N/A':Math.round(j.freeD)+' GB');"
    "  }catch(e){}"
    "}"
    "async function ip(){"
    "  try{ const t=await (await fetch('/ip',{cache:'no-store'})).text();"
    "       document.getElementById('ip').innerHTML='<small>IP: '+t+'</small>'; }catch(e){}"
    "}"
    "tick(); ip(); setInterval(tick,1000); setInterval(ip,5000);"
    "</script>"
    "</body></html>";

void handleIndex()
{
    server.send(200, "text/html", PAGE_INDEX);
}
void handleIP()
{
    server.send(200, "text/plain", ipText);
}
void handleMetrics()
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"cpu\":%.1f,\"mem\":%.1f,\"gpu\":%.1f,\"diskPct\":%.1f,"
             "\"diskMBps\":%.2f,\"cpuTempF\":%.1f,\"gpuTempF\":%.1f,"
             "\"freeC\":%.1f,\"freeD\":%.1f}",
             cur.cpu, cur.mem, cur.gpu, cur.diskPct,
             cur.diskMBps, cur.cpuTempF, cur.gpuTempF,
             cur.freeC, cur.freeD);
    server.send(200, "application/json", buf);
}

// ------------------- WiFi connect -------------------
void wifiConnect()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Show a small connecting screen via sprite (no flicker)
    gfx.setTextColor(fg, bg);
    gfx.setTextDatum(MC_DATUM);
    gfx.setFreeFont(&FreeSansBold12pt7b);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 12000)
    {
        gfx.fillSprite(bg);
        gfx.drawString("Connecting WiFi...", W / 2, H / 2 - 12);
        gfx.setFreeFont(&FreeSans12pt7b);
        gfx.drawString(String("Status: ") + WiFi.status(), W / 2, H / 2 + 16);
        gfx.pushSprite(0, 0);
        delay(250);
        yield();
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        IPAddress ip = WiFi.localIP();
        ipText = ip.toString();
        server.on("/", handleIndex);
        server.on("/metrics", handleMetrics);
        server.on("/ip", handleIP);
        server.begin();
    }
    else
    {
        ipText = "WiFi: not connected";
    }
}

// ------------------- Setup / Loop -------------------
void setup()
{
    pinMode(PIN_LCD_POWER, OUTPUT);
    digitalWrite(PIN_LCD_POWER, HIGH);

    pinMode(BTN_BACK, INPUT_PULLUP);
    pinMode(BTN_NEXT, INPUT_PULLUP);

    Serial.begin(BAUD);

    tft.init();
    tft.setRotation(1); // 320x170
    // Create full-screen sprite (~ 320*170*2B = ~109 KB)
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

    // WiFi connect & start web server
    wifiConnect();

    // Start in CPU mode
    setBarTargetFromMode();
}

void loop()
{
    // Serve HTTP
    if (WiFi.status() == WL_CONNECTED)
    {
        server.handleClient();
    }

    // Buttons
    if (buttonPressed(BTN_NEXT))
    {
        nextMode();
        setBarTargetFromMode();
    }
    if (buttonPressed(BTN_BACK))
    {
        prevMode();
        setBarTargetFromMode();
    }

    // Serial CSV input
    while (Serial.available())
    {
        char c = (char)Serial.read();
        if (c == '\n')
        {
            if (parseCSVLine(serialBuf))
            {
                histCPU[histIdx] = cur.cpu;
                histGPU[histIdx] = cur.gpu;
                histDISK[histIdx] = cur.diskPct;
                histIdx = (histIdx + 1) % HIST_N;
                setBarTargetFromMode();
            }
            serialBuf = "";
        }
        else if (c != '\r')
        {
            serialBuf += c;
            if (serialBuf.length() > 200)
                serialBuf.remove(0, serialBuf.length() - 200);
        }
    }

    // ~30 FPS pacing
    static uint32_t nextFrame = 0;
    uint32_t now = millis();
    if (now >= nextFrame)
    {
        nextFrame = now + 33; // ~30 FPS
        animateBar();
        render();
    }
}
