"""Board-owned handoff consumption and fallback contract."""
from pathlib import Path
import unittest
BSP_DISPLAY = Path(__file__).resolve().parents[1] / "components/esp-mosaico-bsp/onboard/display.c"
class BootHandoffTests(unittest.TestCase):
    def test_bsp_adopts_handoff_with_cold_initialization_fallback(self) -> None:
        source = BSP_DISPLAY.read_text(encoding="utf-8")
        self.assertIn("mosaico_boot_handoff_consume()", source)
        self.assertIn("s_handoff_init", source)
        self.assertIn("if (!boot_panel_ready) {", source)
        self.assertIn("ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(s_panel)", source)
        self.assertIn("ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true)", source)
