# Module slot scan

Split-screen hot-plug test. The 480×480 panel is divided in half:

| Side | Slot | EEPROM |
|------|------|--------|
| Left | Left | `0x50` |
| Right | Right | `0x51` |

- Empty slot: placeholder with the slot address
- Camera (left only): live preview
- Interaction board: LED toggles, IR send, key / PIR / light status
- Other identified boards: name only

Discovery keeps scanning both slots after insert or claim. Unplug is reported
after three missed probes.

```bash
idf.py --preview set-target esp32s31
idf.py build
idf.py -p PORT flash monitor
```

## What to try

1. Boot with both slots empty. Each half shows `Empty`.
2. Plug the camera on the left. The left half should show the preview; the
   right half must keep scanning.
3. Plug an interaction board on the right. Use the on-screen LED / IR controls.
4. Unplug one side. That half returns to `Empty` after about 1.5 s.

The camera DVP data line shares GPIO33 with USB-Serial/JTAG, so this example
uses the UART console.
