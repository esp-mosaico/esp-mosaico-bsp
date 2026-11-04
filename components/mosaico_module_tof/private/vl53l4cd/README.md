# VL53L4CD Ultra Lite Driver

Backend for the default **VL53L4CDV0DH/1** sensor. The module's common API selects
this driver after reading chip ID `0xEBAA` from 16-bit register `0x010F`.

`vl53l4cd_api.c` and `vl53l4cd_api.h` are from STMicroelectronics
[x-cube-tof1](https://github.com/STMicroelectronics/x-cube-tof1/tree/c12709c3ac802d91dd2a215178f53cb94c024709/Drivers/BSP/Components/vl53l4cd/modules),
commit `c12709c3ac802d91dd2a215178f53cb94c024709` (ULD 2.2.2).
The upstream licensing notice is preserved in [LICENSE.md](LICENSE.md).
The full [BSD terms](BSD-3-Clause.txt) are retained from ST's
[VL53L4CD Arduino distribution](https://github.com/stm32duino/VL53L4CD/blob/main/LICENSE.md).

Local changes:

- `platform.h` and `platform.c` implement the required ULD port using the shared
  ESP-IDF I2C bus, 16-bit register addresses, big-endian values, and FreeRTOS waits.
  The first I2C error is retained so the application can report it and reconnect.
- `VL53L4CD_GetResult()` guards its per-SPAD divisions when communication fails
  or the reported SPAD count is zero. The application marks zero-SPAD samples
  invalid.

The offset and cross-talk calibration extensions are not included. Initialization
uses the sensor's factory settings and the ULD's normal VHV initialization.
