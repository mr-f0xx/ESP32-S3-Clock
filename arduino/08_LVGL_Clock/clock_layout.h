#pragma once
#include <stdint.h>

// Screen-independent geometry shared by the firmware and native layout tests.
// Design coordinates use a 480 px canvas; scale() derives actual rectangles from
// the configured LCD dimensions rather than the logical rotated resolution.
namespace ClockLayout {
struct Rect {
  int16_t x, y, w, h;
  constexpr int16_t right() const { return x + w; }
  constexpr int16_t bottom() const { return y + h; }
  constexpr bool inside(int16_t width, int16_t height) const {
    return x >= 0 && y >= 0 && w > 0 && h > 0 && right() <= width && bottom() <= height;
  }
  constexpr bool overlaps(const Rect &other) const {
    return x < other.right() && other.x < right() &&
           y < other.bottom() && other.y < bottom();
  }
};
constexpr int16_t scale(int16_t value, int16_t extent) {
  return static_cast<int16_t>((static_cast<int32_t>(value) * extent + 239) / 480);
}
constexpr Rect rect(int16_t x, int16_t y, int16_t w, int16_t h,
                    int16_t width, int16_t height) {
  return {scale(x, width), scale(y, height), scale(w, width), scale(h, height)};
}

// Version 2 removes the old AMOLED Black entry and moves Icon Glow to ID 0.
// The other six themes kept their historical IDs, so only the retired default
// and the old Glow ID need remapping.
constexpr uint8_t THEME_COUNT = 7;
constexpr uint8_t THEME_GLOW = 0;
constexpr uint8_t migrateTheme(uint8_t stored, bool version2) {
  if (version2) return stored < THEME_COUNT ? stored : THEME_GLOW;
  if (stored == 0 || stored == 7 || stored > 7) return THEME_GLOW;
  return stored;
}
constexpr uint8_t CLOCK_SIZE_COUNT = 3;
constexpr uint8_t CLOCK_SIZE_DEFAULT = 2;
constexpr uint8_t normalizeClockSize(uint8_t size) {
  return size < CLOCK_SIZE_COUNT ? size : CLOCK_SIZE_DEFAULT;
}
constexpr uint8_t clockSizePixels(uint8_t size) {
  return normalizeClockSize(size) == 0 ? 48 : (normalizeClockSize(size) == 1 ? 72 : 96);
}

struct Control {
  Rect batteryIcon, battery, close, settings[5], wifi, rotation;
  Rect wifiIcon, wifiStatus, wifiIp, rotationIcon, rotationStatus;
  Rect brightnessIcon, brightnessTitle, brightnessTrack, brightnessValue;
  Rect volumeIcon, volumeTitle, volumeTrack, volumeValue, hint;
  int16_t knobOverhang;
};
inline Control control(int16_t width, int16_t height) {
  Control l{};
  auto r = [&](int16_t x, int16_t y, int16_t w, int16_t h) {
    return rect(x, y, w, h, width, height);
  };
  l.batteryIcon = r(20, 10, 56, 56);
  l.battery = r(96, 18, 120, 40);
  l.close = r(416, 16, 44, 44);
  for (uint8_t i = 0; i < 5; ++i)
    l.settings[i] = r(20 + (i % 3) * 150, 72 + (i / 3) * 52, 140, 44);
  l.wifi = r(20, 184, 214, 104);
  l.rotation = r(246, 184, 214, 104);
  l.wifiIcon = r(32, 206, 56, 56);
  l.wifiStatus = r(104, 202, 120, 28);
  l.wifiIp = r(104, 234, 120, 24);
  l.rotationIcon = r(258, 206, 56, 56);
  l.rotationStatus = r(330, 212, 120, 44);
  l.brightnessIcon = r(20, 322, 48, 48);
  l.brightnessTitle = r(88, 300, 292, 22);
  l.brightnessTrack = r(88, 340, 292, 12);
  l.brightnessValue = r(394, 326, 66, 40);
  l.volumeIcon = r(20, 400, 48, 48);
  l.volumeTitle = r(88, 378, 292, 22);
  l.volumeTrack = r(88, 418, 292, 12);
  l.volumeValue = r(394, 404, 66, 40);
  l.hint = r(20, 452, 440, 22);
  l.knobOverhang = scale(10, width);
  return l;
}

struct Timer {
  Rect back, title, time, status, progress, presets[4], minus, start, plus, reset, hint;
  Rect alertTitle, alertTime, dismiss;
};
inline Timer timer(int16_t width, int16_t height) {
  Timer l{};
  auto r = [&](int16_t x, int16_t y, int16_t w, int16_t h) {
    return rect(x, y, w, h, width, height);
  };
  l.back = r(20, 20, 48, 48);
  l.title = r(84, 26, 312, 36);
  l.time = r(24, 90, 432, 108);
  l.status = r(40, 204, 400, 28);
  l.progress = r(40, 236, 400, 3);
  for (uint8_t i = 0; i < 4; ++i) l.presets[i] = r(24 + i * 110, 248, 102, 48);
  l.minus = r(24, 316, 88, 60);
  l.start = r(124, 316, 232, 60);
  l.plus = r(368, 316, 88, 60);
  l.reset = r(124, 390, 232, 48);
  l.hint = r(24, 450, 432, 24);
  l.alertTitle = r(32, 112, 416, 48);
  l.alertTime = r(32, 180, 416, 80);
  l.dismiss = r(100, 292, 280, 64);
  return l;
}

struct Weather {
  Rect location, updated, current, icon, temperature, condition, divider, details[4];
  Rect forecastTitle, forecast[3], forecastDate[3], forecastIcon[3],
       forecastTemperature[3], forecastCondition[3], dots[2];
  Rect trendLocation, hourly, hourlyTitle, hourlyStatus, hourlyChart,
       hourlyY[3], hourlyX[3], minutely, minutelyTitle, minutelyStatus,
       minutelyChart, minutelyY[2], minutelyX[3];
};
inline Weather weather(int16_t width, int16_t height) {
  Weather l{};
  auto r = [&](int16_t x, int16_t y, int16_t w, int16_t h) {
    return rect(x, y, w, h, width, height);
  };
  l.location = r(24, 8, 260, 44);
  l.updated = r(296, 8, 160, 44);
  l.current = r(24, 64, 432, 184);
  l.icon = r(16, 16, 64, 64);
  l.temperature = r(84, 24, 124, 64);
  l.condition = r(12, 98, 200, 70);
  l.divider = r(220, 16, 1, 152);
  for (uint8_t i = 0; i < 4; ++i) l.details[i] = r(232, 12 + i * 42, 188, 38);
  l.forecastTitle = r(24, 260, 432, 26);
  for (uint8_t i = 0; i < 3; ++i) {
    l.forecast[i] = r(24 + i * 148, 294, 136, 160);
    l.forecastDate[i] = r(4, 8, 128, 26);
    l.forecastIcon[i] = r(40, 38, 56, 56);
    l.forecastTemperature[i] = r(4, 94, 128, 24);
    l.forecastCondition[i] = r(6, 122, 124, 34);
  }
  for (uint8_t i = 0; i < 2; ++i) l.dots[i] = r(218 + i * 25, 464, 18, 4);
  l.trendLocation = r(24, 8, 432, 44);
  l.hourly = r(24, 64, 432, 190);
  l.hourlyTitle = r(12, 8, 230, 38);
  l.hourlyStatus = r(254, 8, 166, 38);
  l.hourlyChart = r(62, 56, 354, 100);
  const int16_t hy[] = {52, 94, 136};
  const int16_t hx[] = {62, 184, 356};
  const int16_t hw[] = {60, 110, 60};
  for (uint8_t i = 0; i < 3; ++i) {
    l.hourlyY[i] = r(4, hy[i], 52, 24);
    l.hourlyX[i] = r(hx[i], 162, hw[i], 24);
  }
  l.minutely = r(24, 266, 432, 188);
  l.minutelyTitle = r(12, 8, 408, 26);
  l.minutelyStatus = r(12, 36, 408, 26);
  l.minutelyChart = r(68, 72, 348, 80);
  l.minutelyY[0] = r(4, 68, 58, 24);
  l.minutelyY[1] = r(4, 132, 58, 24);
  const int16_t mx[] = {68, 187, 356};
  const int16_t mw[] = {60, 110, 60};
  for (uint8_t i = 0; i < 3; ++i) l.minutelyX[i] = r(mx[i], 160, mw[i], 24);
  return l;
}
}  // namespace ClockLayout
