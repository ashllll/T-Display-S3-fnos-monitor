from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")


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
            'state.online || state.demo ? "RAID HEALTHY"',
            'state.online || state.demo ? "1G FULL"',
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
        self.assertIn("0.0f, 150.0f", SOURCE)
        self.assertIn("20.0f, 70.0f", SOURCE)

    def test_live_source_is_fnos_exporter_prometheus(self):
        self.assertIn('FNOS_API_PATH "/metrics"', SOURCE)
        self.assertIn("bool parsePrometheusMetrics(HTTPClient &http)", SOURCE)
        self.assertIn('name == "fnos_cpu_cpu_busy_all"', SOURCE)
        self.assertIn('name == "fnos_memory_mem_used"', SOURCE)
        self.assertIn('name == "fnos_store_array_fssize"', SOURCE)
        self.assertIn('name == "fnos_network_receive"', SOURCE)
        self.assertNotIn("deserializeJson", SOURCE)
        self.assertNotIn("space <= name_end", SOURCE)


if __name__ == "__main__":
    unittest.main()
