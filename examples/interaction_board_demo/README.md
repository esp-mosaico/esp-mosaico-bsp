# Interaction board demo

This minimal example automatically discovers one Interaction subboard and presents its state on the 480 x 480 display:

- left and right touch or GPIO buttons;
- PIR motion detection;
- ambient-light sampling;
- six WS2812 LEDs.

The dashboard shows the left and right button states, PIR motion, ambient-light percentage, and raw light value. The left button controls red, the right button controls blue, and PIR motion controls green on the subboard LEDs.

Tap `IR SEND` to transmit an NEC frame with address `0x00` and command `0x10`.

The example supports hot-plugging. Removing the active board returns the dashboard to its waiting state, and inserting an Interaction board reconnects it automatically.

```bash
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash monitor
```

The default input mode prefers touch buttons and falls back to GPIO buttons if touch initialization is unavailable.
