# Mosaico ToF Module

The default sensor is **VL53L4CDV0DH/1** (VL53L4CD). Modules using **VL53L0CXV0DH/1** (VL53L0X) are also supported. Connect one ToF module to either expansion slot. The firmware reads the silicon ID after XSHUT reset and automatically selects the corresponding initialization and measurement steps; applications use the same API for both models.

The full order codes are listed in the ST [VL53L4CD datasheet](https://www.st.com/resource/en/datasheet/vl53l4cd.pdf) and [VL53L0X datasheet](https://www.st.com/resource/en/datasheet/vl53l0x.pdf).

"Default" identifies the standard module hardware. Driver selection follows the actual chip ID on every open, including after reconnection. Both sensors use I2C address `0x29`, and both use the ToF module descriptor (`0x10`). The address and descriptor identify the module connection; the silicon ID determines the sensor driver.

## Automatic Sensor Selection

| Hardware role | Sensor order code | Driver model | Chip identification | Initialization | Reading |
| --- | --- | --- | --- | --- | --- |
| Default | **VL53L4CDV0DH/1** | VL53L4CD | 16-bit register `0x010F` = `0xEBAA` | ST ULD initialization and VHV, 33 ms timing budget, continuous ranging | Poll data-ready for up to 500 ms, read the result, then clear the interrupt |
| Compatible alternative | **VL53L0CXV0DH/1** | VL53L0X | 8-bit register `0xC0` = `0xEE` | PAL data/static initialization, 33 ms timing budget, VHV/phase reference calibration, default profile, single-shot mode | Trigger a single measurement and read the result |

The opening sequence is:

1. Use the BSP's shared I2C bus, enable the module rail, and pulse the slot's mapped XSHUT pin.
2. Check the 8-bit model register `0xC0`. A value of `0xEE` selects VL53L0X.
3. Otherwise, check the 16-bit identification register `0x010F`. A value of `0xEBAA` selects VL53L4CD.
4. Execute the selected model's initialization sequence. Unknown IDs return `ESP_ERR_NOT_SUPPORTED` before model-specific initialization.
5. Read samples through `mosaico_tof_read()`, which dispatches to the selected driver and returns the common distance, validity, signal, and ambient fields.

No model-selection setting or application change is required when switching between supported modules. Initialization logs and `mosaico_tof_model_name()` report the detected driver model (`VL53L4CD` or `VL53L0X`).

## Application API

Initialize the module manager and claim a slot of type `MOSAICO_BOARD_TYPE_TOF` (`0x10`) before opening the sensor:

```c
mosaico_tof_handle_t sensor = NULL;
ESP_ERROR_CHECK(mosaico_tof_open((bsp_subboard_slot_t)lease.slot, &sensor));
ESP_LOGI("app", "Sensor: %s", mosaico_tof_model_name(sensor));
mosaico_tof_data_t data = {0};
ESP_ERROR_CHECK(mosaico_tof_read(sensor, &data));
// Use distance_mm only when data.valid is true.
ESP_ERROR_CHECK(mosaico_tof_close(sensor));
sensor = NULL;
ESP_ERROR_CHECK(mosaico_module_mgr_release(&lease));
```

Use `mosaico_tof_open()` for the default VL53L4CD module and automatic model selection. VL53L4CD uses 16-bit register addressing and its own initialization sequence; VL53L0X uses 8-bit register addressing and PAL reference calibration.

- The bus comes from `bsp_subboard_get_i2c_bus()`, which accounts for the mainboard revision. XSHUT uses GPIO48 on the left slot and its mapped counterpart on the right.
- Only one ToF board may be connected because both models use `0x29`. INT is unused.
- Serialize all operations and destruction on the same handle. `mosaico_tof_read()` is blocking; the VL53L4CD data-ready wait is limited to 500 ms.
- `ESP_OK` indicates successful communication. Check `data.valid` before using the distance; `data.range_status` reports invalid samples. Both models report distance in millimeters and signal/ambient rates in MCPS. `dmax_mm` is unavailable for VL53L4CD and remains zero.
- `mosaico_tof_data_t` aliases the existing `vl53l0x_data_t` result structure so applications can reuse distance displays.
- Close the handle before releasing the slot. Cleanup asserts XSHUT, removes the driver handles, and leaves shared power and the shared I2C bus available. A failed host-resource cleanup retains the handle for retry.
- The original `mosaico_tof_new()` compatibility API still returns a native VL53L0X handle, read with `vl53l0x_single_measure()` and freed with `vl53l0x_destroy()`. It rejects VL53L4CD; use `mosaico_tof_open()` for automatic model selection.

## Driver Sources

The VL53L4CD backend vendors ST's ULD 2.2.2 with an ESP-IDF platform port. Source revision, licensing, and the zero-SPAD division guard are documented in [private/vl53l4cd](private/vl53l4cd/README.md).

The compatible VL53L0X backend remains pinned to [pkolt/vl53l0x 1.0.0](https://components.espressif.com/components/pkolt/vl53l0x/versions/1.0.0). `driver_fix.cmake` patches a source copy in the build directory to fix an I2C-device allocation leak, allow destruction after failed initialization, and clear measurement state after failed single-shot reads. It does not modify `managed_components`; configuration fails if a patch no longer matches.

See [tof_distance](../../examples/tof_distance) for the live dashboard and startup bus scan.
