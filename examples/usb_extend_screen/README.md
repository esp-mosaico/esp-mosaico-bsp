# USB extend screen

Use the Mosaico USB-OTG port as a Windows secondary display. The PC sends
frames over USB High Speed; the board shows them on the CO5300 panel and
can report HID touch and UAC audio.

This application owns TinyUSB. Flash and monitor through the UART port:

```bash
idf.py --preview set-target esp32s31
idf.py build
idf.py -p UART_PORT flash monitor
```

Windows needs the Espressif indirect display driver. See
[README_cn.md](README_cn.md) and the
[esp-iot-solution USB extend screen example](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/device/usb_extend_screen).
