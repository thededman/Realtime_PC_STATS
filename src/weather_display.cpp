#include "weather_display.h"

#include <algorithm>
#include <ctype.h>

#include <M5Unified.h>

#include "Free_Fonts.h"
#include "weather_icons.h"

extern LGFX_Sprite gfx;

namespace {

String titleCase(const char *src) {
  if (!src || !src[0]) return "n/a";
  String work(src);
  work.toLowerCase();
  bool capitalize = true;
  for (size_t i = 0; i < work.length(); ++i) {
    if (isalpha(work[i]) && capitalize) {
      work.setCharAt(i, toupper(work[i]));
      capitalize = false;
    } else if (work[i] == ' ') {
      capitalize = true;
    }
  }
  return work;
}

}  // namespace

WeatherDisplay::WeatherDisplay(ESP32Time &rtc) : rtc_(rtc) {}

void WeatherDisplay::begin() {
  scrollX_ = ANIMATION_START_POSITION;
  updateScrollingMessage();
  updateScrollingBuffer();
  initializeBrightnessControl();

  gfx.fillSprite(TFT_BLACK);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.setTextDatum(MC_DATUM);
  gfx.setFreeFont(&FreeSansBold12pt7b);
  gfx.drawString("Weather mode", WEATHER_SCREEN_WIDTH / 2, WEATHER_SCREEN_HEIGHT / 2 - 12);
  gfx.setFreeFont(&FreeSans12pt7b);
  gfx.drawString("Waiting for data...", WEATHER_SCREEN_WIDTH / 2, WEATHER_SCREEN_HEIGHT / 2 + 14);
  gfx.pushSprite(0, 0);
}

void WeatherDisplay::initializeBrightnessControl() {
  if (brightnessReady_) return;
  applyBrightness(state_.brightness);
  brightnessReady_ = true;
}

void WeatherDisplay::applyBrightness(uint8_t level) {
  uint8_t clamped =
      std::max(WEATHER_BRIGHTNESS_MIN, std::min(WEATHER_BRIGHTNESS_MAX, level));
  state_.brightness = clamped;
  M5.Display.setBrightness(clamped);
}

void WeatherDisplay::handleBrightnessButtons() {
  if (!brightnessReady_) return;
  if (WEATHER_BRIGHTNESS_BUTTON_UP < 0 && WEATHER_BRIGHTNESS_BUTTON_DOWN < 0) return;

  uint32_t now = millis();
  if (now - lastButtonSample_ < 150) return;
  lastButtonSample_ = now;

  bool changed = false;
  int level = state_.brightness;

  if (WEATHER_BRIGHTNESS_BUTTON_UP >= 0 &&
      digitalRead(WEATHER_BRIGHTNESS_BUTTON_UP) == LOW) {
    level += 8;
    changed = true;
  }
  if (WEATHER_BRIGHTNESS_BUTTON_DOWN >= 0 &&
      digitalRead(WEATHER_BRIGHTNESS_BUTTON_DOWN) == LOW) {
    level -= 8;
    changed = true;
  }

  if (changed) {
    level = std::max<int>(WEATHER_BRIGHTNESS_MIN,
                          std::min<int>(WEATHER_BRIGHTNESS_MAX, level));
    applyBrightness(level);
  }
}

WeatherData &WeatherDisplay::getWeatherData() { return data_; }
WeatherDisplayState &WeatherDisplay::getDisplayState() { return state_; }
int16_t &WeatherDisplay::getAni() { return scrollX_; }

void WeatherDisplay::updateLegacyData() {
  // Compatibility hook for the original sketch – nothing required here.
}

void WeatherDisplay::updateScrollingMessage() {
  String desc = titleCase(data_.description);
  String msg;
  msg.reserve(128);
  msg += (strlen(data_.location) ? data_.location : "Weather");
  msg += " | ";
  msg += desc;
  msg += " | Temp ";
  msg += formatTemp(data_.temperature);
  msg += " (";
  msg += formatTemp(data_.tempMin);
  msg += "/";
  msg += formatTemp(data_.tempMax);
  msg += ") | Hum ";
  if (isnan(data_.humidity)) {
    msg += "--%";
  } else {
    msg += String(lroundf(data_.humidity));
    msg += "%";
  }
  msg += " | Wind ";
  if (isnan(data_.windSpeed)) {
    msg += "-- mph";
  } else {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f mph", data_.windSpeed);
    msg += buf;
  }

  msg.toCharArray(data_.scrollingMessage, sizeof(data_.scrollingMessage));
}

void WeatherDisplay::updateScrollingBuffer() {
  if (data_.scrollingMessage[0] == '\0') {
    strncpy(data_.scrollingMessage, "Fetching data ...", sizeof(data_.scrollingMessage) - 1);
    data_.scrollingMessage[sizeof(data_.scrollingMessage) - 1] = '\0';
  }
  scrollBuffer_ = data_.scrollingMessage;
  ensureScrollMetrics();
}

void WeatherDisplay::ensureScrollMetrics() {
  if (scrollBuffer_.isEmpty()) {
    scrollPixelWidth_ = WEATHER_SCREEN_WIDTH;
    return;
  }
  gfx.setTextDatum(TL_DATUM);
  gfx.setFreeFont(&FreeSans12pt7b);
  scrollPixelWidth_ = gfx.textWidth(scrollBuffer_.c_str());
  if (scrollPixelWidth_ < WEATHER_SCREEN_WIDTH) {
    scrollPixelWidth_ = WEATHER_SCREEN_WIDTH;
  }
}

