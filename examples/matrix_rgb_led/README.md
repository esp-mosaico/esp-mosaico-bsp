# 8×8 RGB Matrix LED Example

Drive an ESP-Mosaico 8×8 WS2812 matrix board with eight looping animations: plasma rainbow, spiral comet, expanding ripples, meteor rain, fire, bouncing rainbow balls, sweeping scan lines, and twinkling stars. Each effect runs for approximately 8 seconds.

## Hardware and Requirements

- ESP-Mosaico mainboard with an ESP32-S31 chip.
- An 8×8 WS2812 matrix daughterboard with a valid EEPROM descriptor (Matrix LED board type `0x11`), installed in either slot.
- ESP-IDF 6.2 or later.

The BSP automatically detects mainboard v1.0/v1.2 and initializes the subboard interface. Hardware v1.1 uses the v1.2 configuration. The mainboard LCD shows a 240×240 live preview of the LED matrix, the current effect name, and the selected slot on a dark blue background.

## Configuration

The example scans both slots and starts the effects only after validating a matrix board's EEPROM descriptor and claiming its slot. The detected slot selects GPIO48 (left) or GPIO47 (right) automatically. If both slots contain matrix boards, one is selected; this example drives one board at a time.

Without a valid matrix board, the LCD shows `Waiting for matrix board`. Removing the active board stops the effects and returns to this screen. Inserting a valid board in either slot starts playback again. Other board types and invalid descriptors do not start the LED driver.

Adjust brightness and animation timing in `main/main.c`:

| Parameter | Default | Description |
| --- | --- | --- |
| `BRIGHTNESS` | 128 | Global brightness, from 0 to 255 |
| `FRAME_DELAY_MS` | 30 | Delay between frames, in milliseconds |
| `EFFECT_DURATION_MS` | 8000 | Duration of each effect, in milliseconds |

Matrix coordinates start at LED0 as `(0,0)` in the left-slot orientation. LEDs use index `x * 8 + y`. The LCD preview places logical pixel 0 at the top-right and pixel 63 at the bottom-left. For the right slot, the example rotates LED output by 180 degrees so both slots show the same animation orientation; the LCD preview stays unchanged.

## Build and Run

From the repository root, run the following commands. Replace the ESP-IDF path and serial port with those for your setup:

```bash
source /path/to/esp-idf/export.sh
cd examples/matrix_rgb_led
idf.py build
unset ESPBAUD
idf.py -p /dev/ttyACM1 -b 1152000 flash
idf.py -p /dev/ttyACM1 monitor
```

After detecting a matrix board, the LED matrix plays the effects below while the LCD previews the same pixel data. The sequence repeats approximately every 64 seconds:

`plasma → spiral → ripples → meteor → fire → balls → scan → twinkle`

The serial monitor prints the current effect name, for example:

```text
matrix_ui: LCD preview ready: 240x240, 8x8 pixels
matrix_rgb: Waiting for a matrix LED board in either slot
matrix_led: 8x8 matrix ready: slot=left GPIO48
matrix_rgb: Playing effect: plasma
matrix_rgb: Playing effect: spiral
```

Press `Ctrl+]` to exit the serial monitor. If the matrix stays dark, check the power supply, slot connection, and the module identification messages in the serial monitor. An EEPROM with an invalid descriptor must be programmed correctly before the example will start playback.
