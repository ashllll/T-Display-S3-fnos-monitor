#include <Arduino.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <math.h>

#if __has_include("local_config.h")
#include "local_config.h"
#else
#define FNOS_DEMO_MODE 1
#define FNOS_HOST ""
#define FNOS_API_PATH "/metrics"
#define FNOS_API_TOKEN ""
#endif

#ifndef FNOS_REDUCED_MOTION
#define FNOS_REDUCED_MOTION 0
#endif

namespace {

constexpr int16_t SCREEN_W = 170;
constexpr int16_t SCREEN_H = 320;
constexpr uint8_t PAGE_COUNT = 7;
constexpr uint8_t HISTORY_COUNT = 14;
constexpr uint32_t REFRESH_MS = 5000;
constexpr uint32_t FRAME_INTERVAL_US = 16667;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 18;
constexpr float SPRING_RESPONSE = 0.40f;
constexpr float SPRING_OMEGA = 2.0f * PI / SPRING_RESPONSE;

enum class Page : uint8_t { Overview, Network, Storage, System, Normal, HighLoad, Timeout };

struct Palette {
  uint16_t background, panel, panel_alt, text, dim, idle, guide;
  uint16_t cyan, blue, green, amber, orange, red;
};

struct NasState {
  bool online = false;
  bool demo = true;
  int cpu = -1;
  int memory = -1;
  int ping_ms = -1;
  int clients = -1;
  int cpu_temp_c = -1;
  int disk_temp_c = -1;
  int lan_speed_mbps = -1;
  int docker_running = -1;
  int docker_total = -1;
  int packet_loss = -1;
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
DebouncedButton previous_button{BUTTON_1};
DebouncedButton next_button{BUTTON_2};

bool frame_a_ready = false;
bool frame_b_ready = false;
int cached_a_page = -1000;
int cached_b_page = -1000;
float page_position = 0.0f;
float page_target = 0.0f;
float page_velocity = 0.0f;
uint32_t last_refresh = 0;
uint32_t last_frame_us = 0;
bool screen_dirty = true;

float down_history[HISTORY_COUNT]{};
float up_history[HISTORY_COUNT]{};
float ping_history[HISTORY_COUNT]{};
float cpu_history[HISTORY_COUNT]{};
float temp_history[HISTORY_COUNT]{};

const char *const PAGE_NAMES[PAGE_COUNT] = {
    "OVERVIEW", "NETWORK", "STORAGE", "SYSTEM", "NORMAL", "HIGH LOAD", "TIMEOUT"};
const uint8_t HIGH_LOAD_CURVE[HISTORY_COUNT] = {
    12, 24, 48, 34, 70, 55, 88, 68, 96, 78, 100, 80, 94, 86};
const uint8_t TIMEOUT_CURVE[HISTORY_COUNT] = {
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42};

int pageModulo(int value) {
  int result = value % PAGE_COUNT;
  return result < 0 ? result + PAGE_COUNT : result;
}

bool realtimeValid() { return state.demo || state.online; }

int storagePercent() {
  if (!isfinite(state.used_tb) || !isfinite(state.total_tb) || state.total_tb <= 0.0f) return -1;
  return constrain(static_cast<int>(roundf(state.used_tb / state.total_tb * 100.0f)), 0, 100);
}

String floatValue(float value, uint8_t decimals = 1) {
  return isfinite(value) ? String(value, static_cast<unsigned int>(decimals)) : String("--.-");
}

String percentValue(int value) { return value >= 0 ? String(value) + "%" : String("--%"); }
String intValue(int value) { return value >= 0 ? String(value) : String("--"); }
String uptimeValue() { return state.uptime_days >= 0 ? String(state.uptime_days) + "d" : String("--d"); }

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

void drawTemperature(TFT_eSprite &canvas, int value, int16_t x, int16_t y,
                     uint8_t font, uint16_t text_color, uint8_t datum = TL_DATUM) {
  const String number = intValue(value);
  const int16_t number_width = canvas.textWidth(number, font);
  const int16_t c_width = canvas.textWidth("C", font);
  const int16_t radius = font == 1 ? 1 : 2;
  const int16_t degree_width = radius * 2 + 3;
  const int16_t total_width = number_width + degree_width + c_width;
  int16_t left = x;
  if (datum == TR_DATUM || datum == MR_DATUM || datum == BR_DATUM) left -= total_width;
  else if (datum == TC_DATUM || datum == MC_DATUM || datum == BC_DATUM) left -= total_width / 2;
  drawLabel(canvas, number, left, y, font, text_color);
  const int16_t degree_x = left + number_width + radius + 1;
  const int16_t degree_y = y + radius + 1;
  canvas.drawCircle(degree_x, degree_y, radius, text_color);
  drawLabel(canvas, "C", left + number_width + degree_width, y, font, text_color);
}

void drawFitNumber(TFT_eSprite &canvas, const String &text, int16_t x, int16_t y,
                   int16_t max_width, int16_t max_height, uint16_t text_color,
                   uint8_t datum = TL_DATUM) {
  const uint8_t fonts[] = {6, 4, 2};
  uint8_t selected = 2;
  for (uint8_t font : fonts) {
    if (canvas.textWidth(text, font) <= max_width && canvas.fontHeight(font) <= max_height) {
      selected = font;
      break;
    }
  }
  drawLabel(canvas, text, x, y, selected, text_color, datum);
}

void drawHeader(TFT_eSprite &canvas, const String &title, const String &status,
                uint16_t status_color) {
  drawLabel(canvas, title, 8, 6, 2, color.text);
  drawLabel(canvas, status, 162, 9, 1, status_color, TR_DATUM);
}

void drawFooter(TFT_eSprite &canvas, Page page) {
  const uint8_t active = static_cast<uint8_t>(page);
  drawLabel(canvas, PAGE_NAMES[active], 8, 294, 1, color.text);
  for (uint8_t index = 0; index < PAGE_COUNT; ++index) {
    canvas.fillCircle(102 + index * 10, 298, index == active ? 3 : 2,
                      index == active ? color.text : color.idle);
  }
}

void drawMetricTile(TFT_eSprite &canvas, int16_t x, int16_t y, int16_t width,
                    const String &name, const String &value, uint16_t value_color = 0) {
  canvas.fillRoundRect(x, y, width, 38, 7, color.panel_alt);
  drawLabel(canvas, name, x + 7, y + 7, 1, color.dim);
  drawFitNumber(canvas, value, x + 7, y + 18, width - 12, 18,
                value_color ? value_color : color.text);
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
  const float down_pattern[HISTORY_COUNT] = {
      .66f, .74f, .68f, .86f, .72f, .96f, .78f, 1.02f, .82f, .98f, .76f, .91f, .84f, 1.04f};
  const float up_pattern[HISTORY_COUNT] = {
      .82f, .87f, .79f, .94f, .84f, 1.02f, .86f, 1.08f, .88f, 1.03f, .90f, 1.00f, .94f, .91f};
  const float ping_pattern[HISTORY_COUNT] = {
      .88f, .96f, .83f, 1.14f, .91f, 1.22f, .95f, 1.28f, .98f, 1.18f, .90f, 1.11f, .94f, 1.04f};
  const float down = isfinite(state.down_mbps) ? max(1.0f, state.down_mbps) : 128.6f;
  const float up = isfinite(state.up_mbps) ? max(.2f, state.up_mbps) : 12.4f;
  const float ping = state.ping_ms >= 0 ? state.ping_ms : 24.0f;
  const float cpu = state.cpu >= 0 ? state.cpu : 12.0f;
  const float temp = state.disk_temp_c >= 0 ? state.disk_temp_c : 36.0f;
  for (uint8_t index = 0; index < HISTORY_COUNT; ++index) {
    if (state.demo) {
      down_history[index] = down * down_pattern[index];
      up_history[index] = up * up_pattern[index];
      ping_history[index] = ping * ping_pattern[index];
      cpu_history[index] = cpu * (.76f + (index % 5) * .08f);
      temp_history[index] = temp + (static_cast<int>(index % 5) - 2) * .55f;
    } else {
      down_history[index] = isfinite(state.down_mbps) ? state.down_mbps : NAN;
      up_history[index] = isfinite(state.up_mbps) ? state.up_mbps : NAN;
      ping_history[index] = state.ping_ms >= 0 ? state.ping_ms : NAN;
      cpu_history[index] = state.cpu >= 0 ? state.cpu : NAN;
      temp_history[index] = state.disk_temp_c >= 0 ? state.disk_temp_c : NAN;
    }
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
  appendHistory(ping_history, state.ping_ms >= 0 ? state.ping_ms : NAN);
  appendHistory(cpu_history, state.cpu >= 0 ? state.cpu : NAN);
  appendHistory(temp_history, state.disk_temp_c >= 0 ? state.disk_temp_c : NAN);
}

void drawOverview(TFT_eSprite &canvas) {
  const String status = state.demo ? "DEMO ONLINE" : (state.online ? "ONLINE" : "TIMEOUT");
  drawHeader(canvas, "FNOS", status, state.online || state.demo ? color.green : color.red);
  drawLabel(canvas, "DOWNLOAD", 8, 30, 1, color.cyan);
  drawFitNumber(canvas, floatValue(state.down_mbps), 7, 42, 138, 48, color.text);
  drawLabel(canvas, "Mb/s", 162, 68, 1, color.dim, TR_DATUM);
  drawLabel(canvas, "UPLOAD", 8, 96, 1, color.green);
  drawFitNumber(canvas, floatValue(state.up_mbps), 8, 105, 105, 26, color.text);
  drawLabel(canvas, "Mb/s", 162, 113, 1, color.dim, TR_DATUM);
  canvas.fillRoundRect(8, 135, 154, 67, 7, color.panel);
  drawLabel(canvas, "60S LIVE", 15, 142, 1, color.dim);
  const float network_scale = sharedNetworkScale();
  drawHistoryCurve(canvas, 14, 154, 142, 41, down_history, color.cyan, 2, true, 0.0f, network_scale);
  drawHistoryCurve(canvas, 14, 169, 142, 25, up_history, color.green, 1, false, 0.0f, network_scale);
  drawMetricTile(canvas, 8, 213, 46, "PING",
                 state.ping_ms >= 0 ? String(state.ping_ms) + "ms" : "--ms", color.orange);
  drawMetricTile(canvas, 62, 213, 46, "CLIENTS", intValue(state.clients));
  drawMetricTile(canvas, 116, 213, 46, "DISK", percentValue(storagePercent()), color.amber);
  drawLabel(canvas, "CPU " + percentValue(state.cpu), 8, 263, 1, color.dim);
  drawLabel(canvas, "MEM " + percentValue(state.memory), 67, 263, 1, color.dim);
  drawLabel(canvas, uptimeValue(), 162, 263, 1, color.dim, TR_DATUM);
  drawFooter(canvas, Page::Overview);
}

void drawNetwork(TFT_eSprite &canvas) {
  drawHeader(canvas, "NETWORK", state.demo ? "DEMO HEALTHY" : (state.online ? "HEALTHY" : "OFFLINE"),
             state.online || state.demo ? color.green : color.red);
  canvas.fillRoundRect(8, 29, 154, 43, 8, color.panel);
  canvas.fillCircle(22, 50, 7, dimColor(color.cyan, 28));
  canvas.drawCircle(22, 50, 7, color.cyan);
  drawLabel(canvas, "W", 22, 50, 1, color.cyan, MC_DATUM);
  canvas.drawLine(34, 50, 64, 50, color.cyan);
  canvas.fillTriangle(66, 50, 60, 46, 60, 54, color.cyan);
  canvas.fillCircle(82, 50, 7, dimColor(color.blue, 28));
  canvas.drawCircle(82, 50, 7, color.blue);
  drawLabel(canvas, "N", 82, 50, 1, color.blue, MC_DATUM);
  canvas.drawLine(94, 50, 124, 50, color.green);
  canvas.fillTriangle(126, 50, 120, 46, 120, 54, color.green);
  canvas.fillCircle(142, 50, 7, dimColor(color.green, 25));
  canvas.drawCircle(142, 50, 7, color.green);
  drawLabel(canvas, state.clients >= 0 ? String(state.clients) : "--", 142, 50, 1, color.green, MC_DATUM);
  drawLabel(canvas, "WAN DOWN", 8, 83, 1, color.cyan);
  drawFitNumber(canvas, floatValue(state.down_mbps), 162, 78, 105, 26, color.text, TR_DATUM);
  drawLabel(canvas, "WAN UP", 8, 111, 1, color.green);
  drawFitNumber(canvas, floatValue(state.up_mbps), 162, 106, 105, 26, color.text, TR_DATUM);
  canvas.fillRoundRect(8, 139, 154, 61, 7, color.panel);
  drawLabel(canvas, "PING 60S  " + intValue(state.ping_ms) + "ms", 15, 146, 1, color.dim);
  drawHistoryCurve(canvas, 15, 159, 140, 34, ping_history, color.orange, 2, false, 0.0f, 150.0f);
  String lan_value = "-- LINK";
  if (state.lan_speed_mbps >= 1000) lan_value = String(state.lan_speed_mbps / 1000) + "G LINK";
  else if (state.lan_speed_mbps > 0) lan_value = String(state.lan_speed_mbps) + "M LINK";
  else if (state.online) lan_value = "LINK UP";
  drawMetricTile(canvas, 8, 211, 73, "LAN", lan_value);
  const String wifi_value = state.online ? String(WiFi.RSSI()) + "dBm" : (state.demo ? "-48dBm" : "--dBm");
  drawMetricTile(canvas, 89, 211, 73, "WIFI", wifi_value);
  drawLabel(canvas, FNOS_HOST[0] ? String(FNOS_HOST) : String("192.168.9.23"), 8, 266, 1, color.dim);
  drawLabel(canvas, state.packet_loss >= 0 ? String(state.packet_loss) + "% LOSS" : "--% LOSS",
            162, 266, 1, state.packet_loss == 0 ? color.green : color.orange, TR_DATUM);
  drawFooter(canvas, Page::Network);
}

void drawStorage(TFT_eSprite &canvas) {
  const int used = storagePercent();
  drawHeader(canvas, "STORAGE", state.demo ? "DEMO PASS" : (state.online ? "ONLINE" : "OFFLINE"),
             state.online || state.demo ? color.green : color.red);
  canvas.drawArc(69, 76, 46, 36, 30, 330, color.guide, color.background, true);
  if (used > 0) canvas.drawArc(69, 76, 46, 36, 30, 30 + used * 3, color.amber, color.background, true);
  drawLabel(canvas, percentValue(used), 69, 73, 4, color.text, MC_DATUM);
  drawLabel(canvas, "USED", 69, 101, 1, color.dim, MC_DATUM);
  drawLabel(canvas, "TOTAL", 122, 48, 1, color.dim);
  drawLabel(canvas, isfinite(state.total_tb) && state.total_tb > 0 ? String(state.total_tb, 1) + "T" : "--.-T", 122, 60, 2, color.text);
  drawLabel(canvas, "FREE", 122, 87, 1, color.dim);
  const float free_tb = isfinite(state.total_tb) && isfinite(state.used_tb)
                            ? max(0.0f, state.total_tb - state.used_tb) : NAN;
  drawLabel(canvas, isfinite(free_tb) ? String(free_tb, 1) + "T" : "--.-T", 122, 99, 2, color.text);
  canvas.fillRoundRect(8, 133, 154, 70, 7, color.panel);
  drawLabel(canvas, "CAPACITY", 15, 140, 1, color.dim);
  drawLabel(canvas, "POOL", 15, 158, 1, color.text);
  canvas.fillRoundRect(49, 159, 104, 7, 3, color.guide);
  if (used >= 0) canvas.fillRoundRect(49, 159, max(1, used * 104 / 100), 7, 3, color.amber);
  drawLabel(canvas, "USED " + (isfinite(state.used_tb) ? String(state.used_tb, 1) + "T" : String("--.-T")),
            15, 181, 1, color.text);
  drawLabel(canvas, "FREE " + (isfinite(free_tb) ? String(free_tb, 1) + "T" : String("--.-T")),
            153, 181, 1, color.dim, TR_DATUM);
  drawLabel(canvas, "DISK TEMP", 8, 218, 1, color.orange);
  drawTemperature(canvas, state.disk_temp_c, 162, 210, 2,
                  state.disk_temp_c >= 0 ? color.text : color.dim, TR_DATUM);
  drawHistoryCurve(canvas, 8, 237, 154, 31, temp_history, color.orange, 2, false, 20.0f, 70.0f);
  const String raid_value = state.raid_status.length() ? "RAID " + state.raid_status : "RAID UNKNOWN";
  const uint16_t raid_color = state.raid_status == "HEALTHY" ? color.green
                              : state.raid_status.length() ? color.orange : color.dim;
  drawLabel(canvas, raid_value, 8, 275, 1, raid_color);
  drawLabel(canvas, isfinite(state.disk_read_mb_s) ? "R " + String(state.disk_read_mb_s, 1) + "MB/s" : "R --MB/s",
            162, 275, 1, color.blue, TR_DATUM);
  drawFooter(canvas, Page::Storage);
}

void drawSystem(TFT_eSprite &canvas) {
  drawHeader(canvas, "SYSTEM", state.demo ? "DEMO STABLE" : (state.online ? "STABLE" : "OFFLINE"),
             state.online || state.demo ? color.green : color.red);
  canvas.fillRoundRect(8, 31, 73, 74, 8, color.panel);
  drawLabel(canvas, "CPU", 16, 39, 1, color.blue);
  drawFitNumber(canvas, percentValue(state.cpu), 16, 53, 61, 32, color.text);
  drawTemperature(canvas, state.cpu_temp_c, 16, 90, 1,
                  state.cpu_temp_c < 0 ? color.dim
                  : state.cpu_temp_c < 75 ? color.green : color.orange);
  canvas.fillRoundRect(89, 31, 73, 74, 8, color.panel);
  drawLabel(canvas, "MEMORY", 97, 39, 1, color.cyan);
  drawFitNumber(canvas, percentValue(state.memory), 97, 53, 61, 32, color.text);
  drawLabel(canvas, isfinite(state.memory_used_gb) ? String(state.memory_used_gb, 1) + "G" : "--.-G",
            97, 90, 1, color.dim);
  canvas.fillRoundRect(8, 116, 154, 63, 7, color.panel);
  drawLabel(canvas, "CPU LOAD 60S", 15, 123, 1, color.dim);
  drawHistoryCurve(canvas, 15, 137, 140, 35, cpu_history, color.blue, 2, false, 0.0f, 100.0f);
  drawLabel(canvas, "DOCKER", 8, 194, 1, color.dim);
  const String docker = state.docker_running >= 0 && state.docker_total >= 0
                            ? String(state.docker_running) + "/" + String(state.docker_total) : String("--/--");
  drawLabel(canvas, docker, 162, 189, 2, color.text, TR_DATUM);
  canvas.fillRoundRect(8, 218, 154, 35, 7, color.panel_alt);
  canvas.fillCircle(18, 235, 4, color.green);
  drawLabel(canvas, intValue(state.docker_running) + " RUNNING", 28, 228, 1, color.text);
  const int stopped = state.docker_running >= 0 && state.docker_total >= 0
                          ? max(0, state.docker_total - state.docker_running) : -1;
  canvas.fillCircle(111, 235, 4, color.orange);
  drawLabel(canvas, intValue(stopped) + " STOP", 121, 228, 1, color.text);
  drawLabel(canvas, "UPTIME " + uptimeValue(), 8, 268, 1, color.dim);
  drawLabel(canvas, isfinite(state.load_1m) ? "LOAD " + String(state.load_1m, 2) : "LOAD --",
            162, 268, 1, color.dim, TR_DATUM);
  drawFooter(canvas, Page::System);
}

void drawNormal(TFT_eSprite &canvas) {
  drawHeader(canvas, "STATE", state.demo ? "DEMO NORMAL" : (state.online ? "NORMAL" : "OFFLINE"),
             state.online || state.demo ? color.green : color.red);
  canvas.fillRoundRect(8, 29, 154, 32, 8, dimColor(color.green, 18));
  canvas.fillCircle(20, 45, 4, color.green);
  drawLabel(canvas, state.online || state.demo ? "ONLINE HEALTHY" : "LINK LOST",
            30, 38, 2, state.online || state.demo ? color.green : color.red);
  drawLabel(canvas, "DOWNLOAD", 8, 74, 1, color.cyan);
  drawFitNumber(canvas, floatValue(state.down_mbps), 8, 84, 138, 48, color.text);
  drawLabel(canvas, "Mb/s", 162, 111, 1, color.dim, TR_DATUM);
  canvas.fillRoundRect(8, 133, 154, 63, 7, color.panel);
  drawLabel(canvas, "NETWORK 60S", 15, 140, 1, color.dim);
  drawHistoryCurve(canvas, 15, 153, 140, 37, down_history, color.cyan, 2, true);
  drawMetricTile(canvas, 8, 207, 46, "PING",
                 state.ping_ms >= 0 ? String(state.ping_ms) + "ms" : "--ms", color.orange);
  drawMetricTile(canvas, 62, 207, 46, "CPU", percentValue(state.cpu));
  drawMetricTile(canvas, 116, 207, 46, "DISK", percentValue(storagePercent()), color.amber);
  drawLabel(canvas, state.online || state.demo ? "ALL SERVICES READY" : "SERVICES UNKNOWN",
            8, 266, 1, state.online || state.demo ? color.green : color.red);
  drawLabel(canvas, state.packet_loss >= 0 ? String(state.packet_loss) + "% LOSS" : "--% LOSS",
            162, 266, 1, color.dim, TR_DATUM);
  drawFooter(canvas, Page::Normal);
}

void drawHighLoad(TFT_eSprite &canvas) {
  const float down = state.demo ? 842.7f : state.down_mbps;
  const float up = state.demo ? 96.8f : state.up_mbps;
  const int cpu = state.demo ? 82 : state.cpu;
  const int ping = state.demo ? 68 : state.ping_ms;
  const int disk = state.demo ? 87 : storagePercent();
  const bool high = state.demo || cpu >= 80 || disk >= 85 || (isfinite(down) && down >= 800.0f);
  drawHeader(canvas, "STATE", state.demo ? "DEMO HIGH LOAD" : (high ? "HIGH LOAD" : "LOAD WATCH"),
             high ? color.amber : color.green);
  canvas.fillRoundRect(8, 29, 154, 32, 8, dimColor(high ? color.amber : color.green, 16));
  canvas.fillCircle(20, 45, 4, high ? color.amber : color.green);
  drawLabel(canvas, high ? "HIGH LOAD" : "LOAD NORMAL", 30, 38, 2, high ? color.amber : color.green);
  drawLabel(canvas, "DOWNLOAD", 8, 74, 1, color.cyan);
  drawFitNumber(canvas, floatValue(down), 8, 84, 138, 48, color.text);
  drawLabel(canvas, "Mb/s", 162, 111, 1, color.dim, TR_DATUM);
  canvas.fillRoundRect(8, 133, 154, 63, 7, color.panel);
  drawLabel(canvas, high ? "TRAFFIC PEAK 60S" : "TRAFFIC WATCH 60S", 15, 140, 1,
            high ? color.amber : color.dim);
  if (state.demo) {
    drawSmoothCurve(canvas, 15, 153, 140, 37, HIGH_LOAD_CURVE, HISTORY_COUNT, color.cyan, 2, true);
  } else {
    drawHistoryCurve(canvas, 15, 153, 140, 37, down_history, color.cyan, 2, true);
  }
  drawMetricTile(canvas, 8, 207, 46, "PING", ping >= 0 ? String(ping) + "ms" : "--ms", color.orange);
  drawMetricTile(canvas, 62, 207, 46, "CPU", percentValue(cpu), high ? color.amber : color.text);
  drawMetricTile(canvas, 116, 207, 46, "DISK", percentValue(disk), high ? color.amber : color.text);
  drawLabel(canvas, high ? "THROTTLE WATCH" : "CAPACITY READY", 8, 266, 1,
            high ? color.amber : color.green);
  drawLabel(canvas, floatValue(up) + " UP", 162, 266, 1, color.orange, TR_DATUM);
  drawFooter(canvas, Page::HighLoad);
}

void drawTimeout(TFT_eSprite &canvas) {
  const bool timeout = state.demo || !state.online;
  drawHeader(canvas, "STATE", state.demo ? "DEMO TIMEOUT" : (timeout ? "TIMEOUT" : "ONLINE"),
             timeout ? color.red : color.green);
  canvas.fillRoundRect(8, 29, 154, 32, 8, dimColor(timeout ? color.red : color.green, 16));
  canvas.fillCircle(20, 45, 4, timeout ? color.red : color.green);
  drawLabel(canvas, timeout ? "CONNECTION TIMEOUT" : "LINK READY", 30, 38,
            timeout ? 1 : 2, timeout ? color.red : color.green);
  drawLabel(canvas, "DOWNLOAD", 8, 74, 1, timeout ? color.idle : color.cyan);
  drawFitNumber(canvas, timeout ? "--.-" : floatValue(state.down_mbps), 8, 84, 138, 48,
                timeout ? color.dim : color.text);
  drawLabel(canvas, "Mb/s", 162, 111, 1, color.idle, TR_DATUM);
  canvas.fillRoundRect(8, 133, 154, 63, 7, color.panel);
  drawLabel(canvas, timeout ? "NO REALTIME DATA" : "NETWORK 60S", 15, 140, 1,
            timeout ? color.red : color.dim);
  if (timeout) {
    drawSmoothCurve(canvas, 15, 153, 140, 37, TIMEOUT_CURVE, HISTORY_COUNT,
                    color.idle, 2, true, true);
  } else {
    drawHistoryCurve(canvas, 15, 153, 140, 37, down_history, color.cyan, 2, true);
  }
  drawMetricTile(canvas, 8, 207, 73, "LAST GOOD",
                 state.last_good_ms ? String((millis() - state.last_good_ms) / 1000) + "s ago" : "--");
  drawMetricTile(canvas, 89, 207, 73, "NEXT RETRY", timeout ? "5s" : "READY",
                 timeout ? color.red : color.green);
  drawLabel(canvas, timeout ? "API TIMEOUT" : "API CONNECTED", 8, 266, 1,
            timeout ? color.red : color.green);
  drawLabel(canvas, timeout ? "NO DATA" : "VALUES LIVE", 162, 266, 1, color.dim, TR_DATUM);
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
  const float displacement = page_position - page_target;
  const float acceleration = -2.0f * SPRING_OMEGA * page_velocity -
                             SPRING_OMEGA * SPRING_OMEGA * displacement;
  page_velocity += acceleration * delta;
  page_position += page_velocity * delta;
  if (fabsf(page_position - page_target) < .0005f && fabsf(page_velocity) < .002f) {
    page_position = page_target;
    page_velocity = 0.0f;
    if (fabsf(page_target) > 700.0f) {
      const float shift = floorf(page_target / PAGE_COUNT) * PAGE_COUNT;
      page_position -= shift;
      page_target -= shift;
    }
  }
  drawCurrentFrame();
#endif
}

void clearRealtimeValues() {
  state.cpu = state.memory = state.ping_ms = state.clients = -1;
  state.cpu_temp_c = state.disk_temp_c = state.lan_speed_mbps = -1;
  state.docker_running = state.docker_total = state.packet_loss = -1;
  state.used_tb = state.total_tb = state.memory_used_gb = NAN;
  state.load_1m = state.disk_read_mb_s = NAN;
  state.down_mbps = state.up_mbps = NAN;
  state.uptime_days = -1;
  state.raid_status = "";
}

String prometheusLabel(const String &line, const char *name) {
  const String marker = String(name) + "=\"";
  const int start = line.indexOf(marker);
  if (start < 0) return "";
  const int value_start = start + marker.length();
  const int end = line.indexOf('"', value_start);
  return end >= value_start ? line.substring(value_start, end) : String();
}

bool parsePrometheusMetrics(HTTPClient &http) {
  clearRealtimeValues();
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
      state.cpu = constrain(static_cast<int>(round(value)), 0, 100);
    } else if (name == "fnos_cpu_cpu_temp" || name == "fnos_cpu_temp") {
      state.cpu_temp_c = max(state.cpu_temp_c, static_cast<int>(round(value)));
    } else if (name == "fnos_cpu_cpu_loadavg_avg1min") {
      state.load_1m = static_cast<float>(value);
    } else if (name == "fnos_memory_mem_used") {
      memory_used = value;
    } else if (name == "fnos_memory_mem_total") {
      memory_total = value;
    } else if (name == "fnos_disk_temp") {
      state.disk_temp_c = max(state.disk_temp_c, static_cast<int>(round(value)));
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
        state.lan_speed_mbps = max(state.lan_speed_mbps, static_cast<int>(round(value)));
    } else if (name == "fnos_network_receive") {
      network_receive += value;
    } else if (name == "fnos_network_transmit") {
      network_transmit += value;
    } else if (name == "fnos_uptime") {
      state.uptime_days = max(0, static_cast<int>(value / 86400.0));
    } else if (name == "fnos_host_name_info") {
      const String host_name = prometheusLabel(line, "host_name");
      if (host_name.length()) state.hostname = host_name;
    }
  }

