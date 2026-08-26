# Mosaico module manager

`mosaico_module_mgr` discovers left and right modules from their EEPROM,
validates the V1 CRC fields, reports insertion/removal events, and provides an
exclusive claim/release lifecycle for concrete module drivers.

## Left / right discrimination

Both slots share I2C0. The BSP drives each AT24C02 A0 pin so the EEPROMs appear
at different addresses:

| Slot | A0 GPIO | Level | I2C address |
|------|---------|-------|-------------|
| Left | GPIO14 | 0 | `0x50` |
| Right | GPIO39 | 1 | `0x51` |

Discovery probes those addresses. `mosaico_module_mgr_info_t.eeprom_addr` reports
which address identified the board; use `bsp_subboard_map_gpio()` (or a concrete
driver such as `mosaico_button_led`) to resolve connector pins for that slot.

## Usage

Most applications do not need to initialize this component directly. A
concrete driver such as `mosaico_camera` starts it automatically. Initialize it
explicitly only when the application needs discovery events:

```c
const mosaico_module_mgr_config_t config = {
    .scan_period_ms = 200,
    .debounce_count = 3,
    .event_callback = on_module_event,
};
ESP_ERROR_CHECK(mosaico_module_mgr_init(&config));
```

Claimed slots are not polled. This is required for the current left camera
module because its D4 signal shares GPIO14 with the EEPROM address-select
signal. Camera release restores left A0 through
`bsp_subboard_apply_address_select()`.

`MOSAICO_BOARD_TYPE_BUTTON_LED` (`0x14`) identifies the two-key + 3x WS2812
module. Applications should use `mosaico_button_led` rather than claiming the
slot directly. Programming blank EEPROMs is a manufacturing operation and is
not included in the public example set.
