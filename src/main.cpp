#include <Arduino.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <math.h>

#if __has_include("local_config.h")
#include "local_config.h"
#else
#error "Create include/local_config.h from include/local_config.example.h before building"
#endif

#ifndef FNOS_REDUCED_MOTION
#define FNOS_REDUCED_MOTION 0
#endif

namespace {

constexpr int16_t SCREEN_W = 170;
constexpr int16_t SCREEN_H = 320;
constexpr uint8_t PAGE_COUNT = 7;
constexpr uint8_t HISTORY_COUNT = 14;
constexpr uint32_t REFRESH_MS = 1000;
constexpr uint32_t STALE_AFTER_MS = REFRESH_MS * 3;
constexpr uint32_t OFFLINE_AFTER_MS = REFRESH_MS * 10;
constexpr uint16_t HISTORY_WINDOW_SECONDS = HISTORY_COUNT * REFRESH_MS / 1000;
constexpr uint32_t FRAME_INTERVAL_US = 16667;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 18;
constexpr uint8_t BACKLIGHT_PERCENT = 50;
constexpr uint8_t BACKLIGHT_PWM_CHANNEL = 7;
constexpr uint8_t BACKLIGHT_PWM_BITS = 8;
constexpr uint16_t BACKLIGHT_PWM_HZ = 12000;
constexpr uint16_t BACKLIGHT_FADE_MS = 120;
constexpr const char *UI_BUILD_ID = "render-v3.0-seven-page";
constexpr float SPRING_RESPONSE = 0.28f;
constexpr float SPRING_OMEGA = 2.0f * PI / SPRING_RESPONSE;

namespace ui {
constexpr int16_t edge = 8;
constexpr int16_t content_width = SCREEN_W - edge * 2;
constexpr int16_t panel_radius = 7;
constexpr int16_t header_bottom = 26;
constexpr int16_t state_y = 30;
constexpr int16_t state_h = 32;
constexpr int16_t hero_label_y = 74;
constexpr int16_t hero_value_y = 84;
constexpr int16_t hero_unit_y = 111;
constexpr int16_t trend_y = 134;
constexpr int16_t trend_h = 63;
constexpr int16_t tiles_y = 207;
constexpr int16_t summary_y = 266;
constexpr int16_t footer_y = 294;
constexpr int16_t footer_indicator_y = 298;
}  // namespace ui

enum class Page : uint8_t { Overview, Network, Storage, System, Normal, HighLoad, Timeout };
enum class DataTrust : uint8_t { Warming, Live, Stale, Offline };

struct Palette {
  uint16_t background, panel, panel_alt, text, dim, idle, guide;
  uint16_t cyan, blue, lime, green, amber, orange, red;
};

struct NasState {
  bool online = false;
  uint8_t consecutive_failures = 0;
  int cpu = -1;
  int memory = -1;
  int gpu = -1;
  int api_rtt_ms = -1;
  int cpu_temp_c = -1;
  int disk_temp_c = -1;
  int lan_speed_mbps = -1;
  float used_tb = NAN;
  float total_tb = NAN;
  float memory_used_gb = NAN;
  float load_1m = NAN;
  float disk_read_mb_s = NAN;
  float down_mbps = NAN;
  float up_mbps = NAN;
  int uptime_days = -1;
  uint32_t last_good_ms = 0;
  String hostname = "fnOS NAS";
  String raid_status;
  String error;
};

struct DebouncedButton {
  uint8_t pin;
  bool raw = HIGH;
  bool stable = HIGH;
  uint32_t changed_at = 0;

