#pragma once

#include <ESP32Time.h>
#include <Preferences.h>
#include <WiFi.h>

#include "weather_config.h"
#include "weather_display.h"
#include "weather_api.h"
#include "secrets.h"

// These globals are separated into a tiny "engine" that can be
// driven from another sketch (PC stats + weather combo).

extern ESP32Time rtc;
extern Preferences preferences;
extern WeatherDisplay display;
extern WeatherAPI apiClient;

// Call once from setup(), *after* WiFi is connected.
void weatherInit();

// Call repeatedly from loop() while you are in weather mode.
void weatherStep();

// Call from loop() when NOT in weather mode to still update weather data
// for the web portal (no display updates, just API fetch on interval).
void weatherUpdateOnly();
