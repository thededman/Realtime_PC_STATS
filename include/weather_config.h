#pragma once

#include <Arduino.h>

// Period between automatic weather refreshes.
constexpr uint32_t UPDATE_INTERVAL_MS = 5UL * 60UL * 1000UL;

// How many successful updates before we re-sync NTP time.
constexpr uint8_t SYNC_INTERVAL_UPDATES = 6;

// Sprite width/height match the PC dashboard canvas (M5Stack Core3: 320x240).
constexpr int WEATHER_SCREEN_WIDTH = 320;
constexpr int WEATHER_SCREEN_HEIGHT = 240;

// Scrolling text animation
constexpr int16_t ANIMATION_START_POSITION = WEATHER_SCREEN_WIDTH;
constexpr uint8_t WEATHER_SCROLL_STEP = 2;
constexpr uint16_t WEATHER_SCROLL_SPACING = 80;

// Brightness control (M5Unified handles backlight internally)
constexpr uint8_t WEATHER_DEFAULT_BRIGHTNESS = 200;
constexpr uint8_t WEATHER_BRIGHTNESS_MIN = 32;
constexpr uint8_t WEATHER_BRIGHTNESS_MAX = 255;

// Touch brightness buttons disabled (can implement swipe up/down if desired)
constexpr int WEATHER_BRIGHTNESS_BUTTON_UP = -1;
constexpr int WEATHER_BRIGHTNESS_BUTTON_DOWN = -1;

// NTP servers used when syncing time.
static const char NTP_SERVER_1[] = "pool.ntp.org";
static const char NTP_SERVER_2[] = "time.nist.gov";
static const char NTP_SERVER_3[] = "time.google.com";