  explicit DebouncedButton(uint8_t input_pin) : pin(input_pin) {}
};

TFT_eSPI tft;
TFT_eSprite frame_a(&tft);
TFT_eSprite frame_b(&tft);
Palette color;
NasState state;
NasState collector_state;
NasState pending_state;
DebouncedButton previous_button{BUTTON_1};
DebouncedButton next_button{BUTTON_2};

SemaphoreHandle_t telemetry_mutex = nullptr;
TaskHandle_t telemetry_task = nullptr;
volatile bool telemetry_ready = false;

void announceTelemetry();

bool frame_a_ready = false;
bool frame_b_ready = false;
int cached_a_page = -1000;
int cached_b_page = -1000;
float page_position = 0.0f;
float page_target = 0.0f;
float page_velocity = 0.0f;
uint32_t last_frame_us = 0;
bool screen_dirty = true;
bool telemetry_redraw_pending = false;

float down_history[HISTORY_COUNT]{};
float up_history[HISTORY_COUNT]{};
float latency_history[HISTORY_COUNT]{};
float cpu_history[HISTORY_COUNT]{};
float temp_history[HISTORY_COUNT]{};

const char *const PAGE_NAMES[PAGE_COUNT] = {
    "OVERVIEW", "NETWORK", "STORAGE", "SYSTEM", "NORMAL", "HIGH LOAD", "TIMEOUT"};
const uint8_t TIMEOUT_CURVE[HISTORY_COUNT] = {
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42};

int pageModulo(int value) {
  int result = value % PAGE_COUNT;
  return result < 0 ? result + PAGE_COUNT : result;
}

uint16_t dimColor(uint16_t value, uint8_t percent);

DataTrust dataTrust() {
  if (state.last_good_ms == 0)
    return state.consecutive_failures < 3 ? DataTrust::Warming : DataTrust::Offline;
  const uint32_t age_ms = millis() - state.last_good_ms;
  if (state.online && age_ms <= STALE_AFTER_MS) return DataTrust::Live;
  if (age_ms <= OFFLINE_AFTER_MS) return DataTrust::Stale;
  return DataTrust::Offline;
}

bool realtimeValid() { return dataTrust() == DataTrust::Live; }

bool telemetryReadable() {
  const DataTrust trust = dataTrust();
  return trust == DataTrust::Live || trust == DataTrust::Stale;
}

String trustLabel() {
  switch (dataTrust()) {
    case DataTrust::Warming: return "WARMING";
    case DataTrust::Live: return "LIVE";
    case DataTrust::Stale:
      return "STALE " + String((millis() - state.last_good_ms) / 1000) + "S";
    case DataTrust::Offline: return "OFFLINE";
  }
  return "NO DATA";
}

uint16_t trustColor() {
  switch (dataTrust()) {
    case DataTrust::Warming: return color.dim;
    case DataTrust::Live: return color.green;
    case DataTrust::Stale: return color.amber;
    case DataTrust::Offline: return color.red;
  }
  return color.dim;
}

uint16_t readingColor(uint16_t live_color) {
  if (dataTrust() == DataTrust::Stale) return dimColor(live_color, 52);
  if (dataTrust() == DataTrust::Warming || dataTrust() == DataTrust::Offline) return color.idle;
  return live_color;
}

int storagePercent() {
  if (!telemetryReadable()) return -1;
  if (!isfinite(state.used_tb) || !isfinite(state.total_tb) || state.total_tb <= 0.0f) return -1;
  return constrain(static_cast<int>(roundf(state.used_tb / state.total_tb * 100.0f)), 0, 100);
}

String floatValue(float value, uint8_t decimals = 1) {
  return telemetryReadable() && isfinite(value)
             ? String(value, static_cast<unsigned int>(decimals)) : String("--.-");
}

String percentValue(int value) {
  return telemetryReadable() && value >= 0 ? String(value) + "%" : String("--%");
}
String intValue(int value) { return telemetryReadable() && value >= 0 ? String(value) : String("--"); }
String uptimeValue() {
  return telemetryReadable() && state.uptime_days >= 0 ? String(state.uptime_days) + "d" : String("--d");
}

uint16_t dimColor(uint16_t value, uint8_t percent) {
  const uint16_t r = ((value >> 11) & 0x1F) * percent / 100;
  const uint16_t g = ((value >> 5) & 0x3F) * percent / 100;
  const uint16_t b = (value & 0x1F) * percent / 100;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void initPalette() {
  color.background = TFT_BLACK;
  color.panel = tft.color565(14, 17, 22);
  color.panel_alt = tft.color565(16, 26, 32);
  color.text = tft.color565(245, 247, 250);
  color.dim = tft.color565(138, 147, 163);
  color.idle = tft.color565(74, 82, 96);
  color.guide = tft.color565(37, 49, 62);
  color.cyan = tft.color565(55, 196, 214);
  color.blue = tft.color565(63, 169, 245);
  color.lime = tft.color565(200, 230, 60);
  color.green = tft.color565(54, 210, 126);
  color.amber = tft.color565(255, 200, 0);
  color.orange = tft.color565(255, 122, 26);
  color.red = tft.color565(255, 64, 87);
}

void drawLabel(TFT_eSprite &canvas, const String &text, int16_t x, int16_t y,
               uint8_t font, uint16_t text_color, uint8_t datum = TL_DATUM) {
  canvas.setTextDatum(datum);
  canvas.setTextColor(text_color);
  canvas.drawString(text, x, y, font);
}

String fitLabel(TFT_eSprite &canvas, const String &text, uint8_t font, int16_t max_width) {
  if (max_width <= 0) return "";
  if (canvas.textWidth(text, font) <= max_width) return text;
  for (int length = text.length() - 1; length > 0; --length) {
    const String candidate = text.substring(0, length) + ".";
    if (canvas.textWidth(candidate, font) <= max_width) return candidate;
  }
  return "";
}

void drawBoundedLabel(TFT_eSprite &canvas, const String &text, int16_t x, int16_t y,
                      int16_t width, uint8_t font, uint16_t text_color,
                      uint8_t datum = TL_DATUM) {
  const String fitted = fitLabel(canvas, text, font, width);
  int16_t anchor_x = x;
  if (datum == TR_DATUM || datum == MR_DATUM || datum == BR_DATUM) anchor_x += width;
  else if (datum == TC_DATUM || datum == MC_DATUM || datum == BC_DATUM) anchor_x += width / 2;
  drawLabel(canvas, fitted, anchor_x, y, font, text_color, datum);
}

void drawBoundedPair(TFT_eSprite &canvas, const String &left, const String &right,
                     int16_t x, int16_t y, int16_t width, int16_t gap,
                     uint16_t left_color, uint16_t right_color) {
  const String fitted_right = fitLabel(canvas, right, 1, width);
  const int16_t right_width = canvas.textWidth(fitted_right, 1);
  const int16_t left_width = max<int16_t>(0, width - gap - right_width);
  drawBoundedLabel(canvas, left, x, y, left_width, 1, left_color);
  drawLabel(canvas, fitted_right, x + width, y, 1, right_color, TR_DATUM);
}

void drawEdgePair(TFT_eSprite &canvas, const String &left, const String &right, int16_t y,
                  uint16_t left_color, uint16_t right_color) {
  drawBoundedPair(canvas, left, right, ui::edge, y, ui::content_width, 8,
                  left_color, right_color);
}

void drawPanel(TFT_eSprite &canvas, int16_t x, int16_t y, int16_t width,
               int16_t height, int16_t radius = ui::panel_radius,
               uint16_t fill_color = 0, uint16_t border_color = 0) {
  const uint16_t fill = fill_color ? fill_color : color.panel;
  const uint16_t border = border_color ? border_color : color.guide;
  canvas.fillRoundRect(x, y, width, height, radius, fill);
  canvas.drawRoundRect(x, y, width, height, radius, border);
}

void drawTemperature(TFT_eSprite &canvas, int value, int16_t x, int16_t y,
                     uint8_t font, uint16_t text_color, uint8_t datum = TL_DATUM,
                     int16_t max_width = SCREEN_W) {
  String number = intValue(value);
  uint8_t selected = font;
  auto totalWidth = [&](const String &candidate, uint8_t candidate_font) {
    const int16_t radius = candidate_font == 1 ? 1 : 2;
    return canvas.textWidth(candidate, candidate_font) + radius * 2 + 3 +
           canvas.textWidth("C", candidate_font);
  };
  if (totalWidth(number, selected) > max_width) selected = 1;
  if (totalWidth(number, selected) > max_width) number = "--";
  const int16_t number_width = canvas.textWidth(number, selected);
  const int16_t c_width = canvas.textWidth("C", selected);
  const int16_t radius = selected == 1 ? 1 : 2;
  const int16_t degree_width = radius * 2 + 3;
  const int16_t total_width = number_width + degree_width + c_width;
  int16_t left = x;
  if (datum == TR_DATUM || datum == MR_DATUM || datum == BR_DATUM) left -= total_width;
  else if (datum == TC_DATUM || datum == MC_DATUM || datum == BC_DATUM) left -= total_width / 2;
  drawLabel(canvas, number, left, y, selected, text_color);
  const int16_t degree_x = left + number_width + radius + 1;
  const int16_t degree_y = y + radius + 1;
  canvas.drawCircle(degree_x, degree_y, radius, text_color);
  drawLabel(canvas, "C", left + number_width + degree_width, y, selected, text_color);
}

void drawFitNumber(TFT_eSprite &canvas, const String &text, int16_t x, int16_t y,
                   int16_t max_width, int16_t max_height, uint16_t text_color,
                   uint8_t datum = TL_DATUM) {
  const uint8_t fonts[] = {6, 4, 2, 1};
  uint8_t selected = 1;
  bool found = false;
  for (uint8_t font : fonts) {
    if (canvas.textWidth(text, font) <= max_width && canvas.fontHeight(font) <= max_height) {
      selected = font;
      found = true;
      break;
    }
  }
  const String fitted = found ? text : fitLabel(canvas, text, 1, max_width);
  drawLabel(canvas, fitted, x, y, selected, text_color, datum);
}

void drawValueUnit(TFT_eSprite &canvas, const String &value, const String &unit,
                   int16_t x, int16_t value_y, int16_t unit_y, int16_t width,
                   int16_t max_height, uint16_t value_color, uint16_t unit_color) {
  constexpr int16_t gap = 4;
  const String fitted_unit = fitLabel(canvas, unit, 1, width / 3);
  const int16_t unit_width = canvas.textWidth(fitted_unit, 1);
  const int16_t number_width = max<int16_t>(1, width - unit_width - gap);
  const int16_t number_right = x + number_width;
  drawFitNumber(canvas, value, number_right, value_y, number_width, max_height,
                value_color, TR_DATUM);
  drawLabel(canvas, fitted_unit, x + width, unit_y, 1, unit_color, TR_DATUM);
}

void drawHeader(TFT_eSprite &canvas, const String &title, const String &status,
                uint16_t status_color) {
  const String fitted_status = fitLabel(canvas, status, 1, 82);
  const int16_t status_width = canvas.textWidth(fitted_status, 1);
  const int16_t title_width = 142 - status_width;
  drawLabel(canvas, fitLabel(canvas, title, 2, title_width), ui::edge, 6, 2, color.text);
  canvas.fillCircle(SCREEN_W - ui::edge - status_width - 6, 13, 2, status_color);
  drawLabel(canvas, fitted_status, SCREEN_W - ui::edge, 9, 1, status_color, TR_DATUM);
}

void drawHistoryCurve(TFT_eSprite &canvas, int16_t x, int16_t y, int16_t width,
                      int16_t height, const float *history, uint16_t line_color,
                      uint8_t thickness, bool fill_area, float scale_min,
                      float scale_max);

void drawStateBand(TFT_eSprite &canvas, const String &text, uint16_t state_color,
                   uint8_t font) {
  canvas.fillRoundRect(ui::edge, ui::state_y, ui::content_width, ui::state_h, 6,
                       dimColor(state_color, 8));
  canvas.drawRoundRect(ui::edge, ui::state_y, ui::content_width, ui::state_h, 6,
                       state_color);
  drawBoundedLabel(canvas, text, 14, ui::state_y + ui::state_h / 2, 142, font,
                   state_color, MC_DATUM);
}

void drawHeroBlock(TFT_eSprite &canvas, const String &label, const String &value,
                   uint16_t identity_color, uint16_t unit_color = 0) {
  drawBoundedLabel(canvas, label, ui::edge, ui::hero_label_y, ui::content_width,
                   1, identity_color);
  drawValueUnit(canvas, value, "Mb/s", ui::edge, ui::hero_value_y,
                ui::hero_unit_y, ui::content_width, 48,
                readingColor(color.text), unit_color ? unit_color : color.dim);
}

void drawTrendPanel(TFT_eSprite &canvas, const String &label,
                    const float *history, uint16_t line_color,
                    bool fill_area = false, float scale_min = NAN,
                    float scale_max = NAN) {
  drawPanel(canvas, ui::edge, ui::trend_y, ui::content_width, ui::trend_h);
  drawBoundedLabel(canvas, label, 15, ui::trend_y + 7, 140, 1, color.dim);
  drawHistoryCurve(canvas, 15, ui::trend_y + 20, 140, 36, history, line_color,
                   2, fill_area, scale_min, scale_max);
}

void drawPageAccent(TFT_eSprite &canvas, uint16_t accent) {
  canvas.fillRoundRect(ui::edge, ui::header_bottom - 2, ui::content_width, 2, 1,
                       dimColor(accent, 65));
}

void drawFooter(TFT_eSprite &canvas, Page page) {
  const uint8_t active = static_cast<uint8_t>(page);
  drawLabel(canvas, PAGE_NAMES[active], ui::edge, ui::footer_y, 1, color.text);
  for (uint8_t index = 0; index < PAGE_COUNT; ++index) {
    const int16_t x = 101 + index * 9;
    if (index == active) canvas.fillRoundRect(x - 4, ui::footer_indicator_y - 2, 8, 4, 2, color.text);
    else canvas.fillCircle(x, ui::footer_indicator_y, 1, color.idle);
  }
}

void drawMetricTile(TFT_eSprite &canvas, int16_t x, int16_t y, int16_t width,
                    const String &name, const String &value, uint16_t value_color = 0) {
  const uint16_t identity_color = value_color ? value_color : color.guide;
  drawPanel(canvas, x, y, width, 38, ui::panel_radius, color.panel_alt,
            dimColor(identity_color, 45));
  canvas.fillRoundRect(x + 6, y + 6, 2, 9, 1, identity_color);
  drawLabel(canvas, name, x + 12, y + 7, 1, color.dim);
  drawFitNumber(canvas, value, x + 7, y + 18, width - 12, 18,
                readingColor(value_color ? value_color : color.text));
}

void drawTopologyNode(TFT_eSprite &canvas, int16_t x, const String &name,
                      uint16_t identity_color, bool active) {
  const uint16_t border = active ? identity_color : dimColor(identity_color, 42);
  canvas.fillRoundRect(x, 38, 44, 24, 6,
                       active ? dimColor(identity_color, 18) : color.panel);
  canvas.drawRoundRect(x, 38, 44, 24, 6, border);
  drawLabel(canvas, name, x + 22, 50, 1, border, MC_DATUM);
}

bool normalizeHistory(const float *source, uint8_t *target,
                      float scale_min = NAN, float scale_max = NAN) {
  float minimum = INFINITY;
  float maximum = -INFINITY;
  for (uint8_t index = 0; index < HISTORY_COUNT; ++index) {
    if (!isfinite(source[index])) continue;
    minimum = min(minimum, source[index]);
    maximum = max(maximum, source[index]);
  }
  if (!isfinite(minimum)) return false;
  float carried_value = minimum;
  if (isfinite(scale_min) && isfinite(scale_max) && scale_max > scale_min) {
    minimum = scale_min;
    maximum = scale_max;
  } else {
    minimum = min(0.0f, minimum);
    maximum = max(maximum * 1.08f, minimum + 1.0f);
  }
  for (uint8_t index = 0; index < HISTORY_COUNT; ++index) {
    if (isfinite(source[index])) carried_value = source[index];
    const float value = carried_value;
    target[index] = static_cast<uint8_t>(constrain(
        (value - minimum) * 100.0f / (maximum - minimum), 0.0f, 100.0f));
  }
  return true;
}

float sharedNetworkScale() {
  float peak = 1.0f;
  for (uint8_t index = 0; index < HISTORY_COUNT; ++index) {
    if (isfinite(down_history[index])) peak = max(peak, down_history[index]);
    if (isfinite(up_history[index])) peak = max(peak, up_history[index]);
  }
  return peak * 1.08f;
}

void drawSmoothCurve(TFT_eSprite &canvas, int16_t x, int16_t y, int16_t width,
                     int16_t height, const uint8_t *values, uint8_t count,
                     uint16_t line_color, uint8_t thickness = 2,
                     bool fill_area = false, bool dashed = false) {
  if (count < 2) return;
  const uint16_t fill_color = dimColor(line_color, 22);
  const float step_x = static_cast<float>(width - 1) / (count - 1);
  auto valueY = [&](uint8_t index) {
    return y + height - 1 - (constrain(values[index], 0, 100) * (height - 1) / 100.0f);
  };
  float previous_x = x;
  float previous_y = valueY(0);
  uint16_t dash_index = 0;
  for (uint8_t segment = 0; segment < count - 1; ++segment) {
    const uint8_t i0 = segment > 0 ? segment - 1 : segment;
    const uint8_t i1 = segment;
    const uint8_t i2 = segment + 1;
    const uint8_t i3 = segment + 2 < count ? segment + 2 : segment + 1;
    const float p0x = x + i0 * step_x;
    const float p1x = x + i1 * step_x;
    const float p2x = x + i2 * step_x;
    const float p3x = x + i3 * step_x;
    const float p0y = valueY(i0);
    const float p1y = valueY(i1);
    const float p2y = valueY(i2);
    const float p3y = valueY(i3);
    const float c1x = p1x + (p2x - p0x) / 6.0f;
    const float c1y = p1y + (p2y - p0y) / 6.0f;
    const float c2x = p2x - (p3x - p1x) / 6.0f;
    const float c2y = p2y - (p3y - p1y) / 6.0f;
    const uint8_t samples = max<uint8_t>(5, static_cast<uint8_t>(ceilf(step_x)));
    for (uint8_t sample = 1; sample <= samples; ++sample) {
      const float t = static_cast<float>(sample) / samples;
      const float u = 1.0f - t;
      const float bx = u * u * u * p1x + 3.0f * u * u * t * c1x +
                       3.0f * u * t * t * c2x + t * t * t * p2x;
      const float by = u * u * u * p1y + 3.0f * u * u * t * c1y +
                       3.0f * u * t * t * c2y + t * t * t * p2y;
      const int16_t pixel_x = roundf(bx);
      const int16_t pixel_y = constrain(static_cast<int16_t>(roundf(by)), y, y + height - 1);
      if (fill_area) canvas.drawFastVLine(pixel_x, pixel_y, y + height - pixel_y, fill_color);
      if (!dashed || ((dash_index / 3) % 2 == 0)) {
        canvas.drawLine(roundf(previous_x), roundf(previous_y), pixel_x, pixel_y, line_color);
        if (thickness > 1 && pixel_y + 1 < y + height) {
          canvas.drawLine(roundf(previous_x), roundf(previous_y) + 1, pixel_x, pixel_y + 1, line_color);
        }
      }
      previous_x = bx;
      previous_y = pixel_y;
      ++dash_index;
    }
  }
}

void drawHistoryCurve(TFT_eSprite &canvas, int16_t x, int16_t y, int16_t width,
                      int16_t height, const float *history, uint16_t line_color,
                      uint8_t thickness = 2, bool fill_area = false,
                      float scale_min = NAN, float scale_max = NAN) {
  if (!realtimeValid()) {
    drawSmoothCurve(canvas, x, y, width, height, TIMEOUT_CURVE, HISTORY_COUNT,
                    color.idle, 2, false, true);
    return;
  }
  uint8_t normalized[HISTORY_COUNT];
  if (!normalizeHistory(history, normalized, scale_min, scale_max)) {
    drawSmoothCurve(canvas, x, y, width, height, TIMEOUT_CURVE, HISTORY_COUNT,
                    color.idle, 2, false, true);
    return;
  }
  drawSmoothCurve(canvas, x, y, width, height, normalized, HISTORY_COUNT,
                  line_color, thickness, fill_area);
}

void seedHistories() {
  for (uint8_t index = 0; index < HISTORY_COUNT; ++index) {
    down_history[index] = isfinite(state.down_mbps) ? state.down_mbps : NAN;
    up_history[index] = isfinite(state.up_mbps) ? state.up_mbps : NAN;
    latency_history[index] = state.api_rtt_ms >= 0 ? state.api_rtt_ms : NAN;
    cpu_history[index] = state.cpu >= 0 ? state.cpu : NAN;
    temp_history[index] = state.disk_temp_c >= 0 ? state.disk_temp_c : NAN;
  }
}

void appendHistory(float *history, float value) {
  if (!isfinite(value)) return;
  for (uint8_t index = 0; index < HISTORY_COUNT - 1; ++index) history[index] = history[index + 1];
  history[HISTORY_COUNT - 1] = value;
}

void appendCurrentStateToHistory() {
  if (!realtimeValid()) return;
  appendHistory(down_history, state.down_mbps);
  appendHistory(up_history, state.up_mbps);
  appendHistory(latency_history, state.api_rtt_ms >= 0 ? state.api_rtt_ms : NAN);
  appendHistory(cpu_history, state.cpu >= 0 ? state.cpu : NAN);
  appendHistory(temp_history, state.disk_temp_c >= 0 ? state.disk_temp_c : NAN);
}

void drawOverview(TFT_eSprite &canvas) {
  drawHeader(canvas, "FNOS", trustLabel(), trustColor());
  drawPageAccent(canvas, color.cyan);
  drawLabel(canvas, "DOWNLOAD", 8, 30, 1, color.cyan);
  drawValueUnit(canvas, floatValue(state.down_mbps), "Mb/s", 8, 42, 68, 154, 48,
                readingColor(color.text), color.dim);
  drawLabel(canvas, "UPLOAD", 8, 96, 1, color.blue);
  drawValueUnit(canvas, floatValue(state.up_mbps), "Mb/s", 8, 105, 113, 154, 26,
                readingColor(color.text), color.dim);
  drawPanel(canvas, 8, 135, 154, 67);
  drawLabel(canvas, String(HISTORY_WINDOW_SECONDS) + "S LIVE", 15, 142, 1, color.dim);
  const float network_scale = sharedNetworkScale();
  drawHistoryCurve(canvas, 14, 154, 142, 41, down_history, color.cyan, 2, true, 0.0f, network_scale);
  drawHistoryCurve(canvas, 14, 169, 142, 25, up_history, color.blue, 1, false, 0.0f, network_scale);
  drawMetricTile(canvas, 8, 213, 46, "API RT",
                 state.api_rtt_ms >= 0 ? String(state.api_rtt_ms) + "ms" : "--ms", color.orange);
  drawMetricTile(canvas, 62, 213, 46, "MEM", percentValue(state.memory));
  drawMetricTile(canvas, 116, 213, 46, "DISK", percentValue(storagePercent()), color.amber);
  drawBoundedLabel(canvas, "CPU " + percentValue(state.cpu), 8, 263, 48, 1, color.dim);
  drawBoundedLabel(canvas, "TEMP", 62, 263, 27, 1, color.dim);
  drawTemperature(canvas, state.cpu_temp_c, 120, 263, 1, color.dim, TR_DATUM, 31);
  drawBoundedLabel(canvas, uptimeValue(), 132, 263, 30, 1, color.dim, TR_DATUM);
  drawFooter(canvas, Page::Overview);
}

void drawNetwork(TFT_eSprite &canvas) {
  drawHeader(canvas, "NETWORK", trustLabel(), trustColor());
  drawPageAccent(canvas, color.blue);
  drawPanel(canvas, 8, 29, 154, 43, 8);
  const bool source_active = realtimeValid();
  const bool wifi_active = WiFi.status() == WL_CONNECTED;
  drawTopologyNode(canvas, 9, "NAS", color.blue, source_active);
  canvas.drawLine(53, 50, 62, 50, source_active ? color.cyan : color.idle);
  canvas.fillTriangle(64, 50, 59, 47, 59, 53, source_active ? color.cyan : color.idle);
  drawTopologyNode(canvas, 64, "WIFI", color.cyan, wifi_active);
  canvas.drawLine(108, 50, 117, 50, wifi_active ? color.lime : color.idle);
  canvas.fillTriangle(119, 50, 114, 47, 114, 53, wifi_active ? color.lime : color.idle);
  drawTopologyNode(canvas, 118, "LCD", color.lime, true);
  drawBoundedLabel(canvas, "WAN DOWN", 8, 83, 54, 1, color.cyan);
  drawValueUnit(canvas, floatValue(state.down_mbps), "Mb/s", 68, 78, 91, 94, 26,
                readingColor(color.text), color.dim);
  drawBoundedLabel(canvas, "WAN UP", 8, 111, 54, 1, color.blue);
  drawValueUnit(canvas, floatValue(state.up_mbps), "Mb/s", 68, 106, 119, 94, 26,
                readingColor(color.text), color.dim);
  drawPanel(canvas, 8, 139, 154, 61);
  drawBoundedLabel(canvas, "API RTT " + String(HISTORY_WINDOW_SECONDS) + "S  " +
                        intValue(state.api_rtt_ms) + "ms", 15, 146, 140, 1, color.dim);
  drawHistoryCurve(canvas, 15, 159, 140, 34, latency_history, color.orange, 2, false, 0.0f, 1500.0f);
  String lan_value = "-- LINK";
  if (state.lan_speed_mbps >= 1000) lan_value = String(state.lan_speed_mbps / 1000) + "G LINK";
  else if (state.lan_speed_mbps > 0) lan_value = String(state.lan_speed_mbps) + "M LINK";
  else if (state.online) lan_value = "LINK UP";
  drawMetricTile(canvas, 8, 211, 73, "LAN", lan_value);
  const String wifi_value = wifi_active ? String(WiFi.RSSI()) + "dBm" : "--dBm";
  drawMetricTile(canvas, 89, 211, 73, "WIFI", wifi_value);
  drawEdgePair(canvas, FNOS_HOST[0] ? String(FNOS_HOST) : String("192.168.9.23"),
               trustLabel(), 266, color.dim, trustColor());
  drawFooter(canvas, Page::Network);
}

void drawStorage(TFT_eSprite &canvas) {
  const int used = storagePercent();
  drawHeader(canvas, "STORAGE", trustLabel(), trustColor());
  drawPageAccent(canvas, color.amber);
  canvas.drawArc(69, 76, 46, 36, 30, 330, color.guide, color.background, true);
  if (used > 0) canvas.drawArc(69, 76, 46, 36, 30, 30 + used * 3, color.amber, color.background, true);
  drawLabel(canvas, percentValue(used), 69, 73, 4, color.text, MC_DATUM);
  drawLabel(canvas, "USED", 69, 101, 1, color.dim, MC_DATUM);
  drawLabel(canvas, "TOTAL", 122, 48, 1, color.dim);
  drawFitNumber(canvas,
                isfinite(state.total_tb) && state.total_tb > 0 ? String(state.total_tb, 1) + "T" : "--.-T",
                122, 60, 40, 18, color.text);
  drawLabel(canvas, "FREE", 122, 87, 1, color.dim);
  const float free_tb = isfinite(state.total_tb) && isfinite(state.used_tb)
                            ? max(0.0f, state.total_tb - state.used_tb) : NAN;
  drawFitNumber(canvas, isfinite(free_tb) ? String(free_tb, 1) + "T" : "--.-T",
                122, 99, 40, 18, color.text);
  drawPanel(canvas, 8, 133, 154, 70);
  drawLabel(canvas, "CAPACITY", 15, 140, 1, color.dim);
  drawLabel(canvas, "POOL", 15, 158, 1, color.text);
  canvas.fillRoundRect(49, 159, 104, 7, 3, color.guide);
  if (used >= 0) canvas.fillRoundRect(49, 159, max(1, used * 104 / 100), 7, 3, color.amber);
  drawBoundedPair(canvas,
                  "USED " + (isfinite(state.used_tb) ? String(state.used_tb, 1) + "T" : String("--.-T")),
                  "FREE " + (isfinite(free_tb) ? String(free_tb, 1) + "T" : String("--.-T")),
                  15, 181, 138, 8, color.text, color.dim);
  canvas.drawFastVLine(84, 174, 21, color.guide);
  drawLabel(canvas, "DISK TEMP", 8, 218, 1, color.orange);
  drawTemperature(canvas, state.disk_temp_c, 162, 210, 2,
                  state.disk_temp_c >= 0 ? color.text : color.dim, TR_DATUM, 54);
  drawHistoryCurve(canvas, 8, 237, 154, 31, temp_history, color.orange, 2, false, 20.0f, 70.0f);
  const String raid_value = state.raid_status.length() ? "RAID " + state.raid_status : "RAID UNKNOWN";
  const uint16_t raid_color = state.raid_status == "HEALTHY" ? color.green
                              : state.raid_status.length() ? color.orange : color.dim;
  drawEdgePair(canvas, raid_value,
               isfinite(state.disk_read_mb_s) ? "R " + String(state.disk_read_mb_s, 1) + "MB/s" : "R --MB/s",
               275, raid_color, color.blue);
  drawFooter(canvas, Page::Storage);
}

void drawSystem(TFT_eSprite &canvas) {
  drawHeader(canvas, "SYSTEM", trustLabel(), trustColor());
  drawPageAccent(canvas, color.blue);
  drawPanel(canvas, 8, 31, 73, 74, 8);
  drawLabel(canvas, "CPU", 16, 39, 1, color.blue);
  drawFitNumber(canvas, percentValue(state.cpu), 16, 53, 61, 32, color.text);
  drawTemperature(canvas, state.cpu_temp_c, 16, 90, 1,
                  state.cpu_temp_c < 0 ? color.dim
                  : state.cpu_temp_c < 75 ? color.green : color.orange,
                  TL_DATUM, 57);
  drawPanel(canvas, 89, 31, 73, 74, 8);
  drawLabel(canvas, "MEMORY", 97, 39, 1, color.cyan);
  drawFitNumber(canvas, percentValue(state.memory), 97, 53, 61, 32, color.text);
  drawBoundedLabel(canvas,
                   isfinite(state.memory_used_gb) ? String(state.memory_used_gb, 1) + "G" : "--.-G",
                   97, 90, 57, 1, color.dim);
  drawPanel(canvas, 8, 116, 154, 63);
  drawLabel(canvas, "CPU LOAD " + String(HISTORY_WINDOW_SECONDS) + "S", 15, 123, 1, color.dim);
  drawHistoryCurve(canvas, 15, 137, 140, 35, cpu_history, color.blue, 2, false, 0.0f, 100.0f);
  drawLabel(canvas, "GPU", 8, 194, 1, color.dim);
  drawLabel(canvas, percentValue(state.gpu), 162, 189, 2, color.text, TR_DATUM);
  drawPanel(canvas, 8, 218, 154, 35, 7, color.panel_alt);
  const bool storage_healthy = state.raid_status == "HEALTHY";
  canvas.fillCircle(18, 235, 4, storage_healthy ? color.green : color.orange);
  drawBoundedLabel(canvas, state.raid_status.length() ? state.raid_status : "NO DATA",
                   28, 228, 82, 1, storage_healthy ? color.text : color.orange);
  drawTemperature(canvas, state.disk_temp_c, 153, 228, 1,
                  state.disk_temp_c >= 0 ? color.text : color.dim, TR_DATUM, 35);
  drawEdgePair(canvas, "UPTIME " + uptimeValue(),
               isfinite(state.load_1m) ? "LOAD " + String(state.load_1m, 2) : "LOAD --",
               268, color.dim, color.dim);
  drawFooter(canvas, Page::System);
}

void drawNormal(TFT_eSprite &canvas) {
  const DataTrust trust = dataTrust();
  const bool live = trust == DataTrust::Live;
  const String state_text = live ? "ONLINE HEALTHY"
                            : trust == DataTrust::Stale ? "DATA STALE"
                            : trust == DataTrust::Warming ? "SYNCING" : "LINK LOST";
  drawHeader(canvas, "STATE", trustLabel(), trustColor());
  drawPageAccent(canvas, color.green);
  drawStateBand(canvas, state_text, trustColor(), live ? 2 : 1);
  drawHeroBlock(canvas, "DOWNLOAD", floatValue(state.down_mbps), color.cyan);
  drawTrendPanel(canvas, "NETWORK " + String(HISTORY_WINDOW_SECONDS) + "S",
                 down_history, color.cyan, true);
  drawMetricTile(canvas, 8, ui::tiles_y, 46, "API RT",
                 state.api_rtt_ms >= 0 ? String(state.api_rtt_ms) + "ms" : "--ms", color.orange);
  drawMetricTile(canvas, 62, ui::tiles_y, 46, "CPU", percentValue(state.cpu));
  drawMetricTile(canvas, 116, ui::tiles_y, 46, "DISK", percentValue(storagePercent()), color.amber);
  drawEdgePair(canvas, live ? "SERVICES READY" : "SERVICES UNKNOWN",
               state.api_rtt_ms >= 0 ? "API " + String(state.api_rtt_ms) + "ms" : "API NO DATA",
               ui::summary_y, trustColor(),
               state.api_rtt_ms >= 0 ? color.green : color.dim);
  drawFooter(canvas, Page::Normal);
}

void drawHighLoad(TFT_eSprite &canvas) {
  const float down = state.down_mbps;
  const float up = state.up_mbps;
  const int cpu = state.cpu;
  const int api_rtt = state.api_rtt_ms;
  const int disk = storagePercent();
  const bool live = dataTrust() == DataTrust::Live;
  const bool high = live && (cpu >= 80 || disk >= 85 || (isfinite(down) && down >= 800.0f));
  drawHeader(canvas, "STATE", live ? (high ? "HIGH LOAD" : "LOAD WATCH") : trustLabel(),
             live ? (high ? color.amber : color.green) : trustColor());
  const uint16_t load_color = live ? (high ? color.amber : color.green) : trustColor();
  drawPageAccent(canvas, load_color);
  drawStateBand(canvas, live ? (high ? "HIGH LOAD" : "LOAD NORMAL") : trustLabel(),
                load_color, live ? 2 : 1);
  drawHeroBlock(canvas, "DOWNLOAD", floatValue(down), color.cyan);
  drawTrendPanel(canvas, String(high ? "TRAFFIC PEAK " : "TRAFFIC WATCH ") +
                           HISTORY_WINDOW_SECONDS + "S",
                 down_history, color.cyan, true);
  drawMetricTile(canvas, 8, ui::tiles_y, 46, "API RT",
                 api_rtt >= 0 ? String(api_rtt) + "ms" : "--ms", color.orange);
  drawMetricTile(canvas, 62, ui::tiles_y, 46, "CPU", percentValue(cpu), high ? color.amber : color.text);
  drawMetricTile(canvas, 116, ui::tiles_y, 46, "DISK", percentValue(disk), high ? color.amber : color.text);
  drawEdgePair(canvas, live ? (high ? "THROTTLE WATCH" : "CAPACITY READY") : trustLabel(),
               floatValue(up) + " UP", ui::summary_y,
               live ? (high ? color.amber : color.green) : trustColor(), color.blue);
  drawFooter(canvas, Page::HighLoad);
}

void drawTimeout(TFT_eSprite &canvas) {
  const DataTrust trust = dataTrust();
  const bool unavailable = trust == DataTrust::Warming || trust == DataTrust::Offline;
  const String state_text = trust == DataTrust::Warming ? "WAITING FOR DATA"
                            : trust == DataTrust::Stale ? "CACHED VALUES"
                            : trust == DataTrust::Offline ? "CONNECTION LOST" : "LINK READY";
  drawHeader(canvas, "STATE", trustLabel(), trustColor());
  drawPageAccent(canvas, trustColor());
  drawStateBand(canvas, state_text, trustColor(),
                trust == DataTrust::Live ? 2 : 1);
  drawHeroBlock(canvas, "DOWNLOAD",
                unavailable ? "--.-" : floatValue(state.down_mbps),
                unavailable ? color.idle : color.cyan, color.idle);
  drawPanel(canvas, ui::edge, ui::trend_y, ui::content_width, ui::trend_h);
  drawBoundedLabel(canvas, unavailable ? String("NO REALTIME DATA")
                                       : "NETWORK " + String(HISTORY_WINDOW_SECONDS) + "S",
                   15, ui::trend_y + 7, 140, 1,
                   unavailable ? trustColor() : color.dim);
  if (unavailable) {
    drawSmoothCurve(canvas, 15, ui::trend_y + 20, 140, 36, TIMEOUT_CURVE, HISTORY_COUNT,
                    color.idle, 2, true, true);
  } else {
    drawHistoryCurve(canvas, 15, ui::trend_y + 20, 140, 36, down_history,
                     color.cyan, 2, true);
  }
  drawMetricTile(canvas, 8, ui::tiles_y, 73, "LAST GOOD",
                 state.last_good_ms ? String((millis() - state.last_good_ms) / 1000) + "s ago" : "--");
  drawMetricTile(canvas, 89, ui::tiles_y, 73, "NEXT RETRY", unavailable ? "1s" : "READY",
                 unavailable ? trustColor() : color.green);
  drawEdgePair(canvas, unavailable ? "API UNAVAILABLE" : "API CONNECTED",
               trustLabel(), ui::summary_y,
               unavailable ? trustColor() : color.green, trustColor());
  drawFooter(canvas, Page::Timeout);
}

void renderPage(TFT_eSprite &canvas, Page page) {
  canvas.fillSprite(color.background);
  switch (page) {
    case Page::Overview: drawOverview(canvas); break;
    case Page::Network: drawNetwork(canvas); break;
    case Page::Storage: drawStorage(canvas); break;
    case Page::System: drawSystem(canvas); break;
    case Page::Normal: drawNormal(canvas); break;
    case Page::HighLoad: drawHighLoad(canvas); break;
    case Page::Timeout: drawTimeout(canvas); break;
  }
  canvas.drawRect(0, 0, SCREEN_W, SCREEN_H, color.guide);
}

void invalidateFrames() {
  cached_a_page = -1000;
  cached_b_page = -1000;
  screen_dirty = true;
}

void ensureFrame(TFT_eSprite &frame, int &cached_page, int virtual_page) {
  const int page = pageModulo(virtual_page);
  if (cached_page == page) return;
  renderPage(frame, static_cast<Page>(page));
  cached_page = page;
}

void drawCurrentFrame() {
  const int base_page = static_cast<int>(floorf(page_position));
  const float fraction = page_position - base_page;
  const int16_t offset = constrain(static_cast<int16_t>(roundf(fraction * SCREEN_W)), 0, SCREEN_W);
  if (!frame_a_ready) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(color.red);
    tft.drawString("FRAMEBUFFER", SCREEN_W / 2, 145, 2);
    tft.drawString("UNAVAILABLE", SCREEN_W / 2, 170, 2);
    screen_dirty = false;
    return;
  }
  if (!frame_b_ready) {
    const int nearest_page = static_cast<int>(roundf(page_position));
    ensureFrame(frame_a, cached_a_page, nearest_page);
    frame_a.pushSprite(0, 0);
    screen_dirty = false;
    return;
  }
  ensureFrame(frame_a, cached_a_page, base_page);
  ensureFrame(frame_b, cached_b_page, base_page + 1);
  frame_a.pushSprite(-offset, 0);
  frame_b.pushSprite(SCREEN_W - offset, 0);
  screen_dirty = false;
}

void announcePage() {
  const int page = pageModulo(static_cast<int>(roundf(page_target)));
  Serial.printf("UI_PAGE %d %s\n", page, PAGE_NAMES[page]);
}

void selectRelativePage(int direction) {
  page_target += direction;
#if FNOS_REDUCED_MOTION
  page_position = page_target;
  page_velocity = 0.0f;
#endif
  screen_dirty = true;
  announcePage();
}

// Advance a critically damped spring from its current presentation value.
// The closed-form step stays stable when the loop is briefly late, while
// preserving velocity so a new page target can be chosen mid-transition.
void advancePageSpring(float delta) {
  const float displacement = page_position - page_target;
  const float omega = SPRING_OMEGA;
  const float velocity_term = page_velocity + omega * displacement;
  const float decay = expf(-omega * delta);
  const float next_displacement =
      (displacement + velocity_term * delta) * decay;
  page_velocity = (page_velocity - omega * velocity_term * delta) * decay;
  page_position = page_target + next_displacement;
}

bool updateButton(DebouncedButton &button, int direction) {
  const bool reading = digitalRead(button.pin);
  const uint32_t now = millis();
  if (reading != button.raw) {
    button.raw = reading;
    button.changed_at = now;
  }
  if (reading != button.stable && now - button.changed_at >= BUTTON_DEBOUNCE_MS) {
    button.stable = reading;
    if (button.stable == LOW) {
      selectRelativePage(direction);
      return true;
    }
  }
  return false;
}

void updatePageAnimation() {
#if FNOS_REDUCED_MOTION
  if (screen_dirty) drawCurrentFrame();
#else
  const uint32_t now_us = micros();
  if (!screen_dirty && fabsf(page_position - page_target) < .0005f && fabsf(page_velocity) < .001f) return;
  if (last_frame_us != 0 && now_us - last_frame_us < FRAME_INTERVAL_US) return;
  float delta = last_frame_us == 0 ? 1.0f / 60.0f : (now_us - last_frame_us) / 1000000.0f;
  last_frame_us = now_us;
  delta = constrain(delta, .001f, 1.0f / 30.0f);
  advancePageSpring(delta);
  if (fabsf(page_position - page_target) < .0005f && fabsf(page_velocity) < .002f) {
    page_position = page_target;
    page_velocity = 0.0f;
    if (fabsf(page_target) > 700.0f) {
      const float shift = floorf(page_target / PAGE_COUNT) * PAGE_COUNT;
      page_position -= shift;
      page_target -= shift;
    }
    if (telemetry_redraw_pending) {
      telemetry_redraw_pending = false;
      invalidateFrames();
    }
  }
  drawCurrentFrame();
#endif
}

void clearRealtimeValues(NasState &target) {
  target.cpu = target.memory = target.gpu = target.api_rtt_ms = -1;
  target.cpu_temp_c = target.disk_temp_c = target.lan_speed_mbps = -1;
  target.used_tb = target.total_tb = target.memory_used_gb = NAN;
  target.load_1m = target.disk_read_mb_s = NAN;
  target.down_mbps = target.up_mbps = NAN;
  target.uptime_days = -1;
  target.raid_status = "";
}

String prometheusLabel(const String &line, const char *name) {
  const String marker = String(name) + "=\"";
  const int start = line.indexOf(marker);
  if (start < 0) return "";
  const int value_start = start + marker.length();
  const int end = line.indexOf('"', value_start);
  return end >= value_start ? line.substring(value_start, end) : String();
}

bool parsePrometheusMetrics(HTTPClient &http, NasState &target) {
  clearRealtimeValues(target);
  double memory_used = NAN;
  double memory_total = NAN;
  double storage_total = 0.0;
  double storage_free = 0.0;
  double disk_read = 0.0;
  double network_receive = 0.0;
  double network_transmit = 0.0;
  bool saw_storage_total = false;
  bool saw_storage_free = false;
  bool saw_fnos_metric = false;
  bool smart_ok = true;
  String running_interfaces = ",";

  WiFiClient *stream = http.getStreamPtr();
  uint32_t idle_deadline = millis() + 5000;
  while (http.connected() || stream->available()) {
    if (!stream->available()) {
      if (static_cast<int32_t>(millis() - idle_deadline) >= 0) break;
      delay(1);
      continue;
    }
    String line = stream->readStringUntil('\n');
    line.trim();
    idle_deadline = millis() + 5000;
    if (line.length() == 0 || line[0] == '#') continue;

    const int brace = line.indexOf('{');
    const int space = line.indexOf(' ');
    const int name_end = brace >= 0 && brace < space ? brace : space;
    if (name_end <= 0 || space < name_end) continue;
    const String name = line.substring(0, name_end);
    if (!name.startsWith("fnos_")) continue;
    saw_fnos_metric = true;
    const double value = line.substring(line.lastIndexOf(' ') + 1).toDouble();

    if (name == "fnos_cpu_cpu_busy_all" || name == "fnos_cpu_usage") {
      target.cpu = constrain(static_cast<int>(round(value)), 0, 100);
    } else if (name == "fnos_cpu_cpu_temp" || name == "fnos_cpu_temp") {
      target.cpu_temp_c = max(target.cpu_temp_c, static_cast<int>(round(value)));
    } else if (name == "fnos_cpu_cpu_loadavg_avg1min") {
      target.load_1m = static_cast<float>(value);
    } else if (name == "fnos_gpu_busy") {
      target.gpu = max(target.gpu, constrain(static_cast<int>(round(value)), 0, 100));
    } else if (name == "fnos_memory_mem_used") {
      memory_used = value;
    } else if (name == "fnos_memory_mem_total") {
      memory_total = value;
    } else if (name == "fnos_disk_temp") {
      target.disk_temp_c = max(target.disk_temp_c, static_cast<int>(round(value)));
    } else if (name == "fnos_disk_read") {
      disk_read += value;
    } else if (name == "fnos_disk_smart_status_passed" && value < .5) {
      smart_ok = false;
    } else if (name == "fnos_store_array_fssize") {
      storage_total += value;
      saw_storage_total = true;
    } else if (name == "fnos_store_array_frsize") {
      storage_free += value;
      saw_storage_free = true;
    } else if (name == "fnos_store_array_md_array_state_info") {
      const String array_state = prometheusLabel(line, "array_state");
      if (array_state.length() && array_state != "clean") smart_ok = false;
    } else if (name == "fnos_network_running" && value >= .5) {
      const String interface_name = prometheusLabel(line, "interface_name");
      if (interface_name.length()) running_interfaces += interface_name + ",";
    } else if (name == "fnos_network_speed") {
      const String interface_name = prometheusLabel(line, "interface_name");
      if (running_interfaces.indexOf("," + interface_name + ",") >= 0)
        target.lan_speed_mbps = max(target.lan_speed_mbps, static_cast<int>(round(value)));
    } else if (name == "fnos_network_receive") {
      network_receive += value;
    } else if (name == "fnos_network_transmit") {
      network_transmit += value;
    } else if (name == "fnos_uptime") {
      target.uptime_days = max(0, static_cast<int>(value / 86400.0));
    } else if (name == "fnos_host_name_info") {
      const String host_name = prometheusLabel(line, "host_name");
      if (host_name.length()) target.hostname = host_name;
    }
  }

  if (isfinite(memory_used)) target.memory_used_gb = memory_used / 1073741824.0;
  if (isfinite(memory_used) && isfinite(memory_total) && memory_total > 0.0)
    target.memory = constrain(static_cast<int>(round(memory_used / memory_total * 100.0)), 0, 100);
  if (saw_storage_total && storage_total > 0.0) {
    target.total_tb = storage_total / 1099511627776.0;
    if (saw_storage_free) target.used_tb = max(0.0, storage_total - storage_free) / 1099511627776.0;
  }
  if (disk_read >= 0.0) target.disk_read_mb_s = disk_read / 1048576.0;
  target.down_mbps = network_receive * 8.0 / 1000000.0;
  target.up_mbps = network_transmit * 8.0 / 1000000.0;
  target.raid_status = smart_ok ? "HEALTHY" : "DEGRADED";
  return saw_fnos_metric;
}

bool fetchState(NasState &target) {
  if (WiFi.status() != WL_CONNECTED) {
    target.online = false;
    target.error = "Wi-Fi unavailable";
    target.consecutive_failures = min<int>(255, target.consecutive_failures + 1);
    return false;
  }
  NasState candidate = target;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(String("http://") + FNOS_HOST + FNOS_API_PATH);
  if (strlen(FNOS_API_TOKEN) > 0) http.addHeader("Authorization", String("Bearer ") + FNOS_API_TOKEN);
  const uint32_t request_started_ms = millis();
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    target.online = false;
    target.error = code > 0 ? "HTTP " + String(code) : "request timeout";
    target.consecutive_failures = min<int>(255, target.consecutive_failures + 1);
    http.end();
    return false;
  }
  if (!parsePrometheusMetrics(http, candidate)) {
    http.end();
    target.online = false;
    target.error = "metrics empty";
    target.consecutive_failures = min<int>(255, target.consecutive_failures + 1);
    return false;
  }
  candidate.api_rtt_ms = min<int>(9999, static_cast<int>(millis() - request_started_ms));
  http.end();
  candidate.online = true;
  candidate.consecutive_failures = 0;
  candidate.error = "";
  candidate.last_good_ms = max<uint32_t>(1, millis());
  target = candidate;
  return true;
}

