# Mosaico Thermal Imaging Module

`mosaico_module_thermal` is an ESP-IDF driver component for the Mosaico MLX90640 thermal imaging module. It provides contactless temperature acquisition at a resolution of 32 × 24 pixels. Applications can read a complete temperature matrix in degrees Celsius, the sensor ambient temperature, and the sensor supply voltage for thermal image display, temperature distribution monitoring, and hotspot detection.

The component handles sensor initialization, calibration parameter loading, temperature calculation, and bad pixel correction. Applications handle color palettes, image scaling, display rotation, and user interaction. See the [thermal_camera example](../../examples/thermal_camera) for a complete application.

## Module Specifications

| Item | Description |
| --- | --- |
| Sensor | MLX90640 |
| Image size | 32 × 24, totaling 768 temperature pixels |
| Communication interface | Mosaico BSP subboard I²C bus |
| I²C address | Fixed 7-bit address `0x33` |
| I²C clock | Configurable, 400 kHz by default, up to 1 MHz |
| Acquisition mode | Chess mode; two different subpages form a complete image |
| ADC resolution | Fixed at 17 bits |
| Subpage refresh rate | 0.5–64 Hz, 16 Hz by default |
| Complete image rate | Approximately 8 Hz in theory with default settings; actual throughput depends on bus transfers and temperature calculation time |
| Module power | The module 5 V rail is enabled through the BSP during initialization |
| Supported target | ESP32-S31 (`esp32s31`) |
| Software requirements | ESP-IDF >= 6.2 and `esp-mosaico-bsp` |
| Component version | 0.1.0 |
| License | Apache-2.0 |

The module can be connected to either the left or right Mosaico subboard slot. Both slots share the I²C bus, and the sensor address is fixed, so only one thermal imaging module may be connected at a time.

## Project Integration

Add `mosaico_module_thermal` and `mosaico_module_mgr` as application dependencies. For projects using this repository layout, refer to the example's [`main/idf_component.yml`](../../examples/thermal_camera/main/idf_component.yml):

```yaml
dependencies:
  idf: ">=6.2"
  mosaico_module_thermal:
    path: ../../../components/mosaico_module_thermal
  mosaico_module_mgr:
    path: ../../../components/mosaico_module_mgr
  esp-mosaico-bsp:
    path: ../../../components/esp-mosaico-bsp
```

Paths are relative to the directory containing `idf_component.yml`. Adjust them for other project layouts and ensure that the BSP dependency path remains valid.

The public API is declared in [`mosaico_module_thermal.h`](include/mosaico_module_thermal.h). Use the [module manager](../mosaico_module_mgr/README.md) for module discovery and slot ownership. The board type is `MOSAICO_BOARD_TYPE_THERMAL` (`0x12`).

## Usage

1. Call `mosaico_module_mgr_init(NULL)` to initialize the module manager and subboard bus.
2. Call `mosaico_module_mgr_claim()` with `expected_type` set to `MOSAICO_BOARD_TYPE_THERMAL` to obtain a slot lease. Set the slot to `MOSAICO_MODULE_MGR_SLOT_AUTO` to select an available matching slot automatically.
3. Cast `lease.slot` to `bsp_subboard_slot_t` and pass it to `mosaico_thermal_new()` to create a sensor handle. Pass `NULL` as the configuration to use the defaults.
4. Call `mosaico_thermal_read()` to acquire a complete temperature frame. Use the new data only when the call returns `ESP_OK`.
5. When finished, call `mosaico_thermal_del()`. After successful deletion, set the handle to `NULL`, then call `mosaico_module_mgr_release(&lease)` to release the slot.

All component calls are synchronous; the component does not create an acquisition task. Schedule acquisition in an application task and serialize all operations on the same handle, including deletion. The component does not claim slots or delete the shared BSP bus.

See [`thermal_camera/main/main.c`](../../examples/thermal_camera/main/main.c) for initialization, continuous acquisition, error handling, and resource cleanup.

## Configuration

Initialize `mosaico_thermal_config_t` with `MOSAICO_THERMAL_DEFAULT_CONFIG()`, then adjust the fields as needed:

```c
mosaico_thermal_config_t config = MOSAICO_THERMAL_DEFAULT_CONFIG();
config.refresh_rate = 5; // 16 Hz subpage rate.
config.emissivity = 0.95f;
```

