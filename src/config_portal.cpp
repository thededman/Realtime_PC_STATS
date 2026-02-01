#include "config_portal.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <M5Unified.h>
#include "Free_Fonts.h"

// External sprite for display
extern LGFX_Sprite gfx;

// Portal configuration
static const char* AP_SSID = "PCMonitor-Setup";
static const char* AP_PASS = "";  // Open network for easy access
static const IPAddress AP_IP(172, 16, 1, 1);
static const IPAddress AP_GATEWAY(172, 16, 1, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);
static const byte DNS_PORT = 53;

// NVS namespace and keys
static const char* NVS_NAMESPACE = "pcmonitor";
static const char* KEY_CONFIGURED = "configured";
static const char* KEY_WIFI_SSID = "wifi_ssid";
static const char* KEY_WIFI_PASS = "wifi_pass";
static const char* KEY_API_KEY = "owm_key";
static const char* KEY_CITY = "owm_city";
static const char* KEY_UNITS = "owm_units";

// Portal state
static Preferences prefs;
static WebServer portalServer(80);
static DNSServer dnsServer;
static bool setupMode = false;

// Cached config values
static String cachedSSID;
static String cachedPass;
static String cachedApiKey;
static String cachedCity;
static String cachedUnits;

// HTML page for configuration
static const char PORTAL_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>PC Monitor Setup</title>
  <style>
    * { box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
      color: #eee;
      margin: 0;
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 20px;
    }
    .container {
      background: rgba(255,255,255,0.05);
      border-radius: 16px;
      padding: 30px;
      max-width: 400px;
      width: 100%;
      box-shadow: 0 8px 32px rgba(0,0,0,0.3);
    }
    h1 {
      margin: 0 0 8px 0;
      font-size: 24px;
      text-align: center;
    }
    .subtitle {
      text-align: center;
      color: #888;
      margin-bottom: 24px;
      font-size: 14px;
    }
    .section {
      margin-bottom: 20px;
    }
    .section-title {
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: 1px;
      color: #0af;
      margin-bottom: 12px;
      border-bottom: 1px solid #333;
      padding-bottom: 6px;
    }
    label {
      display: block;
      margin-bottom: 6px;
      font-size: 14px;
      color: #aaa;
    }
    input, select {
      width: 100%;
      padding: 12px;
      border: 1px solid #333;
      border-radius: 8px;
      background: #111;
      color: #fff;
      font-size: 16px;
      margin-bottom: 12px;
    }
    input:focus, select:focus {
      outline: none;
      border-color: #0af;
    }
    button {
      width: 100%;
      padding: 14px;
      background: linear-gradient(135deg, #0af 0%, #08f 100%);
      border: none;
      border-radius: 8px;
      color: #fff;
      font-size: 16px;
      font-weight: 600;
      cursor: pointer;
      margin-top: 10px;
    }
    button:hover {
      opacity: 0.9;
    }
    .info {
      font-size: 12px;
      color: #666;
      margin-top: 16px;
      text-align: center;
    }
    .info a {
      color: #0af;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>PC Monitor Setup</h1>
    <p class="subtitle">Configure your M5Stack device</p>
    <form action="/save" method="POST">
      <div class="section">
        <div class="section-title">WiFi Network</div>
        <label>SSID (Network Name)</label>
        <input type="text" name="ssid" placeholder="Your WiFi name" required>
        <label>Password</label>
        <input type="password" name="pass" placeholder="WiFi password">
      </div>
      <div class="section">
        <div class="section-title">Weather Settings</div>
        <label>OpenWeatherMap API Key</label>
        <input type="text" name="apikey" placeholder="Your API key" required>
        <label>City Name</label>
        <input type="text" name="city" placeholder="e.g., New York" required>
        <label>Temperature Units</label>
        <select name="units">
          <option value="imperial">Fahrenheit</option>
          <option value="metric">Celsius</option>
        </select>
      </div>
      <button type="submit">Save & Connect</button>
    </form>
    <p class="info">
      Get a free API key at <a href="https://openweathermap.org/api" target="_blank">openweathermap.org</a>
    </p>
  </div>
</body>
</html>
)HTML";

static const char SAVE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Saved!</title>
  <style>
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
      color: #eee;
      margin: 0;
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      text-align: center;
      padding: 20px;
    }
    .container {
      background: rgba(255,255,255,0.05);
      border-radius: 16px;
      padding: 40px;
      max-width: 400px;
    }
    h1 { color: #0f0; margin-bottom: 16px; }
    p { color: #aaa; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Settings Saved!</h1>
    <p>Your device will now restart and connect to your WiFi network.</p>
    <p>This may take a few seconds...</p>
  </div>
</body>
</html>
)HTML";

// Load cached config from NVS
static void loadConfig() {
  prefs.begin(NVS_NAMESPACE, true);  // read-only
  cachedSSID = prefs.getString(KEY_WIFI_SSID, "");
  cachedPass = prefs.getString(KEY_WIFI_PASS, "");
  cachedApiKey = prefs.getString(KEY_API_KEY, "");
  cachedCity = prefs.getString(KEY_CITY, "");
  cachedUnits = prefs.getString(KEY_UNITS, "imperial");
  prefs.end();
}

// Handle root request - serve config form
static void handleRoot() {
  portalServer.send(200, "text/html", PORTAL_HTML);
}

// Handle save request - store config and reboot
static void handleSave() {
  String ssid = portalServer.arg("ssid");
  String pass = portalServer.arg("pass");
  String apikey = portalServer.arg("apikey");
  String city = portalServer.arg("city");
  String units = portalServer.arg("units");

  // Validate required fields
  if (ssid.isEmpty() || apikey.isEmpty() || city.isEmpty()) {
    portalServer.send(400, "text/plain", "Missing required fields");
    return;
  }

  // Save to NVS
  prefs.begin(NVS_NAMESPACE, false);  // read-write
  prefs.putString(KEY_WIFI_SSID, ssid);
  prefs.putString(KEY_WIFI_PASS, pass);
  prefs.putString(KEY_API_KEY, apikey);
  prefs.putString(KEY_CITY, city);
  prefs.putString(KEY_UNITS, units);
  prefs.putBool(KEY_CONFIGURED, true);
  prefs.end();

  // Send success page
  portalServer.send(200, "text/html", SAVE_HTML);

  // Update display
  gfx.fillSprite(TFT_BLACK);
  gfx.setTextColor(TFT_GREEN, TFT_BLACK);
  gfx.setTextDatum(MC_DATUM);
  gfx.setFreeFont(&FreeSansBold12pt7b);
  gfx.drawString("Settings Saved!", 160, 100);
  gfx.setFreeFont(&FreeSans12pt7b);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.drawString("Restarting...", 160, 140);
  gfx.pushSprite(0, 0);

  // Wait for response to be sent, then restart
  delay(2000);
  ESP.restart();
}

// Handle captive portal redirect
static void handleNotFound() {
  portalServer.sendHeader("Location", String("http://") + AP_IP.toString(), true);
  portalServer.send(302, "text/plain", "");
}

void configPortalInit() {
  loadConfig();
}

bool configPortalCheck() {
  prefs.begin(NVS_NAMESPACE, true);
  bool configured = prefs.getBool(KEY_CONFIGURED, false);
  prefs.end();
  return configured && !cachedSSID.isEmpty();
}

void configPortalStart() {
  setupMode = true;

  // Display setup screen
  displaySetupScreen();

  // Configure AP
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(AP_SSID, AP_PASS);

  // Start DNS server for captive portal
  dnsServer.start(DNS_PORT, "*", AP_IP);

  // Configure web server routes
  portalServer.on("/", HTTP_GET, handleRoot);
  portalServer.on("/save", HTTP_POST, handleSave);
  portalServer.on("/generate_204", handleRoot);  // Android captive portal
  portalServer.on("/fwlink", handleRoot);        // Microsoft captive portal
  portalServer.onNotFound(handleNotFound);

  portalServer.begin();

  Serial.println("Portal started at 192.168.4.1");
}

void configPortalLoop() {
  if (!setupMode) return;
  dnsServer.processNextRequest();
  portalServer.handleClient();
}

void configPortalStop() {
  if (!setupMode) return;
  portalServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  setupMode = false;
}

String getConfigWifiSSID() {
  return cachedSSID;
}

String getConfigWifiPass() {
  return cachedPass;
}

String getConfigApiKey() {
  return cachedApiKey;
}

String getConfigCity() {
  return cachedCity;
}

String getConfigUnits() {
  return cachedUnits;
}

bool isInSetupMode() {
  return setupMode;
}

void displaySetupScreen() {
  gfx.fillSprite(TFT_BLACK);
  gfx.setTextDatum(MC_DATUM);

  // Title
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  gfx.setFreeFont(&FreeSansBold18pt7b);
  gfx.drawString("WiFi Setup", 160, 40);

  // Instructions
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.setFreeFont(&FreeSans12pt7b);
  gfx.drawString("Connect to WiFi:", 160, 90);

  // AP name
  gfx.setTextColor(TFT_GREEN, TFT_BLACK);
  gfx.setFreeFont(&FreeSansBold12pt7b);
  gfx.drawString(AP_SSID, 160, 120);

  // Browser instructions
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.setFreeFont(&FreeSans12pt7b);
  gfx.drawString("Then open browser to:", 160, 160);

  // IP address
  gfx.setTextColor(TFT_YELLOW, TFT_BLACK);
  gfx.setFreeFont(&FreeSansBold12pt7b);
  gfx.drawString("192.168.4.1", 160, 190);

  gfx.pushSprite(0, 0);
}
