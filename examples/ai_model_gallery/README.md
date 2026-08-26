# AI model gallery (face ↔ COCO)

Cycles detection models with the AI button:

1. **face** — `espressif/human_face_detect`
2. **coco** — `espressif/coco_detect` (YOLO11n)
3. **idle** — camera only, no inference

Only one model is constructed at a time to limit PSRAM/flash pressure.
Switching modes stops the camera and hardware JPEG decoder first so the
RGB888/rotation buffers do not fragment PSRAM before the next model is
built. Results are logged to the console (no LCD overlay).

The camera emits JPEG directly and the ESP32-S31 hardware JPEG engine decodes
into one reusable RGB888 buffer for inference. Face mode also rotates the
frame 90 degrees so an upright portrait face matches the detector. COCO
skips rotation so YOLO11n and a second RGB888 buffer are not resident at
once. No-detection frames are silent at default log level.

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