void telemetryWorker(void *) {
  TickType_t next_wake = xTaskGetTickCount();
  const TickType_t refresh_ticks = pdMS_TO_TICKS(REFRESH_MS);
  for (;;) {
    vTaskDelayUntil(&next_wake, refresh_ticks);
    fetchState(collector_state);
    if (xSemaphoreTake(telemetry_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      pending_state = collector_state;
      telemetry_ready = true;
      xSemaphoreGive(telemetry_mutex);
    }
    if (xTaskGetTickCount() - next_wake >= refresh_ticks) next_wake = xTaskGetTickCount();
  }
}

void consumeTelemetry() {
  if (!telemetry_ready || telemetry_mutex == nullptr) return;
  if (xSemaphoreTake(telemetry_mutex, 0) != pdTRUE) return;
  if (telemetry_ready) {
    state = pending_state;
    telemetry_ready = false;
  }
  xSemaphoreGive(telemetry_mutex);
  announceTelemetry();
  appendCurrentStateToHistory();
  if (fabsf(page_position - page_target) < .0005f && fabsf(page_velocity) < .002f) {
    invalidateFrames();
  } else {
    telemetry_redraw_pending = true;
  }
}

void announceTelemetry() {
  Serial.printf(
      "FNOS_STATE ui=%s online=%s cpu=%d memory=%d gpu=%d api_rtt=%dms "
      "storage=%d disk_temp=%dC lan=%dM down=%.2f up=%.2f fetch=%s\n",
      UI_BUILD_ID, state.online ? "yes" : "no", state.cpu, state.memory, state.gpu,
      state.api_rtt_ms, storagePercent(), state.disk_temp_c, state.lan_speed_mbps,
      state.down_mbps, state.up_mbps, state.error.length() ? state.error.c_str() : "ok");
}

void initFrames() {
  tft.setAttribute(PSRAM_ENABLE, true);
  frame_a.setColorDepth(16);
  frame_b.setColorDepth(16);
  frame_a_ready = frame_a.createSprite(SCREEN_W, SCREEN_H) != nullptr;
  frame_b_ready = frame_b.createSprite(SCREEN_W, SCREEN_H) != nullptr;
  if (!frame_a_ready && frame_b_ready) {
    frame_b.deleteSprite();
    frame_b_ready = false;
  }
  Serial.printf("UI_BUFFERS primary=%s secondary=%s psram=%s\n",
                frame_a_ready ? "ok" : "failed", frame_b_ready ? "ok" : "failed",
                psramFound() ? "yes" : "no");
}

void initBacklightOff() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON ? LOW : HIGH);
  ledcSetup(BACKLIGHT_PWM_CHANNEL, BACKLIGHT_PWM_HZ, BACKLIGHT_PWM_BITS);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, 0);
  ledcAttachPin(TFT_BL, BACKLIGHT_PWM_CHANNEL);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, 0);
}

