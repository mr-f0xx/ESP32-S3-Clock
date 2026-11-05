#include "../arduino/08_LVGL_Clock/clock_layout.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace ClockLayout;

static void inside(const Rect &r, int16_t w, int16_t h, const char *name) {
  if (!r.inside(w, h)) {
    std::printf("FAIL %s (%d,%d %dx%d) outside %dx%d\n", name, r.x, r.y, r.w, r.h, w, h);
    assert(false);
  }
}
static void separated(const Rect &a, const Rect &b, const char *name) {
  if (a.overlaps(b)) {
    std::fprintf(stderr, "FAIL %s overlaps\n", name);
    assert(false);
  }
}
static Rect expanded(const Rect &r, int16_t amount) {
  return {static_cast<int16_t>(r.x - amount), static_cast<int16_t>(r.y - amount),
          static_cast<int16_t>(r.w + 2 * amount), static_cast<int16_t>(r.h + 2 * amount)};
}

static void testControl(int16_t w, int16_t h) {
  const Control l = control(w, h);
  const Rect all[] = {l.batteryIcon, l.battery, l.close, l.wifi, l.rotation,
    l.wifiIcon, l.wifiStatus, l.wifiIp, l.rotationIcon, l.rotationStatus,
    l.brightnessIcon, l.brightnessTitle, l.brightnessTrack, l.brightnessValue,
    l.volumeIcon, l.volumeTitle, l.volumeTrack, l.volumeValue, l.hint};
  for (const Rect &r : all) inside(r, w, h, "control");
  for (const Rect &r : l.settings) inside(r, w, h, "settings");
  for (int i = 0; i < 5; ++i)
    for (int j = i + 1; j < 5; ++j) separated(l.settings[i], l.settings[j], "settings grid");
  separated(l.batteryIcon, l.battery, "battery");
  separated(l.battery, l.close, "battery/close");
  separated(l.wifi, l.rotation, "connectivity");
  separated(l.wifiIcon, l.wifiStatus, "wifi icon/status");
  separated(l.wifiStatus, l.wifiIp, "wifi status/ip");
  separated(l.rotationIcon, l.rotationStatus, "rotation icon/status");
  const Rect brightness = expanded(l.brightnessTrack, l.knobOverhang);
  const Rect volume = expanded(l.volumeTrack, l.knobOverhang);
  inside(brightness, w, h, "brightness knob");
  inside(volume, w, h, "volume knob");
  separated(brightness, volume, "slider knobs");
  separated(brightness, l.brightnessTitle, "brightness title");
  separated(brightness, l.brightnessValue, "brightness value");
  separated(brightness, l.brightnessIcon, "brightness icon");
  separated(volume, l.volumeTitle, "volume title");
  separated(volume, l.volumeValue, "volume value");
  separated(volume, l.volumeIcon, "volume icon");
  separated(volume, l.hint, "volume/hint");
  for (const Rect &r : l.settings) {
    separated(r, l.wifi, "settings/wifi");
    separated(r, l.rotation, "settings/rotation");
  }
  separated(l.wifi, brightness, "wifi/brightness");
  separated(l.rotation, brightness, "rotation/brightness");
}

static void testTimer(int16_t w, int16_t h) {
  const Timer l = timer(w, h);
  const Rect all[] = {l.back, l.title, l.time, l.status, l.progress, l.minus,
    l.start, l.plus, l.reset, l.hint, l.alertTitle, l.alertTime, l.dismiss};
  for (const Rect &r : all) inside(r, w, h, "timer");
  for (const Rect &r : l.presets) inside(r, w, h, "preset");
  separated(l.back, l.title, "timer header");
  separated(l.time, l.status, "time/status");
  separated(l.status, l.progress, "status/progress");
  separated(l.progress, l.presets[0], "progress/presets");
  for (int i = 0; i < 4; ++i) {
    separated(l.presets[i], l.minus, "preset/minus");
    separated(l.presets[i], l.start, "preset/start");
    separated(l.presets[i], l.plus, "preset/plus");
    for (int j = i + 1; j < 4; ++j) separated(l.presets[i], l.presets[j], "presets");
  }
  separated(l.minus, l.start, "minus/start");
  separated(l.start, l.plus, "start/plus");
  separated(l.start, l.reset, "start/reset");
  separated(l.reset, l.hint, "reset/hint");
  separated(l.alertTitle, l.alertTime, "alert title/time");
  separated(l.alertTime, l.dismiss, "alert time/dismiss");
}

