# SoftAP HTTP camera stream

Starts an open SoftAP (`mosaico-cam`) and serves OV3640 frames over HTTP:

- `http://192.168.4.1/` — tiny HTML page with the MJPEG stream
- `http://192.168.4.1/jpg` — single JPEG snapshot
- `http://192.168.4.1/stream` — multipart MJPEG stream

The OV3640 emits JPEG directly. Snapshot and MJPEG handlers send the camera
buffer without an intermediate conversion, copy, or software encode.

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

Connect a phone/laptop to Wi-Fi SSID `mosaico-cam` (open, no password), then
open `http://192.168.4.1/` in a browser.

Note: GPIO33 is camera D2 and shares the USB Serial/JTAG pad — console is UART.
