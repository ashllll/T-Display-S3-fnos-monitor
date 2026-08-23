from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
EXAMPLE_CONFIG = (ROOT / "include" / "local_config.example.h").read_text(encoding="utf-8")


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
        self.assertIn("void drawEdgePair(", SOURCE)
        self.assertIn("fitLabel(canvas, title, 2, title_width)", SOURCE)

    def test_page_motion_is_interruptible_and_not_blocked_by_network(self):
        self.assertIn("constexpr float SPRING_RESPONSE = 0.28f;", SOURCE)
        self.assertIn('xTaskCreate(telemetryWorker, "fnos-telemetry"', SOURCE)
        self.assertIn("page_target += direction;", SOURCE)
        self.assertIn("telemetry_redraw_pending = true;", SOURCE)


if __name__ == "__main__":
    unittest.main()