static void testWeather(int16_t w, int16_t h) {
  const Weather l = weather(w, h);
  const Rect all[] = {l.location, l.updated, l.current, l.forecastTitle,
    l.trendLocation, l.hourly, l.minutely};
  for (const Rect &r : all) inside(r, w, h, "weather");
  for (const Rect &r : l.forecast) inside(r, w, h, "forecast card");
  for (const Rect &r : l.dots) inside(r, w, h, "weather dot");
  separated(l.location, l.updated, "weather top-right");
  separated(l.current, l.forecastTitle, "current/forecast title");
  separated(l.forecastTitle, l.forecast[0], "forecast title/cards");
  separated(l.hourly, l.minutely, "trend cards");
  const Rect currentChildren[] = {l.icon, l.temperature, l.condition, l.divider,
    l.details[0], l.details[1], l.details[2], l.details[3]};
  for (const Rect &r : currentChildren) {
    assert(r.x >= 0 && r.y >= 0 &&
           r.right() <= l.current.w && r.bottom() <= l.current.h);
  }
  separated(l.temperature, l.divider, "temperature/divider");
  separated(l.condition, l.divider, "condition/divider");
  for (int i = 0; i < 4; ++i) {
    separated(l.divider, l.details[i], "divider/detail");
    for (int j = i + 1; j < 4; ++j) separated(l.details[i], l.details[j], "details");
  }
  for (int i = 0; i < 3; ++i) {
    const Rect children[] = {l.forecastDate[i], l.forecastIcon[i],
      l.forecastTemperature[i], l.forecastCondition[i]};
    for (const Rect &r : children)
      assert(r.x >= 0 && r.y >= 0 && r.right() <= l.forecast[i].w && r.bottom() <= l.forecast[i].h);
    separated(l.forecast[i], l.forecast[(i + 1) % 3], "forecast cards");
    separated(l.forecastDate[i], l.forecastIcon[i], "forecast date/icon");
    separated(l.forecastIcon[i], l.forecastTemperature[i], "forecast icon/temp");
    separated(l.forecastTemperature[i], l.forecastCondition[i], "forecast temp/condition");
  }
  const Rect hourlyChildren[] = {l.hourlyTitle, l.hourlyStatus, l.hourlyChart,
    l.hourlyY[0], l.hourlyY[1], l.hourlyY[2], l.hourlyX[0], l.hourlyX[1], l.hourlyX[2]};
  for (const Rect &r : hourlyChildren)
    assert(r.x >= 0 && r.y >= 0 && r.right() <= l.hourly.w && r.bottom() <= l.hourly.h);
  separated(l.hourlyTitle, l.hourlyStatus, "hourly title/status");
  separated(l.hourlyChart, l.hourlyX[0], "hourly chart/x");
  const Rect minutelyChildren[] = {l.minutelyTitle, l.minutelyStatus, l.minutelyChart,
    l.minutelyY[0], l.minutelyY[1], l.minutelyX[0], l.minutelyX[1], l.minutelyX[2]};
  for (const Rect &r : minutelyChildren)
    assert(r.x >= 0 && r.y >= 0 && r.right() <= l.minutely.w && r.bottom() <= l.minutely.h);
  separated(l.minutelyTitle, l.minutelyStatus, "minutely title/status");
  separated(l.minutelyStatus, l.minutelyChart, "minutely status/chart");
  separated(l.minutelyChart, l.minutelyX[0], "minutely chart/x");
}

int main() {
  assert(migrateTheme(0, false) == THEME_GLOW);
  for (uint8_t old = 1; old <= 6; ++old) assert(migrateTheme(old, false) == old);
  assert(migrateTheme(7, false) == THEME_GLOW);
  assert(migrateTheme(255, false) == THEME_GLOW);
  for (uint8_t current = 0; current < THEME_COUNT; ++current) assert(migrateTheme(current, true) == current);
  assert(migrateTheme(THEME_COUNT, true) == THEME_GLOW);
  assert(clockSizePixels(0) == 48 && clockSizePixels(1) == 72 && clockSizePixels(2) == 96);
  assert(normalizeClockSize(200) == CLOCK_SIZE_DEFAULT);
  const int16_t sizes[][2] = {{480, 480}, {600, 600}, {480, 360}, {360, 360}};
  for (const auto &size : sizes) {
    testControl(size[0], size[1]);
    testTimer(size[0], size[1]);
    testWeather(size[0], size[1]);
  }
  std::puts("layout: control/timer/weather bounds, separation, theme migration and clock sizes OK");
  return 0;
}