| Field | Default | Description |
| --- | --- | --- |
| `clock_speed_hz` | `400000` | I²C clock frequency; must be greater than 0 and no greater than `1000000` Hz |
| `refresh_rate` | `5` | Subpage refresh rate code from `0` to `7`; see the table below |
| `emissivity` | `0.95f` | Target object emissivity; must be a finite value in `(0, 1]` |
| `reflected_offset_c` | `8.0f` | Offset used to estimate reflected temperature; must be finite. Reflected temperature = ambient temperature − this offset, in °C |

| `refresh_rate` | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Subpage refresh rate (Hz) | 0.5 | 1 | 2 | 4 | 8 | 16 | 32 | 64 |
| Theoretical complete image rate (Hz) | 0.25 | 0.5 | 1 | 2 | 4 | 8 | 16 | 32 |

A complete image requires two different subpages, so the configured refresh rate is the subpage rate rather than the complete image rate. Before increasing it, ensure that bus bandwidth and processing capacity are sufficient. Repeated frame overruns can cause read timeouts. When using 1 MHz I²C, also verify wiring, pull-ups, and platform driver stability.

## Temperature Frames

`mosaico_thermal_read()` returns data through `mosaico_thermal_frame_t`:

| Field | Description |
| --- | --- |
| `pixels[768]` | Pixel temperatures in row-major order, in °C |
| `ambient_c` | Sensor ambient temperature, in °C |
| `supply_v` | Sensor supply voltage, in V; this is not the module 5 V rail voltage |

The temperature at coordinate `(x, y)` is `frame.pixels[y * MOSAICO_THERMAL_WIDTH + x]`, where `0 <= x < 32` and `0 <= y < 24`. The component preserves the sensor's native image orientation. Applications can rotate the display to match the module's mounting orientation.

A successful read provides temperature data from two different subpages with bad pixel correction applied. If a read fails, the caller's output frame remains unchanged. Applications should avoid treating the previous frame as a new sample.

## API Reference

| Function | Description |
| --- | --- |
| `mosaico_thermal_new(slot, config, out_sensor)` | Enables power, attaches the sensor, loads calibration parameters, and configures acquisition |
| `mosaico_thermal_read(sensor, frame, timeout_ms)` | Synchronously acquires two subpages, calculates temperatures, and returns a complete frame |
| `mosaico_thermal_del(sensor)` | Removes the I²C device and frees the sensor handle |

### Timeouts and Memory

Initialization uses a 2-second I/O deadline, with a maximum wait of 100 ms per transfer. The read parameter `timeout_ms` must be positive. Polling and I/O for both subpages share this deadline; task scheduling and temperature calculation may extend the actual return time beyond it.

Allow at least two subpage periods plus transfer and calculation time when choosing a read timeout. Use `1000` ms with the default configuration; at a subpage refresh rate of 0.5 Hz, allow at least `5000` ms.

A temperature frame occupies approximately 3 KB. Static or heap storage is recommended. Calibration parameter extraction during initialization also requires approximately 3 KB of temporary stack space; allocate at least 8 KB of stack to the calling task.

### Error Handling and Cleanup

| Return value | Meaning or suggested action |
| --- | --- |
| `ESP_ERR_INVALID_ARG` | Check the handle, output pointer, slot, configuration, or read timeout |
| `ESP_ERR_INVALID_STATE` | The subboard bus has not been initialized when creating the sensor |
| `ESP_ERR_NO_MEM` | Insufficient memory to create the sensor |
| `ESP_ERR_TIMEOUT` | I/O or complete frame acquisition did not finish before the deadline; check connections, timeout settings, and refresh rate |
| `ESP_ERR_INVALID_RESPONSE` | Invalid sensor calibration or sample data |
| Other errors | An underlying BSP or I²C operation failed; inspect the return value and logs |

If creation fails, the output handle is normally `NULL`. If device removal during initialization cleanup also fails, the component returns a non-NULL handle. Use that handle only to retry `mosaico_thermal_del()`; do not attempt to read from it.

If deletion fails, the handle remains valid. Keep both the handle and slot lease so that cleanup can be retried. Clear the handle and release the slot only after successful deletion. If slot release fails, retain the lease and handle the error.

## License

This component is licensed under Apache-2.0. The MLX90640 temperature calculation algorithm includes code from Melexis N.V.; copyright and license notices are retained in the source files. Applications should use the component's public header. Algorithm headers under `private/` are internal implementation details.
