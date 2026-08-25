from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
EXAMPLE_CONFIG = (ROOT / "include" / "local_config.example.h").read_text(encoding="utf-8")
REFERENCE_DIR = ROOT / "docs" / "assets" / "ui-v3"


class UiContractTests(unittest.TestCase):
    def test_temperature_uses_explicit_degree_renderer(self):
        self.assertIn("void drawTemperature(", SOURCE)
        self.assertIn("drawTemperature(canvas, state.cpu_temp_c", SOURCE)
        self.assertIn("drawTemperature(canvas, state.disk_temp_c", SOURCE)
        self.assertNotIn('String(state.disk_temp_c) + "C"', SOURCE)

    def test_live_ui_does_not_derive_or_hardcode_telemetry(self):
        forbidden = (
            "42 + state.cpu / 5",
            "state.memory * .195f",
            '"R 4MB/s"',
            '"VOL2"',
        )
        for value in forbidden:
            with self.subTest(value=value):
                self.assertNotIn(value, SOURCE)

    def test_missing_storage_is_not_reported_as_zero_percent(self):
        self.assertIn("return -1;", SOURCE)
        self.assertIn("percentValue(storagePercent())", SOURCE)

    def test_metric_fonts_are_height_bounded(self):
        self.assertIn("canvas.fontHeight(font) <= max_height", SOURCE)

    def test_charts_use_metric_scales_and_offline_state(self):
        self.assertIn("if (!realtimeValid())", SOURCE)
        self.assertIn("0.0f, 100.0f", SOURCE)
        self.assertIn("0.0f, 1500.0f", SOURCE)
        self.assertIn("20.0f, 70.0f", SOURCE)

    def test_live_source_is_fnos_exporter_prometheus(self):
        self.assertIn('#define FNOS_API_PATH "/metrics"', EXAMPLE_CONFIG)
        self.assertIn('#error "Create include/local_config.h', SOURCE)
        self.assertIn("bool parsePrometheusMetrics(HTTPClient &http, NasState &target)", SOURCE)
        self.assertIn('name == "fnos_cpu_cpu_busy_all"', SOURCE)
        self.assertIn('name == "fnos_memory_mem_used"', SOURCE)
        self.assertIn('name == "fnos_store_array_fssize"', SOURCE)
        self.assertIn('name == "fnos_network_receive"', SOURCE)
        self.assertIn('name == "fnos_gpu_busy"', SOURCE)
        self.assertNotIn("deserializeJson", SOURCE)
        self.assertNotIn("space <= name_end", SOURCE)

    def test_backlight_stays_off_until_first_frame_and_is_limited(self):
        setup_source = SOURCE[SOURCE.index("void setup()") : SOURCE.index("void loop()")]
        self.assertIn("constexpr uint8_t BACKLIGHT_PERCENT = 50;", SOURCE)
        self.assertLess(setup_source.index("initBacklightOff();"), setup_source.index("Serial.begin(115200);"))
        self.assertLess(setup_source.index("drawCurrentFrame();"), setup_source.index("fadeInBacklight();"))
        self.assertLess(SOURCE.index("ledcWrite(BACKLIGHT_PWM_CHANNEL, 0);"),
                        SOURCE.index("ledcAttachPin(TFT_BL, BACKLIGHT_PWM_CHANNEL);"))
        self.assertNotIn("LCD_BL", SOURCE)
        self.assertNotIn("LCD_POWER_ON", SOURCE)

    def test_unsupported_exporter_fields_are_not_primary_tiles(self):
        self.assertNotIn('"CLIENTS"', SOURCE)
        self.assertNotIn('"DOCKER"', SOURCE)
        self.assertNotIn('"% LOSS"', SOURCE)
        self.assertIn('"API RT"', SOURCE)

    def test_refreshes_each_second_without_overlapping_requests(self):
        self.assertIn("constexpr uint32_t REFRESH_MS = 1000;", SOURCE)
        self.assertIn("HISTORY_WINDOW_SECONDS = HISTORY_COUNT * REFRESH_MS / 1000", SOURCE)
        self.assertIn("vTaskDelayUntil(&next_wake, refresh_ticks);", SOURCE)
        self.assertIn("next_wake = xTaskGetTickCount();", SOURCE)
        loop_source = SOURCE[SOURCE.index("void loop()") :]
        self.assertIn("consumeTelemetry();", loop_source)
        self.assertNotIn("fetchState(", loop_source)
        self.assertNotIn('"NEXT RETRY", timeout ? "5s"', SOURCE)

    def test_labels_are_width_bounded(self):
        self.assertIn("String fitLabel(", SOURCE)
        self.assertIn("void drawBoundedLabel(", SOURCE)
        self.assertIn("void drawBoundedPair(", SOURCE)
        self.assertIn("void drawEdgePair(", SOURCE)
        self.assertIn("fitLabel(canvas, title, 2, title_width)", SOURCE)

    def test_dynamic_values_and_units_have_non_overlapping_width_budgets(self):
        self.assertIn("void drawValueUnit(", SOURCE)
        self.assertIn("width - unit_width - gap", SOURCE)
        self.assertIn("const String fitted = found ? text : fitLabel", SOURCE)
        self.assertEqual(SOURCE.count("drawValueUnit(canvas"), 5)
        self.assertIn("void drawHeroBlock(", SOURCE)
        self.assertNotIn('drawLabel(canvas, "Mb/s"', SOURCE)

    def test_render_reference_panels_and_state_bands_are_shared_components(self):
        self.assertIn("void drawPanel(", SOURCE)
        self.assertGreaterEqual(SOURCE.count("drawPanel(canvas"), 11)
        self.assertIn("void drawTrendPanel(", SOURCE)
        self.assertIn("void drawStateBand(", SOURCE)
        self.assertEqual(SOURCE.count("drawStateBand(canvas"), 3)
        self.assertIn("canvas.drawRoundRect(ui::edge, ui::state_y, ui::content_width, ui::state_h, 6,", SOURCE)
        self.assertIn("canvas.drawFastVLine(84, 174, 21, color.guide);", SOURCE)
        self.assertIn("int16_t max_width = SCREEN_W", SOURCE)
        self.assertIn('UI_BUILD_ID = "render-v3.0-seven-page"', SOURCE)
        self.assertIn("canvas.drawRect(0, 0, SCREEN_W, SCREEN_H, color.guide);", SOURCE)
        self.assertIn('Serial.printf("UI_BUILD %s pages=%u adaptive=yes', SOURCE)

    def test_page_motion_is_interruptible_and_not_blocked_by_network(self):
        self.assertIn("constexpr float SPRING_RESPONSE = 0.28f;", SOURCE)
        self.assertIn("void advancePageSpring(float delta)", SOURCE)
        self.assertIn("expf(-omega * delta)", SOURCE)
        self.assertIn("page_velocity + omega * displacement", SOURCE)
        self.assertNotIn("const float acceleration =", SOURCE)
        self.assertIn('xTaskCreate(telemetryWorker, "fnos-telemetry"', SOURCE)
        self.assertIn("page_target += direction;", SOURCE)
        self.assertIn("telemetry_redraw_pending = true;", SOURCE)

    def test_cuktech_v2_uses_semantic_tokens_and_stable_identity_colors(self):
        self.assertIn("namespace ui {", SOURCE)
        self.assertIn("constexpr int16_t edge = 8;", SOURCE)
        self.assertIn('drawLabel(canvas, "UPLOAD", 8, 96, 1, color.blue);', SOURCE)
        self.assertIn('drawBoundedLabel(canvas, "WAN UP", 8, 111, 54, 1, color.blue);', SOURCE)
        self.assertIn('drawTopologyNode(canvas, 9, "NAS", color.blue', SOURCE)
        self.assertIn('drawTopologyNode(canvas, 64, "WIFI", color.cyan', SOURCE)
        self.assertIn('drawTopologyNode(canvas, 118, "LCD", color.lime', SOURCE)
        self.assertIn("canvas.fillRoundRect(x - 4, ui::footer_indicator_y - 2, 8, 4", SOURCE)

    def test_v3_has_fixed_non_overlapping_vertical_regions(self):
        expected = {
            "header_bottom": 26,
            "state_y": 30,
            "state_h": 32,
            "hero_label_y": 74,
            "hero_value_y": 84,
            "hero_unit_y": 111,
            "trend_y": 134,
            "trend_h": 63,
            "tiles_y": 207,
            "summary_y": 266,
            "footer_y": 294,
        }
        for name, value in expected.items():
            with self.subTest(name=name):
                self.assertIn(f"constexpr int16_t {name} = {value};", SOURCE)
        self.assertLess(expected["header_bottom"], expected["state_y"])
        self.assertLess(expected["state_y"] + expected["state_h"], expected["hero_label_y"])
        self.assertLess(expected["hero_unit_y"], expected["trend_y"])
        self.assertLess(expected["trend_y"] + expected["trend_h"], expected["tiles_y"])
        self.assertLess(expected["tiles_y"] + 38, expected["summary_y"])
        self.assertLess(expected["summary_y"], expected["footer_y"])

    def test_all_seven_page_functions_use_v3_components(self):
        for name in ("Overview", "Network", "Storage", "System", "Normal", "HighLoad", "Timeout"):
            self.assertIn(f"void draw{name}(TFT_eSprite &canvas)", SOURCE)
        self.assertEqual(SOURCE.count("drawPageAccent(canvas"), 7)
        self.assertEqual(SOURCE.count("case Page::"), 7)
        self.assertEqual(SOURCE.count("drawHeroBlock(canvas"), 3)
        self.assertEqual(SOURCE.count("drawTrendPanel(canvas"), 2)

    def test_seven_native_resolution_reference_images_exist(self):
        names = (
            "01-overview-170x320", "02-network-170x320", "03-storage-170x320",
            "04-system-170x320", "05-normal-170x320", "06-high-load-170x320",
            "07-timeout-170x320",
        )
        for name in names:
            with self.subTest(name=name):
                self.assertTrue((REFERENCE_DIR / f"{name}.svg").is_file())
                self.assertTrue((REFERENCE_DIR / f"{name}.png").is_file())
                svg = (REFERENCE_DIR / f"{name}.svg").read_text(encoding="utf-8")
                self.assertIn('width="170" height="320" viewBox="0 0 170 320"', svg)

    def test_data_trust_distinguishes_warming_live_stale_and_offline(self):
        self.assertIn("enum class DataTrust : uint8_t { Warming, Live, Stale, Offline };", SOURCE)
        self.assertIn("constexpr uint32_t STALE_AFTER_MS = REFRESH_MS * 3;", SOURCE)
        self.assertIn("constexpr uint32_t OFFLINE_AFTER_MS = REFRESH_MS * 10;", SOURCE)
        self.assertIn('case DataTrust::Warming: return "WARMING";', SOURCE)
        self.assertIn('return "STALE " + String(', SOURCE)
        self.assertIn('case DataTrust::Offline: return "OFFLINE";', SOURCE)
        self.assertIn("state.consecutive_failures < 3", SOURCE)
        self.assertIn("candidate.consecutive_failures = 0;", SOURCE)
        self.assertIn("NasState candidate = target;", SOURCE)
        self.assertIn("parsePrometheusMetrics(http, candidate)", SOURCE)
        self.assertIn("target = candidate;", SOURCE)
        setup_source = SOURCE[SOURCE.index("void setup()") : SOURCE.index("void loop()")]
        self.assertNotIn("fetchState(state);", setup_source)


if __name__ == "__main__":
    unittest.main()
