#pragma once

#include <Arduino.h>
#include <ESP32Time.h>
#include <WiFiClientSecure.h>

#include "weather_config.h"
#include "weather_display.h"

class WeatherAPI {
 public:
  explicit WeatherAPI(ESP32Time &rtc);

  // Sync RTC with NTP
  void setTime();

  // Populate WeatherData/State by calling OpenWeather.
  bool getData(WeatherData &data, WeatherDisplayState &state);

 private:
  ESP32Time &rtc_;
  WiFiClientSecure client_;
  bool fetchForecast(WeatherData &data);
};
