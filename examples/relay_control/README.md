# ESP-MOSAICO — Module Relay

A four-channel relay touch console for the ESP-Mosaico board, with a 480 × 480 dark interface, mint accents, and an `ALL ON` / `ALL OFF` toggle button.

## Interface and Controls

The upper-left title has two lines: `ESP-MOSAICO` and `Module Relay`. The connection badge shows `ONLINE` or `OFFLINE`; the activity strip shows `00 / 04 ACTIVE` through `04 / 04 ACTIVE`, the connected slot (`LEFT SLOT` / `RIGHT SLOT`), and one indicator per channel.

Tap anywhere on a channel card to toggle that channel. Each card shows `ON` / `OFF`, an animated switch, and a hint for the next action. Enabled channels are highlighted in mint green. Press feedback accompanies each tap; a 180 ms switch animation follows successful GPIO commands without delaying the relay writes.

The bottom button displays the action that the next tap will perform:

| Current state | Button | Action on tap |
| --- | --- | --- |
| All four channels off | `ALL ON` | Switch all four channels on |
| One to three channels on | `ALL ON` | Switch all four channels on |
| All four channels on | `ALL OFF` | Switch all four channels off |
| No module connected | `ALL ON` (disabled) | No action |

Starting with all channels off, repeated taps alternate between all on and all off. Individual channel changes also update the bottom button, active count, and indicators immediately.

## Startup, Disconnection, and Errors

All channels start off at startup and after reconnection, with no automatic activation. When disconnected, the badge changes to `OFFLINE`, the slot shows `NO MODULE`, cards show `--` / `Unavailable`, and all control buttons are disabled.

Command failures appear in amber. Group commands attempt all four outputs and update each displayed state only after its write succeeds. Failed writes retain their previous displayed states, and the bottom button follows the resulting channel states using the table above. If a group command fails, check the individual channels before issuing another command.

The interface shows the last successful GPIO command states, not relay contact feedback.

## Hardware and Setup

- An ESP-Mosaico mainboard and a four-channel relay module connected to either slot. If two modules are connected, the example controls only the first one it successfully claims.
- The module EEPROM must contain a valid descriptor with type `MOSAICO_BOARD_TYPE_RELAY` (`0x13`). Use the [`module_eeprom_programmer`](../module_eeprom_programmer) example first if the EEPROM has not been programmed.
- An ESP-IDF 6.2 environment with ESP32-S31 support.

```bash
cd examples/relay_control
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the connected serial device, for example `/dev/ttyUSB0`:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Target, PSRAM, display, and Montserrat 14/16/28 font settings are provided in `sdkconfig.defaults`.

## Serial Logs

The example logs UI startup, individual channel changes, group commands, and failures. The module manager and relay driver also report module identification and initialization. Typical application messages are:

```text
relay_control: Relay console UI ready (480x480)
relay_control: Channel 01 -> ON
relay_control: Channel 01 -> OFF
relay_control: All channels -> ON
relay_control: All channels -> OFF
relay_control: Module removed; outputs OFF
```

Group success messages are emitted only when all four writes succeed. A failed command logs `Relay command failed: ...` and shows `Command failed. Check individual channels.` on screen. Exit the serial monitor with `Ctrl+]`.

## Code Structure

[`main/main.c`](main/main.c) contains the LVGL console, animated channel controls, group toggle, and a module discovery loop: claim a slot through the manager → call `mosaico_relay_new()` → handle touch input with `mosaico_relay_set()` → switch off and delete the driver when removal is detected → release the slot. The display lock protects callbacks, state, and handle lifetime. Initialization failures release the slot before retrying.

The driver is in [`components/mosaico_module_relay`](../../components/mosaico_module_relay) and can also be used without a display. Manager scanning and debouncing take time, so removal detection and output shutdown are not immediate. Rapid removal and reinsertion may not be recognized as a complete disconnection event.