void setBacklightPercent(uint8_t percent) {
  const uint8_t clamped = constrain(percent, 0, 100);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, clamped * 255U / 100U);
}

void fadeInBacklight() {
  constexpr uint8_t steps = 12;
  for (uint8_t step = 1; step <= steps; ++step) {
    setBacklightPercent(BACKLIGHT_PERCENT * step / steps);
    delay(BACKLIGHT_FADE_MS / steps);
  }
}

}  // namespace

void setup() {
  initBacklightOff();
  Serial.begin(115200);
  Serial.printf("UI_BUILD %s pages=%u adaptive=yes\n", UI_BUILD_ID, PAGE_COUNT);
  pinMode(BUTTON_1, INPUT_PULLUP);
  pinMode(BUTTON_2, INPUT_PULLUP);
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  initPalette();
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color.text);
  tft.drawString("FNOS MONITOR", SCREEN_W / 2, 148, 2);
  tft.setTextColor(color.dim);
  tft.drawString("building 7 views...", SCREEN_W / 2, 172, 1);
  WiFi.mode(WIFI_STA);
  WiFi.begin(FNOS_WIFI_SSID, FNOS_WIFI_PASSWORD);
  announceTelemetry();
  seedHistories();
  initFrames();
  invalidateFrames();
  drawCurrentFrame();
  fadeInBacklight();
  announcePage();
  collector_state = state;
  pending_state = state;
  telemetry_mutex = xSemaphoreCreateMutex();
  if (telemetry_mutex != nullptr) {
    xTaskCreate(telemetryWorker, "fnos-telemetry", 8192, nullptr, 1, &telemetry_task);
  }
}

void loop() {
  updateButton(previous_button, -1);
  updateButton(next_button, 1);
  consumeTelemetry();
  updatePageAnimation();
  delay(2);
}
