[English](README.md) | [简体中文](README_CN.md)

# ToF Distance Example

Displays independent left/right distance readings and recognizes directional
hand swipes on the ESP-Mosaico LCD. The default sensor is **VL53L4CDV0DH/1**
(VL53L4CD); **VL53L0CXV0DH/1** (VL53L0X) is also supported. Firmware automatically
selects the matching driver from the chip ID; supported models can be mixed.

## Hardware

- ESP-Mosaico / ESP32-S31 with PSRAM and an ESP-IDF 6.2 or newer environment.
- One or two ToF boards with valid EEPROM descriptors (type `0x10`). If needed,
  use [module_eeprom_programmer](../module_eeprom_programmer) to program them.
- One board is sufficient for ranging; directional gestures require both slots.

Both sensors have factory address `0x29`. The example assigns independent
addresses automatically:

| Slot | XSHUT | Sensor address (7-bit) |
|---|---|---|
| Left | GPIO48 | `0x30` |
| Right | GPIO47 | `0x31` |

Both slots are reserved for ToF boards while this example runs, because it
controls their XSHUT pins. Do not connect other module types in these slots.

## Build and Flash

Activate the ESP-IDF environment, then run from the repository root:

```sh
cd examples/tof_distance
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the board's serial port.

## Usage

The two cards show each slot's sensor model, address, distance in millimeters,
status, signal/ambient count rates (MCPS), and range bar. Invalid readings show
`--`; an empty field shows `NO TARGET`. The range bar covers 0–2000 mm; valid
readings beyond that scale still display their numeric value. Touch is disabled.

For gestures:

1. Connect both sensors and clear the sensing area. Wait for
   `READY - WAVE LEFT OR RIGHT`. An empty field with both cards showing `--`
   can still become ready; a measurable background is not required.
2. Move a hand within 30 cm across the left sensor, then the right, and clear
   both fields: `SWIPE RIGHT` and the light bar indicate a rightward swipe.
   Reverse the order for `SWIPE LEFT`.
3. Wait for the detector to rearm before the next swipe. Direction follows the
   mainboard's left/right slots.

| Message | Meaning |
|---|---|
| `WAIT FOR TWO SENSORS` | A sensor is missing or its data is unavailable |
| `HOLD STILL / CLEAR SENSORS` | Clear the sensing area or keep the background stable |
| `READY - WAVE LEFT OR RIGHT` | Ready for a gesture |
| `SWIPE RIGHT` / `SWIPE LEFT` | Last recognized gesture |

Both models use continuous ranging with a 20 ms measurement budget. Sampling
rate depends on the sensor and bus conditions. Disconnection or a communication
failure clears the affected card; reconnection is handled automatically.

## Configuration

Run `idf.py menuconfig` and open **ToF Distance Demo**:

| Setting | Default |
|---|---|
| `CONFIG_TOF_GESTURE_NEAR_MM` — hand detection distance | 300 mm |
| `CONFIG_TOF_GESTURE_THRESHOLD_MM` — distance change for background-based gestures | 60 mm |
| `CONFIG_TOF_GESTURE_WINDOW_MS` — gesture completion window | 1500 ms |
| `CONFIG_TOF_GESTURE_DIAGNOSTICS` — sampling diagnostics every five seconds | Off |

Lower thresholds increase sensitivity and may increase false triggers. Keep
nearby objects outside the hand detection zone.

### Distance Offset

Offsets are configured independently for each model and slot in the same menu:

| Model | Left slot | Right slot | Default |
|---|---|---|---|
| VL53L4CD | `CONFIG_TOF_L4CD_LEFT_OFFSET_ADJUST_MM` | `CONFIG_TOF_L4CD_RIGHT_OFFSET_ADJUST_MM` | 0 mm |
| VL53L0X | `CONFIG_TOF_DISTANCE_OFFSET_ADJUST_MM` | `CONFIG_TOF_DISTANCE_RIGHT_OFFSET_ADJUST_MM` | −37 mm |

The VL53L0X default is specific to the original board. Check each sensor at known
distances before applying compensation; use 0 for no additional adjustment.
Negative offsets reduce the measured distance. Compensation affects both the
display and logs and is stored in firmware configuration.

## Logs and References

Startup logs list responding I²C addresses. ToF sensors are held in shutdown
during this scan and appear later at their assigned addresses. Valid distances
are logged at most once per second per slot; this is not the sampling rate.
Enable sampling diagnostics to investigate missed gestures.

- [ToF component API and driver documentation](../../components/mosaico_module_tof/README.md)
- [VL53L4CD datasheet](https://www.st.com/resource/en/datasheet/vl53l4cd.pdf)
- [VL53L0X datasheet](https://www.st.com/resource/en/datasheet/vl53l0x.pdf)
