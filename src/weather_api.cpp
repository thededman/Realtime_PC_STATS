#include "weather_api.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <algorithm>
#include <math.h>
#include <time.h>
#include <limits.h>

#include <ArduinoJson.h>

#include "config_portal.h"
#include "weather_config.h"

// Build API URL dynamically from portal config
static String buildWeatherUrl() {
  String url = "https://api.openweathermap.org/data/2.5/weather?q=";
  url += getConfigCity();
  url += "&appid=";
  url += getConfigApiKey();
  url += "&units=";
  url += getConfigUnits();
  return url;
}

static String buildForecastUrl() {
  String url = "https://api.openweathermap.org/data/2.5/forecast?q=";
  url += getConfigCity();
  url += "&appid=";
  url += getConfigApiKey();
  url += "&units=";
  url += getConfigUnits();
  return url;
}

namespace {

constexpr uint32_t kSecondsPerDay = 86400;
constexpr uint32_t kMiddaySeconds = 12 * 3600;

struct ForecastBucket {
  bool used = false;
  float tempMin = 1e6f;
  float tempMax = -1e6f;
  uint32_t representativeTs = 0;
  uint32_t bestDelta = UINT32_MAX;
  char description[48] = "";
  char icon[4] = "01d";
};

void resetForecast(WeatherData &data) {
  for (auto &entry : data.forecast) {
    entry.timestamp = 0;
    entry.tempMin = NAN;
    entry.tempMax = NAN;
    entry.description[0] = '\0';
    strlcpy(entry.icon, "01d", sizeof(entry.icon));
    entry.label[0] = '\0';
    entry.valid = false;
  }
}

void formatDayLabel(uint32_t epoch, int32_t tzOffset, bool isToday, char *out, size_t len) {
  if (isToday) {
    strlcpy(out, "Today", len);
    return;
  }
  time_t localTs = static_cast<time_t>(epoch) + tzOffset;
  struct tm info;
  gmtime_r(&localTs, &info);
  strftime(out, len, "%a", &info);
}

}  // namespace

WeatherAPI::WeatherAPI(ESP32Time &rtc) : rtc_(rtc) {
  client_.setInsecure();
}

void WeatherAPI::setTime() {
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    rtc_.setTimeStruct(timeinfo);
  }
}

bool WeatherAPI::getData(WeatherData &data, WeatherDisplayState &state) {
  HTTPClient http;
  String url = buildWeatherUrl();
  if (!http.begin(client_, url)) {
    state.lastFetchOk = false;
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    state.lastFetchOk = false;
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  auto err = deserializeJson(doc, payload);
  if (err) {
    state.lastFetchOk = false;
    return false;
  }

  JsonObject main = doc["main"];
  data.temperature = main["temp"] | NAN;
  data.feelsLike = main["feels_like"] | NAN;
  data.tempMin = main["temp_min"] | NAN;
  data.tempMax = main["temp_max"] | NAN;
  data.pressure = main["pressure"] | NAN;
  data.humidity = main["humidity"] | NAN;

  JsonObject wind = doc["wind"];
  data.windSpeed = wind["speed"] | NAN;

  const char* cityFallback = getConfigCity().c_str();
  strlcpy(data.location,
          doc["name"] | cityFallback,
          sizeof(data.location));
  JsonObject weather0 = doc["weather"][0];
  strlcpy(data.description,
          weather0["description"] | "n/a",
          sizeof(data.description));
  strlcpy(data.icon,
          weather0["icon"] | "01d",
          sizeof(data.icon));

  data.lastUpdateEpoch = doc["dt"] | 0;
  data.timezoneOffset = doc["timezone"] | data.timezoneOffset;
  if (data.lastUpdateEpoch) {
    rtc_.setTime(data.lastUpdateEpoch);
  }

  resetForecast(data);
  bool forecastOk = fetchForecast(data);

  state.lastFetchOk = true;
  state.isConnected = (WiFi.status() == WL_CONNECTED);

  if (!forecastOk) {
    Serial.println("Weather: forecast fetch failed, continuing with current data.");
  }
  return true;
}

bool WeatherAPI::fetchForecast(WeatherData &data) {
  HTTPClient http;
  String url = buildForecastUrl();
  if (!http.begin(client_, url)) {
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  auto err = deserializeJson(doc, payload);
  if (err) {
    return false;
  }

  JsonArray entries = doc["list"].as<JsonArray>();
  if (entries.isNull() || entries.size() == 0) {
    return false;
  }

  int32_t tzOffset = doc["city"]["timezone"] | data.timezoneOffset;
  data.timezoneOffset = tzOffset;
  int32_t baseDay = (data.lastUpdateEpoch + data.timezoneOffset) / kSecondsPerDay;
  if (baseDay <= 0) {
    uint32_t firstTs = entries[0]["dt"] | 0;
    baseDay = (firstTs + data.timezoneOffset) / kSecondsPerDay;
  }

  ForecastBucket buckets[3];

  for (JsonObject entry : entries) {
    uint32_t ts = entry["dt"] | 0;
    if (!ts) continue;

    int32_t localDay = static_cast<int32_t>((ts + data.timezoneOffset) / kSecondsPerDay);
    int idx = localDay - baseDay;
    if (idx < 0 || idx >= 3) continue;

    ForecastBucket &bucket = buckets[idx];
    bucket.used = true;

    float tempMin = entry["main"]["temp_min"] | NAN;
    float tempMax = entry["main"]["temp_max"] | NAN;
    if (!isnan(tempMin)) bucket.tempMin = (bucket.tempMin == 1e6f) ? tempMin : min(bucket.tempMin, tempMin);
    if (!isnan(tempMax)) bucket.tempMax = (bucket.tempMax == -1e6f) ? tempMax : max(bucket.tempMax, tempMax);

    uint32_t localSeconds = static_cast<uint32_t>((ts + data.timezoneOffset) % kSecondsPerDay);
    uint32_t delta = (localSeconds > kMiddaySeconds)
                         ? (localSeconds - kMiddaySeconds)
                         : (kMiddaySeconds - localSeconds);
    if (delta < bucket.bestDelta) {
      bucket.bestDelta = delta;
      bucket.representativeTs = ts;
      JsonObject w = entry["weather"][0];
      strlcpy(bucket.description, w["description"] | "n/a", sizeof(bucket.description));
      strlcpy(bucket.icon, w["icon"] | "01d", sizeof(bucket.icon));
    }
  }

  bool any = false;
  for (int i = 0; i < 3; ++i) {
    WeatherForecast &out = data.forecast[i];
    const ForecastBucket &bucket = buckets[i];
    if (!bucket.used) {
      out.valid = false;
      continue;
    }
    any = true;
    out.valid = true;
    out.timestamp = bucket.representativeTs;
    out.tempMin = (bucket.tempMin == 1e6f) ? NAN : bucket.tempMin;
    out.tempMax = (bucket.tempMax == -1e6f) ? NAN : bucket.tempMax;
    strlcpy(out.description, bucket.description, sizeof(out.description));
    strlcpy(out.icon, bucket.icon, sizeof(out.icon));

    uint32_t labelTs = bucket.representativeTs;
    if (!labelTs) {
      labelTs = (baseDay + i) * kSecondsPerDay;
    }
    formatDayLabel(labelTs, data.timezoneOffset, i == 0, out.label, sizeof(out.label));
  }

  return any;
}