  if (isfinite(memory_used)) state.memory_used_gb = memory_used / 1073741824.0;
  if (isfinite(memory_used) && isfinite(memory_total) && memory_total > 0.0)
    state.memory = constrain(static_cast<int>(round(memory_used / memory_total * 100.0)), 0, 100);
  if (saw_storage_total && storage_total > 0.0) {
    state.total_tb = storage_total / 1099511627776.0;
    if (saw_storage_free) state.used_tb = max(0.0, storage_total - storage_free) / 1099511627776.0;
  }
  if (disk_read >= 0.0) state.disk_read_mb_s = disk_read / 1048576.0;
  state.down_mbps = network_receive * 8.0 / 1000000.0;
  state.up_mbps = network_transmit * 8.0 / 1000000.0;
  state.raid_status = smart_ok ? "HEALTHY" : "DEGRADED";
  return saw_fnos_metric;
}

bool fetchState() {
#if FNOS_DEMO_MODE
  const float phase = (millis() / 5000) * .35f;
  state.demo = true;
  state.online = false;
  state.cpu = 12 + static_cast<int>((sinf(phase) + 1.0f) * 3.0f);
  state.memory = 43;
  state.ping_ms = 24 + static_cast<int>((sinf(phase * .7f) + 1.0f) * 3.0f);
  state.clients = 14;
  state.cpu_temp_c = 48;
  state.disk_temp_c = 36;
  state.lan_speed_mbps = 1000;
  state.docker_running = 12;
  state.docker_total = 14;
  state.packet_loss = 0;
  state.used_tb = 8.6f;
  state.total_tb = 12.7f;
  state.memory_used_gb = 8.4f;
  state.load_1m = .42f;
  state.disk_read_mb_s = 4.0f;
  state.down_mbps = 128.6f + sinf(phase) * 8.0f;
  state.up_mbps = 12.4f + sinf(phase * .8f) * 1.2f;
  state.uptime_days = 127;
  state.hostname = "FNOS";
  state.raid_status = "HEALTHY";
  state.error = "";
  state.last_good_ms = max<uint32_t>(1, millis());
  return true;
#else
  state.demo = false;
  if (WiFi.status() != WL_CONNECTED) {
    state.online = false;
    state.error = "Wi-Fi unavailable";
    clearRealtimeValues();
    return false;
  }
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(String("http://") + FNOS_HOST + FNOS_API_PATH);
  if (strlen(FNOS_API_TOKEN) > 0) http.addHeader("Authorization", String("Bearer ") + FNOS_API_TOKEN);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    state.online = false;
    state.error = code > 0 ? "HTTP " + String(code) : "request timeout";
    clearRealtimeValues();
    http.end();
    return false;
  }
  if (!parsePrometheusMetrics(http)) {
    http.end();
    state.online = false;
    state.error = "metrics empty";
    clearRealtimeValues();
    return false;
  }
  http.end();
  state.online = true;
  state.error = "";
  state.last_good_ms = max<uint32_t>(1, millis());
  return true;
#endif
}

void announceTelemetry() {
  Serial.printf(
      "FNOS_STATE online=%s demo=%s cpu=%d memory=%d down=%.2f up=%.2f error=%s\n",
      state.online ? "yes" : "no", state.demo ? "yes" : "no", state.cpu, state.memory,
      state.down_mbps, state.up_mbps, state.error.length() ? state.error.c_str() : "none");
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

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(LCD_POWER_ON, OUTPUT);
  digitalWrite(LCD_POWER_ON, HIGH);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
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
#if !FNOS_DEMO_MODE
  WiFi.mode(WIFI_STA);
  WiFi.begin(FNOS_WIFI_SSID, FNOS_WIFI_PASSWORD);
#endif
  fetchState();
  announceTelemetry();
  seedHistories();
  initFrames();
  invalidateFrames();
  drawCurrentFrame();
  announcePage();
  last_refresh = millis();
}

void loop() {
  updateButton(previous_button, -1);
  updateButton(next_button, 1);
  if (millis() - last_refresh >= REFRESH_MS) {
    last_refresh = millis();
    fetchState();
    announceTelemetry();
    appendCurrentStateToHistory();
    invalidateFrames();
  }
  updatePageAnimation();
  delay(2);
}
