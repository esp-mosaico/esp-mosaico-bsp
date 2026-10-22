# Mosaico Relay Component

A four-channel, active-high relay driver extracted from the `relay_board` example on the `test/subboard_test_lh` branch. It reuses BSP slot mapping and board-revision-specific power control, has no LVGL dependency, and creates no background tasks.

| Channel | API index | Left-slot GPIO | Right-slot GPIO |
| --- | --- | --- | --- |
| CH1 | 0 | 48 | 47 |
| CH2 | 1 | 53 | 46 |
| CH3 | 2 | 13 | 11 |
| CH4 | 3 | 12 | 10 |

Initialize `mosaico_module_mgr`, then claim a slot with `MOSAICO_BOARD_TYPE_RELAY` before creating the driver. The component does not discover or claim slots itself. Create only one instance per slot. See [relay_control](../../examples/relay_control) for a complete example.

```c
mosaico_relay_handle_t relay = NULL;
ESP_ERROR_CHECK(mosaico_relay_new((bsp_subboard_slot_t)lease.slot, &relay));
ESP_ERROR_CHECK(mosaico_relay_set(relay, 0, true));
ESP_ERROR_CHECK(mosaico_relay_all_off(relay));
ESP_ERROR_CHECK(mosaico_relay_del(relay));
relay = NULL;
ESP_ERROR_CHECK(mosaico_module_mgr_release(&lease));
```

Initialization presets each GPIO output level to low before enabling the output driver. All channels are off when creation succeeds. Deletion switches off each channel before freeing the handle, leaves the GPIO outputs low, and keeps the shared power rail enabled. If switch-off fails, deletion returns an error and retains the handle for retry. After successful deletion, the caller should immediately set the handle to `NULL`.

The caller must serialize control and deletion operations on the same handle. `mosaico_relay_all_off()` attempts to switch off every channel and returns the first error; it does not guarantee simultaneous switching. The component has no contact feedback: a successful GPIO write only confirms that the control signal was set. The application and module manager handle insertion and removal detection, which is subject to scanning and debounce delays.
