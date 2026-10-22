# Four-Channel Relay Touch Control

A 480 × 480 dark interface with four channel cards and an `ALL OFF` button. Tap a card to toggle its channel; an energized channel shows a green `ON` state. All buttons are disabled when no module is connected. All channels start off at startup and after reconnection, with no automatic activation.

## Hardware and Setup

- An ESP-Mosaico mainboard and a four-channel relay module connected to either slot. If two modules are connected, the example controls only the first one it successfully claims.
- The module EEPROM must contain a valid descriptor with type `MOSAICO_BOARD_TYPE_RELAY` (`0x13`). Use the `module_eeprom_programmer` example first if the EEPROM has not been programmed.
- An ESP-IDF 6.2 environment with ESP32-S31 support.

```bash
cd examples/relay_control
idf.py build
idf.py -p PORT flash monitor
```

Target, PSRAM, display, and font settings are provided in `sdkconfig.defaults`. The interface shows GPIO command states, not relay contact feedback.

## Code Structure

`main/main.c` contains a minimal LVGL interface and a module discovery loop: claim a slot through the manager → call `mosaico_relay_new()` → handle touch input with `mosaico_relay_set()` → switch off and delete the driver when removal is detected → release the slot. The display lock protects callbacks, state, and handle lifetime. Initialization failures release the slot before retrying.

The driver is in [`components/mosaico_module_relay`](../../components/mosaico_module_relay) and can also be used without a display. Manager scanning and debouncing take time, so removal detection and output shutdown are not immediate. Rapid removal and reinsertion may not be recognized as a complete disconnection event.
