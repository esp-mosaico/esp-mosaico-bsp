# BMI270 gesture recognition

Polls the on-board BMI270 and classifies simple board gestures:

- **shake** — accel magnitude spike
- **tilt** — board leaning on X/Y
- **flip** — Z axis inverted (face-down)

On each new gesture: log + short LED blink + optional motor pulse.

## Hardware

- ESP-Mosaico
- ESP-IDF >= 5.5

## Build and run

```sh
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

Move / tilt / flip the board and watch the serial log.
