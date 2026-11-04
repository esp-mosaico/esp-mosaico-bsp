[English](README.md) | [简体中文](README_CN.md)

# Thermal Camera Example

Displays MLX90640 32 × 24 thermal images on the ESP-Mosaico LCD with a full-width
heatmap, automatic temperature range, and touch temperature measurement.

## Hardware

- ESP-Mosaico / ESP32-S31 with PSRAM and an ESP-IDF 6.2 or newer environment.
- One MLX90640 thermal board in either slot, with a valid Thermal Board EEPROM
  descriptor (type `0x12`). If needed, use
  [module_eeprom_programmer](../module_eeprom_programmer) to program the descriptor.
- Subboard I²C: SDA=GPIO0, SCL=GPIO1; sensor address `0x33`. Connect only one
  thermal board at a time.

## Build and Flash

Activate the ESP-IDF environment, then run from the repository root:

```sh
cd examples/thermal_camera
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the board's serial port.

## Usage

- The header shows `ESP-Mosaico | Module Thermal Camera`. The heatmap fills the
  screen width with equal space above and below; orientation follows the slot.
- Tap the heatmap to select a point. A white box marks the position, with its
  temperature displayed above it. Drag to move the selection; release to keep it
  selected while the temperature updates.
- The footer shows ambient, minimum, maximum, and center temperatures, plus the
  frame number. Enlarging the image does not increase the 32 × 24 sensor resolution.
- Disconnecting the module clears the image and selection. Imaging resumes
  automatically after reconnection; tap again to select a point.

## Configuration

Defaults: 400 kHz I²C, 16 Hz subpage refresh (about 8 complete images per second),
0.95 emissivity, and reflected temperature estimated as ambient minus 8°C.
Adjust the sensor configuration in `main/main.c` for the target material and
environment.

See the [thermal component documentation](../../components/mosaico_module_thermal/README.md)
for sensor specifications and APIs.
