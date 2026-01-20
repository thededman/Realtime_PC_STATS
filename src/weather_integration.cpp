#include <Arduino.h>
#include "weather_integration.h"

// Global objects (mirroring original weather-micro-station sketch)
ESP32Time rtc(0);
Preferences preferences;
WeatherDisplay display(rtc);   // Pass rtc to display
WeatherAPI apiClient(rtc);     // Pass rtc to API client

// Animation and timing variables
static unsigned long timePased = 0;

void weatherInit() {
  Serial.println("Weather subsystem starting...");

  // We assume WiFi is already connected by the main sketch.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Weather: WiFi not connected, weather mode will not update.");
  } else {
    Serial.printf("Weather: WiFi OK, IP=%s RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    display.getDisplayState().isConnected = true;
  }

  // Initialize display
  display.begin();

  // Initialize preferences for secure storage
  preferences.begin("weather", false);

  // Set up brightness control using on-board buttons
  display.initializeBrightnessControl();

  // Initial time synchronization and data fetch
  apiClient.setTime();
  Serial.println("Weather: initial API call...");

  // Clear existing scrolling message and reset animation for startup
  display.getAni() = ANIMATION_START_POSITION;
  strcpy(display.getWeatherData().scrollingMessage, "Fetching data ...");
  display.updateScrollingBuffer();
  delay(2000);

  if (apiClient.getData(display.getWeatherData(), display.getDisplayState())) {
    display.updateLegacyData();
    display.updateScrollingMessage();
    display.getAni() = ANIMATION_START_POSITION;
    display.updateScrollingBuffer();
    Serial.println("Weather: initial API call OK");
  } else {
    Serial.println("Weather: initial API call FAILED");
  }

  // Start periodic timer (UPDATE_INTERVAL_MS is defined in config.h)
  timePased = millis();
}

// Internal helper to fetch weather data (shared by weatherStep and weatherUpdateOnly)
static bool doWeatherFetch() {
  display.getDisplayState().updateCounter++;
  Serial.printf("Weather: timer fired, fetching API at %lu ms\n", millis());

  bool apiSuccess = apiClient.getData(display.getWeatherData(),
                                      display.getDisplayState());
  if (apiSuccess) {
    display.updateLegacyData();
    display.updateScrollingMessage();
    Serial.println("Weather: API call OK");
  } else {
    Serial.println("Weather: API call failed");
  }

  // Periodic time synchronization
  if (display.getDisplayState().updateCounter >= SYNC_INTERVAL_UPDATES) {
    apiClient.setTime();  // Sync time every few updates
    display.getDisplayState().updateCounter = 0;
  }

  return apiSuccess;
}

void weatherStep() {
  // This is a lightly refactored version of the original loop()
  // from weather-micro-station/src/main.cpp.

  static unsigned long lastDisplayUpdate = 0;
  static unsigned long lastMemoryCheck   = 0;
  static int           loopCounter       = 0;

  unsigned long currentMillis = millis();

  // Update display at 40Hz (25ms interval) for smooth animation
  if (currentMillis - lastDisplayUpdate >= 25) {
    // Update animation and scrolling
    display.updateData();

    // Check if it's time for a data update (every UPDATE_INTERVAL_MS)
    if (millis() > timePased + UPDATE_INTERVAL_MS) {
      timePased = millis();

      // Clear existing scrolling message and reset animation
      display.getAni() = ANIMATION_START_POSITION;
      strcpy(display.getWeatherData().scrollingMessage, "Fetching data ...");
      display.updateScrollingBuffer();
      delay(2000);

      // Fetch new weather data
      if (doWeatherFetch()) {
        display.getAni() = ANIMATION_START_POSITION;
        display.updateScrollingBuffer();
      }
    }

    // Draw the display
    display.draw();
    lastDisplayUpdate = currentMillis;
  }

  // Handle brightness control buttons (non-blocking)
  display.handleBrightnessButtons();

  // Memory monitoring (every 30 seconds)
  loopCounter++;
  if (currentMillis - lastMemoryCheck >= 30000) {
    lastMemoryCheck = currentMillis;
    Serial.printf("Weather: free heap=%d bytes, loops=%d\n",
                  ESP.getFreeHeap(), loopCounter);
    loopCounter = 0;
  }

  // Small yield to prevent watchdog triggers and allow other tasks
  yield();
}

void weatherUpdateOnly() {
  // Lightweight weather update for when NOT in weather display mode.
  // Only fetches API data on interval - no display updates.
  if (millis() > timePased + UPDATE_INTERVAL_MS) {
    timePased = millis();
    doWeatherFetch();
  }
}
