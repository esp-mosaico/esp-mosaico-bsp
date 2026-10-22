[English](README.md) | [简体中文](README_CN.md)

# ToF Distance Example

Displays independent left/right VL53L0X readings on the 480 × 480 screen.
Connect one or two ToF boards with valid EEPROM descriptors (Board Type
`0x10`). Use `module_eeprom_programmer` to program descriptors if necessary.

## Features

| Feature | Description |
|---|---|
| Single/dual-slot ranging | Use either slot or both; both initialized sensors stay powered and are measured sequentially, with independent readings and status |
| EEPROM discovery | Detects valid ToF descriptors and waits when no board is present |
| Automatic addressing | Holds both XSHUT lines low at startup, then starts boards individually and assigns `0x30` (left) and `0x31` (right); reapplies addresses after reset |
| Side-by-side dashboard | Cards match physical slots and show millimeter readings, addresses, status, signal/ambient count rates and range bars |
| Bidirectional gestures | Detects left/right swipes from the order of distance excursions; requires valid readings from both sensors |
| Directional animation | Bottom panel with gesture text and a 12-segment chasing light bar, animated independently of measurements |
| Faster, balanced sampling | Uses a 20 ms per-sensor budget, alternating read order, acquisition timestamps and estimated threshold crossings to reduce missed events and directional bias |
| Per-slot compensation | Defaults to an additional −37 mm on both sides, independently adjustable; this is not automatic calibration |
| Recovery | Cleans up and rediscovers an affected slot; invalid ranges clear stale data without being treated as module removal |
| Diagnostics and tests | Range, gesture, actual sampling-rate and per-side validity logs, plus host algorithm regression tests |

### Quick Start

1. Connect one or two ToF boards with valid EEPROM descriptors, then build and flash as described below.
2. Check the corresponding card for a valid distance. One sensor can measure distance, but cannot recognize directional swipes alone.
3. For gestures, aim both sensors at the same interaction area and keep the background steady until the bottom panel shows `READY - WAVE LEFT OR RIGHT`.
4. Move a hand past the left sensor and then the right, allowing distances to recover: expect `SWIPE RIGHT` and a rightward chase. Reverse the motion for `SWIPE LEFT` and a leftward chase.
5. Wait for cooldown and baseline relearning before the next gesture. Direction refers to the mainboard's slots, not the observer's viewpoint.

The demo only displays and logs gesture results. It does not control other devices, provide touchscreen interaction, save measurement history, or perform automatic calibration.

## Hardware and Address Assignment

This demo reserves both slots for ToF boards. Do not use other module types in
these slots while it runs: their mapped XSHUT pins are driven low even before
EEPROM discovery. Pin mapping follows the BSP; on the specified mainboard:

| Slot | XSHUT | Sensor address (7-bit) |
|---|---|---|
| Left | GPIO48 | 0x30 |
| Right | GPIO47 | 0x31 |

At application startup both XSHUT lines are held low before display initialization
and EEPROM discovery. When a valid ToF descriptor is claimed, only that sensor
is released, initialized at its default address `0x29`, and assigned its slot
address. Initialization is serialized; the peer stays either shut down or awake
at its assigned address. The target address is checked for conflicts and probed
after reassignment. EEPROM addresses are unchanged.

Both initialized sensors remain powered. The demo selects the driver's high-speed
profile (20 ms measurement budget per sensor, retaining signal/sigma checks).
A single task takes one blocking single-shot measurement per ready slot,
sequentially, then yields one RTOS tick (10 ms at the current 100 Hz tick rate).
The former extra 100 ms delay is removed. The nominal timing sum is about 50 ms
per pair before polling, I²C and processing overhead; this is not a measured rate.
INT is unused (I²C polling). Initialization/retries
can temporarily increase the peer's update interval.

Removal or a communication error clears only that slot's display, holds its
XSHUT low, destroys its handle and releases its lease. Failed cleanup retains
ownership and retries without a blocking retry loop. The other slot continues
to be serviced. Reconnection resets and reassigns the affected sensor without
intentionally resetting the healthy peer. Invalid range results alone do not
trigger reconnection. Only status 0 is displayed as a valid distance.

Address assignment is volatile and is reapplied after reset. An unexpected
sensor power reset is recovered after measurement failure. Physical hot-plug
contact sequencing cannot be guaranteed by software alone.

## UI and Logs

The title is `ESP-MOSAICO TOF MODULE`. Two side-by-side cards show left/right
addresses, approximately 72 px distance readings, a highlighted 28 px `mm`
unit, status, return signal and ambient count rates (MCPS, not lux).
Each card has a 0–2000 mm bar. Valid readings beyond the scale retain their
numeric value while the bar saturates. Invalid data clears the old distance.
Touch is disabled; display brightness is 80%.

Routine dashboard updates are rate-limited to once per 100 ms, with immediate gesture-event updates.
Every acquired pair still enters gesture detection. Valid measurements are logged
at most once per second per slot. A `Dual sampling: ... pairs/s` log every five
seconds reports actual paired throughput (including invalid readings).

## Gesture Recognition

A panel below the two distance cards shows the last gesture and a 12-segment
animated light bar. Left-then-right distance pulses produce `SWIPE RIGHT` with
a left-to-right chase; right-then-left pulses produce `SWIPE LEFT` with the
opposite animation. Results remain visible for about 2.5 seconds. The LVGL timer
drives the animation independently of blocking measurements.

Keep both sensors aimed at stable, valid backgrounds first (three consecutive
stable sample pairs). Then move a hand across the sensors within the configured
window. The faster sampling supports shorter pulses, but gestures occurring
entirely between samples can still be missed. This is a simple
distance-sequence heuristic, not a general hand tracker.

