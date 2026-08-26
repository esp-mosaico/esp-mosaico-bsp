# Camera LCD Preview

This example discovers and claims the Mosaico Camera subboard, captures OV3640
UYVY frames, converts them to RGB565 with the PPA, and displays a live
480 x 480 preview on the onboard CO5300 LCD.

The example intentionally uses the high-level `mosaico_camera` API. Application
code does not need to initialize SCCB, XCLK, DVP, or the subboard manager
separately.

## Hardware

- ESP-Mosaico
- Camera subboard installed in the **LEFT** slot and aligned with the board
  silkscreen
- UART console; USB Serial/JTAG is disabled because its D- pad
  shares GPIO33 with camera D2

The example sets `allow_unidentified` so the sensor comes up even when the
subboard EEPROM carries no valid descriptor, which is the usual state on
bring-up hardware. Production code should keep it off and rely on the
descriptor.

## Build and run

```sh
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

After the camera is detected, the LCD continuously shows a centered square crop
of the camera image. If capture times out three times consecutively, the example
first restarts the stream. If frames still do not arrive, it releases the Camera
subboard and returns to automatic discovery so unplug/replug can recover.
