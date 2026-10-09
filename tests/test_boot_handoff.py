"""Board-owned handoff consumption and fallback contract."""
from pathlib import Path
import unittest
import subprocess
import tempfile
import shutil
BSP_DISPLAY = Path(__file__).resolve().parents[1] / "components/esp-mosaico-bsp/onboard/display.c"
class BootHandoffTests(unittest.TestCase):
    def test_bsp_adopts_handoff_with_cold_initialization_fallback(self) -> None:
        source = BSP_DISPLAY.read_text(encoding="utf-8")
        self.assertIn("mosaico_boot_handoff_consume()", source)
        self.assertIn("s_handoff_init", source)
        self.assertIn("if (!boot_panel_ready) {", source)
        self.assertIn("ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(s_panel)", source)
        self.assertIn("ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true)", source)

    def test_bootloader_publisher_and_bsp_consumer_share_wire_protocol(self) -> None:
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        root = BSP_DISPLAY.parents[3]
        producer = root.parent / "esp-mosaico-utils/esp-mosaico-recovery/firmware/recovery/bootloader_components/main/mosaico_boot_handoff.h"
        if not producer.is_file():
            self.skipTest("Adjacent utils checkout unavailable")
        consumer = root / "components/esp-mosaico-bsp/include/bsp/mosaico_boot_handoff.h"
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / "soc").mkdir()
            (folder / "soc/lp_system_reg.h").write_text("#define LP_SYSTEM_REG_LP_STORE15_REG 60u\n")
            (folder / "soc/soc.h").write_text(
                "#include <stdint.h>\nextern uint32_t registers[32];\n"
                "#define REG_READ(a) registers[(a)/4]\n"
                "#define REG_WRITE(a,v) (registers[(a)/4]=(v))\n")
            (folder / "producer.c").write_text(
                f'#include "{producer}"\n'
                'void publish(void) { mosaico_boot_handoff_publish(); }\n')
            (folder / "consumer.c").write_text(
                f'#include "{consumer}"\n'
                'bool consume(void) { return mosaico_boot_handoff_consume(); }\n')
            (folder / "compat.c").write_text(
                f'#include "{root}/components/mosaico_boot_splash/include/mosaico_boot_handoff.h"\n'
                'bool compat_consume(void) { return mosaico_boot_handoff_consume(); }\n')
            (folder / "main.c").write_text(
                '#include <assert.h>\n#include <stdint.h>\n#include <stdbool.h>\n'
                'uint32_t registers[32]; void publish(void); bool consume(void); bool compat_consume(void);\n'
                'int main(void) { assert(!consume()); publish(); assert(consume()); '
                'assert(!consume()); publish(); assert(compat_consume()); assert(!consume()); '
                'registers[15]=123; assert(!consume()); '
                'assert(registers[15]==0); return 0; }\n')
            binary = folder / "check"
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(folder), "-I", str(root / "components/esp-mosaico-bsp/include"),
                str(folder / "producer.c"), str(folder / "compat.c"),
                str(folder / "consumer.c"), str(folder / "main.c"),
                "-o", str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True)