Bottom-panel messages:

| Message | Meaning |
|---|---|
| `WAIT FOR TWO VALID TARGETS` | One or both sensors lack a valid distance |
| `HOLD STILL - LEARNING BASELINE` | Keep the background stable while the detector learns/rearms |
| `READY - WAVE LEFT OR RIGHT` | Ready to try a gesture |
| `SWIPE RIGHT` / `SWIPE LEFT` | Last detected gesture; its display does not imply the detector is already rearmed |

Recognition rules:

- Default excursion threshold: 60 mm from each sensor's learned baseline.
- A pulse completes when the distance returns within 30 mm of its baseline.
- Increasing-then-decreasing and decreasing-then-recovering pulses are supported;
  both channels must have the same polarity and consistent onset/completion order.
- Both pulses must finish within 1500 ms of the first onset. Minimum onset
  separation is 25 ms, minimum pulse duration 25 ms, cooldown 800 ms.
- Simultaneous/ambiguous changes, invalid readings, missing modules, or a sample
  gap over 500 ms discard the pending sequence. The baseline is relearned after
  recognition, rejection or timeout. No valid background means recognition waits.
- Each cycle alternates which sensor is measured first. Each sample is timestamped
  at its acquisition midpoint, and threshold crossing times are linearly estimated
  between samples instead of assigning a common frame timestamp. This reduces
  directional bias from sequential reads; it is not hardware synchronization.
- A clear onset order with tied completion times is accepted. Contradictory order
  or simultaneous onsets remain rejected. Individual sample ages are checked.
- The five-second `Sample validity: left=... right=...` log compares usable
  readings on each side; large differences warrant checking alignment/backgrounds.

Tune `Gesture distance excursion threshold (mm)` and `Gesture completion window
(ms)` in `menuconfig → ToF Distance Demo`. Lower thresholds increase sensitivity
and false-trigger risk. No sensor address, offset or INT behavior is changed.
The settings are `CONFIG_TOF_GESTURE_THRESHOLD_MM` (default 60, range 20–500 mm)
and `CONFIG_TOF_GESTURE_WINDOW_MS` (default 1500, range 500–3000 ms).
The return threshold is half the excursion threshold. The 25 ms timing checks
and 800 ms cooldown are constants in `main/tof_gesture.c`, not menu settings.
The shorter measurement budget may increase noise or invalid readings under weak
signal conditions; verify ranging accuracy and gestures on hardware. The 60 mm
amplitude threshold, return hysteresis, ordering checks and cooldown remain active.

Host regression test (run in this example directory):

```bash
cc -std=c11 -Wall -Wextra -Werror -I main tests/test_gesture.c main/tof_gesture.c -o /tmp/tof_gesture_test
/tmp/tof_gesture_test
```

## Build and Flash

Use an ESP32-S31-capable ESP-IDF 6.2 or newer environment, then run from the
repository root:

```bash
cd examples/tof_distance
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the board's serial port. BSP APIs provide display, power,
bus and slot mappings. Supported revisions follow the BSP (v1.0 and v1.2;
v1.1 uses the v1.2 mapping); the new dual-slot behavior requires hardware validation.

## Per-slot Offset Adjustment

The [ST datasheet](https://www.st.com/resource/en/datasheet/vl53l0x.pdf)
(DS11555 Rev 6, section 3.3.2) recommends offset calibration at 100 mm.
Reported readings on the previously tested board were 137 mm (136–139 mm)
at 100 mm and 243 mm (242–246 mm) at 200 mm.

Both slots default to the same **-37 mm** adjustment based on the reported
measurements above. This does not mean that the second board has been independently
calibrated; verify its accuracy separately. The settings remain independently
adjustable per slot and are not tied to EEPROM identity.

In `idf.py menuconfig → ToF Distance Demo`, configure:

- Left slot additional distance offset (mm): `CONFIG_TOF_DISTANCE_OFFSET_ADJUST_MM`.
- Right slot additional distance offset (mm): `CONFIG_TOF_DISTANCE_RIGHT_OFFSET_ADJUST_MM`.

The adjustment is added to each sensor's initial offset once after reset and
verified by register readback. It affects both display and logs, is stored in
firmware configuration rather than EEPROM/NVS, and can be disabled with 0.
This manual adjustment is not a completed ST 50-sample calibration or cross-talk
calibration. Verify each board at 100, 200 and 500 mm measured from its optical
surface. Do not assume one board's compensation applies to another.

## Reference Documentation

- [VL53L0X Datasheet (ST website, PDF, English)](https://www.st.com/resource/en/datasheet/vl53l0x.pdf):
  pin definitions, operating modes and specifications; DS11555 Rev 6 sections
  3.3.1–3.3.3 describe reference, offset and cross-talk calibration.

## Validation Scope

The current functionality passes an ESP32-S31 build. The user has confirmed
gesture recognition on hardware; subsequent fast-motion and direction-balance
improvements still require hardware retesting.

Host regression tests cover both directions/polarities, overlapping pulses,
tied completion times, noise, invalid data, timeouts, cooldown, timestamp wrap
and varying acquisition phases with alternating read order. The simulated phase
sweep passes 12/12 cases per direction; this is not a hardware success rate.

On hardware, verify both boards present at boot, either board alone, removal and
reinsertion, independent address logs and per-board compensation. Compare repeated
left/right swipes with the same path, speed and background, alongside `Sample
validity` and `Dual sampling` logs. Mounting, background reflectivity and motion
speed can still affect recognition.