void WeatherDisplay::updateData() {
  if (scrollBuffer_.isEmpty()) return;
  scrollX_ -= WEATHER_SCROLL_STEP;
  if (scrollX_ <= -scrollPixelWidth_ - WEATHER_SCROLL_SPACING) {
    scrollX_ = WEATHER_SCREEN_WIDTH;
  }
}

String WeatherDisplay::formatTemp(float value) const {
  if (isnan(value)) return "--";
  char buf[16];
  snprintf(buf, sizeof(buf), "%.0fF", value);
  return String(buf);
}

const uint16_t *WeatherDisplay::iconForCode(const char *code) const {
  if (!code) return nullptr;
  if (strcmp(code, "01d") == 0) return icon_01d;
  if (strcmp(code, "01n") == 0) return icon_01n;
  if (strcmp(code, "02d") == 0) return icon_02d;
  if (strcmp(code, "02n") == 0) return icon_02n;
  if (strcmp(code, "03d") == 0) return icon_03d;
  if (strcmp(code, "03n") == 0) return icon_03n;
  if (strcmp(code, "04d") == 0) return icon_04d;
  if (strcmp(code, "04n") == 0) return icon_04n;
  if (strcmp(code, "09d") == 0) return icon_09d;
  if (strcmp(code, "09n") == 0) return icon_09n;
  if (strcmp(code, "10d") == 0) return icon_10d;
  if (strcmp(code, "10n") == 0) return icon_10n;
  if (strcmp(code, "11d") == 0) return icon_11d;
  if (strcmp(code, "11n") == 0) return icon_11n;
  if (strcmp(code, "13d") == 0) return icon_13d;
  if (strcmp(code, "13n") == 0) return icon_13n;
  if (strcmp(code, "50d") == 0) return icon_50d;
  if (strcmp(code, "50n") == 0) return icon_50n;
  return nullptr;
}

void WeatherDisplay::drawTicker() {
  if (scrollBuffer_.isEmpty()) return;
  gfx.fillRect(0, WEATHER_SCREEN_HEIGHT - 28, WEATHER_SCREEN_WIDTH, 28, TFT_DARKGREY);
  gfx.setTextColor(TFT_WHITE, TFT_DARKGREY);
  gfx.setTextDatum(TL_DATUM);
  gfx.setFreeFont(&FreeSans12pt7b);
  gfx.drawString(scrollBuffer_, scrollX_, WEATHER_SCREEN_HEIGHT - 24);
  int16_t secondX = scrollX_ + scrollPixelWidth_ + WEATHER_SCROLL_SPACING;
  if (secondX < WEATHER_SCREEN_WIDTH) {
    gfx.drawString(scrollBuffer_, secondX, WEATHER_SCREEN_HEIGHT - 24);
  }
}

void WeatherDisplay::draw() {
  gfx.fillSprite(TFT_BLACK);

  // Header row (location + time)
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(TFT_CYAN, TFT_BLACK);
  gfx.setFreeFont(&FreeSansBold12pt7b);
  const char *loc = strlen(data_.location) ? data_.location : "Weather";
  gfx.drawString(loc, 8, 6);

  struct tm timeinfo = rtc_.getTimeStruct();
  char timeBuf[16] = "--:--";
  strftime(timeBuf, sizeof(timeBuf), "%H:%M", &timeinfo);
  gfx.setTextDatum(TR_DATUM);
  gfx.drawString(timeBuf, WEATHER_SCREEN_WIDTH - 8, 6);

  // Temperature block
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.setFreeFont(&FreeSansBold18pt7b);
  gfx.drawString(formatTemp(data_.temperature), 8, 34);

  gfx.setFreeFont(&FreeSans12pt7b);
  gfx.drawString(String("Feels ") + formatTemp(data_.feelsLike), 8, 74);

  String desc = titleCase(data_.description);
  gfx.drawString(desc, 8, 98);

  // Icon on the right
  if (const uint16_t *icon = iconForCode(data_.icon)) {
    gfx.pushImage(WEATHER_SCREEN_WIDTH - WEATHER_ICON_WIDTH - 10,
                  32,
                  WEATHER_ICON_WIDTH,
                  WEATHER_ICON_HEIGHT,
                  const_cast<uint16_t *>(icon));
  }

  // Detail rows
  int detailY = 118;
  char buf[48];

  if (!isnan(data_.humidity)) {
    snprintf(buf, sizeof(buf), "Humidity %d%%", (int)lroundf(data_.humidity));
    gfx.drawString(buf, 8, detailY);
    detailY += 20;
  }

  if (!isnan(data_.windSpeed)) {
    snprintf(buf, sizeof(buf), "Wind %.1f mph", data_.windSpeed);
    gfx.drawString(buf, 8, detailY);
    detailY += 20;
  }

  if (!isnan(data_.pressure)) {
    snprintf(buf, sizeof(buf), "Pressure %.0f hPa", data_.pressure);
    gfx.drawString(buf, 8, detailY);
  }

  // Connection badge
  gfx.setTextDatum(TR_DATUM);
  gfx.setTextColor(state_.lastFetchOk ? TFT_GREEN : TFT_RED, TFT_BLACK);
  gfx.drawString(state_.lastFetchOk ? "Updated" : "Offline", WEATHER_SCREEN_WIDTH - 8,
                 WEATHER_SCREEN_HEIGHT - 40);

  drawTicker();
  gfx.pushSprite(0, 0);
}
