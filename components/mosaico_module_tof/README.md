# Mosaico ToF Module

The default sensor is **VL53L4CDV0DH/1** (VL53L4CD). Modules using **VL53L0CXV0DH/1** (VL53L0X) are also supported. The component supports either slot or both with application-managed address coordination. It reads the silicon ID after XSHUT reset and automatically selects the corresponding initialization and measurement steps; applications use the same API for both models.

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
- All sensors start at `0x29`. Hold unused sensors in XSHUT and assign a unique address after opening each slot before releasing the next sensor. This also applies to mixed models. INT is unused.
- Serialize all operations and destruction on the same handle. `mosaico_tof_read()` is blocking; continuous data-ready waits are limited to 500 ms. Use `mosaico_tof_read_ready()` after starting continuous mode to poll without waiting for measurement completion.
- `ESP_OK` indicates successful communication. Check `data.valid` before using the distance; `data.range_status` reports invalid samples. Both models report distance in millimeters and signal/ambient rates in MCPS. `dmax_mm` is unavailable for VL53L4CD and remains zero.
- `mosaico_tof_data_t` aliases the existing `vl53l0x_data_t` result structure so applications can reuse distance displays.
- Close the handle before releasing the slot. Cleanup attempts to stop ranging while the sensor is awake, asserts XSHUT and removes host handles, leaving shared power and the bus available. A removed VL53L0X need not acknowledge stop to free host resources; failed host-device removal still retains the handle for retry.
- The original `mosaico_tof_new()` compatibility API still returns a native VL53L0X handle, read with `vl53l0x_single_measure()` and freed with `vl53l0x_destroy()`. It rejects VL53L4CD; use `mosaico_tof_open()` for automatic model selection.

## Continuous Acquisition

`mosaico_tof_open()` preserves the legacy default: VL53L0X opens in single-shot
mode and VL53L4CD in continuous mode, both at a 33 ms budget. Configure unique
addresses, timing and offsets first, then call `mosaico_tof_start_continuous()`
on each handle. It starts back-to-back VL53L0X acquisition; for an already-running
sensor it is idempotent. Address/timing/offset writes on a running VL53L0X return
`ESP_ERR_INVALID_STATE`; close/reopen to reconfigure.

`mosaico_tof_read_ready(sensor, &data, &ready)` returns `ESP_OK` and `ready=false`
when no new sample is available. If `ready=true`, the new result has been read
and its interrupt acknowledged once; check `data.valid` before using distance.
On failure, the output is invalidated and `ready=false`. Reads still perform
bounded I2C transfers, but do not wait for sensor integration. `timestamp_ms` is
the host's data-ready observation, not an optical synchronization timestamp.
Existing `mosaico_tof_read()` also works after starting continuous mode.

## Sensor Configuration

For multiple sensors, the application must hold unused sensors in XSHUT, open
one slot at a time, and use `mosaico_tof_set_address()` to assign an unused
7-bit address in `0x08`–`0x77`, excluding `0x29`. The component uses
the selected sensor's address register and updates its ESP-IDF device handle.
Failed reassignment must be followed by close/reset/reopen. Addresses are
volatile; initialization must be serialized again after reset or reconnection.
The component does not coordinate peer shutdown on its own.

`mosaico_tof_set_timing_budget()` accepts microseconds. The component opens at
33 ms by default.
VL53L4CD accepts 10–200 ms in whole-millisecond increments and restarts ranging
after an update. VL53L0X retains its PAL validity checks.

`mosaico_tof_get_offset_calibration()` and
`mosaico_tof_set_offset_calibration()` use signed micrometers. The common
writable range is −512000..511000 micrometers, in
250-micrometer steps. VL53L4CD factory fractional offsets and other calibration
registers are retained. Applications can read the initial offset and add
board-specific compensation. Validate each sensor before applying an adjustment.

The VL53L0X compatibility patch also clears the stored I2C handle after removal
during address reassignment, so a failed rebind can be safely destroyed. It
preserves the running flag when acknowledging continuous results, interprets
stop status zero as complete, frees host resources after a failed stop on an
unplugged sensor, and avoids a duplicate PAL result read in the one-shot helper.
Managed dependency sources remain unchanged.

## Driver Sources

The VL53L4CD backend vendors ST's ULD 2.2.2 with an ESP-IDF platform port. Source revision, licensing, and the zero-SPAD division guard are documented in [private/vl53l4cd](private/vl53l4cd/README.md).

The compatible VL53L0X backend remains pinned to [pkolt/vl53l0x 1.0.0](https://components.espressif.com/components/pkolt/vl53l0x/versions/1.0.0). `driver_fix.cmake` patches a source copy in the build directory to fix an I2C-device allocation leak, allow destruction after failed initialization, and clear measurement state after failed single-shot reads. It does not modify `managed_components`; configuration fails if a patch no longer matches.
