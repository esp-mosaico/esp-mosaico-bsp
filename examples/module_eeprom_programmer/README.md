# Module EEPROM Programmer

A touchscreen example for programming and verifying the AT24C02 EEPROM on Mosaico modules.

## Requirements

- ESP-Mosaico core board V1.0 or V1.2; the BSP selects the hardware mapping automatically.
- One module connected to an expansion slot.
- ESP-IDF 6.2 or later with ESP32-S31 support.

## Build and Flash

Activate your ESP-IDF environment, then run:

```bash
cd examples/module_eeprom_programmer
idf.py build
idf.py -p /dev/ttyACM1 -b 1152000 flash monitor
```

Replace `/dev/ttyACM1` with your board's serial port.

## Usage

1. Connect one module at a time.
2. Select its board type on the touchscreen.
3. Tap **Start Write**.
4. Check for **Write and verify OK** after the EEPROM is read back and compared.

Supported profiles: Camera, Button LED, Sensor, TOF, Matrix LED, Thermal, Relay, IO Test, and Interaction.

The example probes `0x50` first, then `0x51` if no device responds. It does not write automatically at startup.

Profiles use module hardware version V1.2, software version V1.0, and vendor ID, board ID, and serial number of `1`. These values are independent of the core board revision. Adjust the defaults in `main/eeprom_program.c` as needed.
