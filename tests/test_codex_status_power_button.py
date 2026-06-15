import unittest
from pathlib import Path


SKETCH_DIR = Path("Examples/Arduino/12_Codex_Status")
SKETCH_FILE = SKETCH_DIR / "12_Codex_Status.ino"


class CodexStatusPowerButtonTests(unittest.TestCase):
    def test_sketch_initializes_power_button_hold_circuit(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn('src/button_bsp/button_bsp.h', sketch)
        self.assertIn('src/tca9554/esp_io_expander_tca9554.h', sketch)
        self.assertIn('button_Init();', sketch)
        self.assertIn('tca9554_init();', sketch)
        self.assertIn('xTaskCreatePinnedToCore(power_button_task', sketch)

    def test_long_press_releases_power_hold_pin(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn('xEventGroupWaitBits(pwr_groups', sketch)
        self.assertIn('get_bit_button(event_bits, 1)', sketch)
        self.assertIn('esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_6, 0)', sketch)

    def test_button_and_tca9554_drivers_are_local_to_sketch(self):
        expected_files = [
            SKETCH_DIR / "src/button_bsp/button_bsp.h",
            SKETCH_DIR / "src/button_bsp/button_bsp.c",
            SKETCH_DIR / "src/button_bsp/multi_button.h",
            SKETCH_DIR / "src/button_bsp/multi_button.c",
            SKETCH_DIR / "src/tca9554/esp_io_expander.h",
            SKETCH_DIR / "src/tca9554/esp_io_expander.c",
            SKETCH_DIR / "src/tca9554/esp_io_expander_tca9554.h",
            SKETCH_DIR / "src/tca9554/esp_io_expander_tca9554.c",
        ]

        for path in expected_files:
            with self.subTest(path=path):
                self.assertTrue(path.is_file())


if __name__ == "__main__":
    unittest.main()
