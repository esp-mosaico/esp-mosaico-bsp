# AI model gallery (face ↔ COCO)

Cycles detection models with the AI button:

1. **face** — `espressif/human_face_detect`
2. **coco** — `espressif/coco_detect` (YOLO11n)
3. **idle** — camera only, no inference

Only one model is constructed at a time to limit PSRAM/flash pressure.
Switching modes stops the camera and releases the PPA conversion buffer first
so it does not fragment PSRAM before the next model is built. The LCD shows a
live square preview with the current mode and detection count. Detailed results
are also logged to the console.

The camera uses its default UYVY resolution. The ESP32-S31 PPA converts each
frame into one reusable BGR888 buffer for inference, supporting both OV3640
and SC101IOT modules. Face mode also rotates the frame 90 degrees so an upright
portrait face matches the detector. No-detection frames are silent at default
log level.

## Hardware

- ESP-Mosaico + Camera subboard (LEFT)
- ESP-IDF >= 6.0

## Build and run

```sh
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

Press AI to switch modes. LED on = inference active.
