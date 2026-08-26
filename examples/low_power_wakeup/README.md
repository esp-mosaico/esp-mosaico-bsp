# Low-power wakeup template

App-shaped deep-sleep template for ESP-Mosaico:

1. Log the wake cause (`timer` / power-on/reset)
2. Blink the status LED briefly while “awake”
3. Arm the timer wake source
4. `bsp_power_prepare_sleep()` → cut rails → `bsp_power_enter_deep_sleep()`

This is a minimal loop you can copy into a product firmware.

## Hardware

- ESP-Mosaico
- ESP-IDF >= 5.5 / 6.x

## Build and run

```sh
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

After boot the board works for a few seconds, then sleeps. It wakes on:

- Timer (`WAKE_TIMER_SEC`, default 10 s), or
- CHIP_PU / reset (logged as `power-on/reset`)

The Boot button is GPIO61. On ESP32-S31 that pad is not an RTC IO, so EXT1
cannot use it for deep sleep. Press reset for an immediate wake.

See skill `add-low-power` for the BSP shutdown sequence and validation
boundaries.
