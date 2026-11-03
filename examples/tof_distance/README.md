# ToF Distance Example

Uses **VL53L4CDV0DH/1** (VL53L4CD) as the default sensor and also supports **VL53L0CXV0DH/1** (VL53L0X). The firmware reads the chip ID and automatically switches to the matching initialization and measurement steps. The application displays a live ranging dashboard on the 480 × 480 screen, waits 100 ms after each measurement, and logs valid results at most once per second, starting with the first valid sample.

The interface uses a dark blue background with mint green accents: a large central distance reading, a fixed 0–2 m visual scale, a moving scale marker, and cards for return signal and ambient light levels (MCPS). The header shows the detected sensor model and active slot. Invalid measurements clear the previous distance, hide the target marker, and show the status code. Initialization or communication failures leave a visible error message. English labels avoid the need for a large Chinese font.

Display initialization, brightness, LVGL locking, power, and slot mapping use the current BSP APIs. The BSP selects the configuration for the hardware revision; the example does not duplicate revision-specific GPIO definitions. Supported revisions follow the BSP: v1.0 and v1.2, with v1.1 using the v1.2 mapping. Touch is disabled.

## Hardware and Usage

- ESP-Mosaico / ESP32-S31 with ESP-IDF 6.2 or newer.
- One ToF board in either slot: **VL53L4CDV0DH/1 by default**, or the compatible **VL53L0CXV0DH/1**. The full order codes are documented in the ST [VL53L4CD datasheet](https://www.st.com/resource/en/datasheet/vl53l4cd.pdf) and [VL53L0X datasheet](https://www.st.com/resource/en/datasheet/vl53l0x.pdf).
- A valid ToF module EEPROM descriptor (type `0x10`) is required for either sensor. Use [`module_eeprom_programmer`](../module_eeprom_programmer) if the descriptor has not been programmed.
- Connect only one ToF board to avoid an address conflict at `0x29` on the shared bus.

After activating the ESP-IDF environment:

```bash
cd examples/tof_distance
idf.py build
idf.py -p PORT flash monitor
```

The module manager runs continuously. The application waits for a valid ToF descriptor in either slot, claims the slot, identifies and initializes the sensor, and displays its actual model and active slot above the gauge. No module at boot is a normal waiting state.

At every startup, the application scans the shared subboard I2C bus immediately after module-manager initialization and before waiting for or claiming a module. The scan runs even with no module connected or no valid EEPROM descriptor. It probes all non-reserved 7-bit addresses (`0x08`–`0x77`) and logs each responding address, the device count (including zero), and any probe errors. Both VL53L0X and VL53L4CD use `0x29`, and the module EEPROM uses `0x50` (left slot) or `0x51` (right slot). Only powered devices that are out of reset can respond; an address acknowledgement does not establish that sensor calibration or ranging works.

Removing the board or encountering a measurement communication error clears the displayed values, closes the sensor handle, resets the mapped XSHUT GPIO, and releases the slot. The application then resumes discovery automatically. Reinsert the board into either slot to resume measurements without resetting the mainboard. Initialization failures are retried with a one-second delay. Cleanup failures retain ownership and are retried before another sensor is opened.

Invalid range results do not indicate removal: the screen shows the range status while measurement continues. Valid distances are logged as `distance=... mm` at most once per second; this limit applies only to serial output. Slot presence is checked through the module manager before and after each blocking measurement. Its normal EEPROM debounce remains active while the slot is claimed.

This supports moving one ToF board between the two slots, not simultaneous operation of two boards sharing address `0x29`.

## Automatic Sensor Selection

"Default" refers to the standard module hardware, **VL53L4CDV0DH/1**. The same firmware supports the compatible **VL53L0CXV0DH/1** through chip-ID detection, with no manual driver selection or rebuilding required when switching supported modules.

| Hardware | Chip-ID check | Automatically selected initialization and read sequence |
| --- | --- | --- |
| **VL53L4CDV0DH/1 — default** | 16-bit register `0x010F` = `0xEBAA` | ST ULD initialization with VHV → 33 ms continuous ranging → poll data-ready (up to 500 ms) → read result → clear interrupt |
| **VL53L0CXV0DH/1 — compatible** | 8-bit register `0xC0` = `0xEE` | PAL initialization → 33 ms timing budget → VHV/phase reference calibration → default profile and single-shot mode → trigger and read each measurement |

After XSHUT reset, the driver first checks the VL53L0X ID at `0xC0`; if it does not match, it checks the VL53L4CD ID at `0x010F` using 16-bit register addressing. Unknown IDs return `ESP_ERR_NOT_SUPPORTED`. Initialization failures trigger cleanup and retry. The application uses `mosaico_tof_open()`, `mosaico_tof_read()`, and `mosaico_tof_close()` for either model, with a common result format for the dashboard. See the [component API documentation](../../components/mosaico_module_tof/README.md) for details.

Both sensors acknowledge at `0x29`, and the module EEPROM describes both as a ToF board. Successful address scanning therefore needs to be followed by chip-ID identification. Applying the VL53L0X calibration sequence to a VL53L4CD causes initialization failures such as `VL53L0X_PerformRefCalibration ... PAL status -7`; the automatic selection prevents that driver mismatch.

## Runtime Logs

After successful initialization, the driver prints the actual model and slot (`VL53L4CD ready` or `VL53L0X ready`). Raw identification details use Debug logging. Unsupported IDs and communication errors are reported explicitly. Valid measurement logs are limited to one line per second, and the unconditional startup I2C scan remains enabled. Temporary register dumps and transport-comparison logs are removed.

`CONFIG_FREERTOS_HZ=1000` is provided in the defaults so short driver delays have millisecond tick resolution.

## Validation Scope

With a v1.2 mainboard on `/dev/ttyUSB0`, a replacement module identified as VL53L4CD (`0xEBAA` at 16-bit register `0x010F`). The previous VL53L0X-only driver timed out during reference calibration. The updated firmware selected the VL53L4CD backend, initialized successfully, and reported valid changing distances of 72–593 mm during observation. No calibration timeouts or application crashes were observed after the fix. Host checks under UBSan covered 16-bit addressing, byte order, result conversion, zero-SPAD samples, I2C failure handling, and short waits at a 100 Hz tick rate. The retained VL53L0X path builds but has not been retested on hardware after adding automatic model selection; complete unplug/replug validation for the new backend remains outstanding.

The serial version was tested with the left slot of the v1.1 mainboard connected through ACM1. The display version was built and flashed to the same board, and logs confirmed successful display, LVGL, and ToF initialization. During observation, measurements consistently returned invalid target status `4`, with no crashes. The rendered interface has not been visually verified from a screen capture or photo. Those earlier runs did not cover other mainboard revisions, the right slot, or removal behavior.

The hot-plug lifecycle passed an ESP32-S31 build and host fault-injection checks covering discovery in both slots, removal during a read, initialization and communication failures, cleanup retries, and continued sampling after invalid ranges. The updated firmware was flashed to ACM1. Hardware logs confirmed right-slot discovery, valid measurements, and successful lease release after communication loss followed by an absent-slot event. A complete unplug/replug cycle in each slot still requires a continuous hardware capture.
