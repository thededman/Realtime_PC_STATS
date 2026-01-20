#pragma once

#include <Arduino.h>
#include <ESP32Time.h>
#include <math.h>

#include "weather_config.h"

struct WeatherDisplayState {
  bool isConnected = false;
  uint8_t brightness = WEATHER_DEFAULT_BRIGHTNESS;
  uint32_t updateCounter = 0;
  bool lastFetchOk = false;
};

struct WeatherForecast {
  uint32_t timestamp = 0;
  float tempMin = NAN;
  float tempMax = NAN;
  char description[48] = "";
  char icon[4] = "01d";
  char label[12] = "";
  bool valid = false;
};

struct WeatherData {
  char location[32] = "";
  char description[64] = "";
  char scrollingMessage[256] = "";
  char icon[4] = "01d";
  float temperature = NAN;
  float feelsLike = NAN;
  float humidity = NAN;
  float windSpeed = NAN;
  float pressure = NAN;
  float tempMin = NAN;
  float tempMax = NAN;
  uint32_t lastUpdateEpoch = 0;
  int32_t timezoneOffset = 0;
  WeatherForecast forecast[3];
};

class WeatherDisplay {
 public:
  explicit WeatherDisplay(ESP32Time &rtc);

  void begin();
  void initializeBrightnessControl();
  void handleBrightnessButtons();
  void updateData();
  void draw();

  WeatherData &getWeatherData();
  WeatherDisplayState &getDisplayState();
  int16_t &getAni();

  void updateLegacyData();
  void updateScrollingMessage();
  void updateScrollingBuffer();

 private:
  void applyBrightness(uint8_t level);
  void ensureScrollMetrics();
  const uint16_t *iconForCode(const char *code) const;
  String formatTemp(float value) const;
  void drawTicker();

  ESP32Time &rtc_;
  WeatherData data_{};
  WeatherDisplayState state_{};
  int16_t scrollX_ = ANIMATION_START_POSITION;
  uint16_t scrollPixelWidth_ = 0;
  String scrollBuffer_;
  bool brightnessReady_ = false;
  uint32_t lastButtonSample_ = 0;
};
