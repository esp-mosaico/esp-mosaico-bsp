[English](README.md) | [简体中文](README_CN.md)

# ToF Distance Example

Displays independent left/right ToF readings on the 480 × 480 screen. The default
sensor is **VL53L4CDV0DH/1** (VL53L4CD); **VL53L0CXV0DH/1** (VL53L0X)
is also supported. Each slot automatically selects its driver from the chip ID,
so supported sensor models may be mixed.
Connect one or two ToF boards with valid EEPROM descriptors (Board Type
`0x10`). Use `module_eeprom_programmer` to program descriptors if necessary.

## Features

| Feature | Description |
|---|---|
| Single/dual-slot ranging | Use either slot or both; both initialized sensors stay powered and are measured sequentially, with independent readings and status |
| Automatic model selection | Reads the silicon ID after reset and selects the matching VL53L4CD or VL53L0X initialization and read sequence |
| EEPROM discovery | Detects valid ToF descriptors and waits when no board is present |
| Automatic addressing | Holds both XSHUT lines low at startup, then starts boards individually and assigns `0x30` (left) and `0x31` (right); reapplies addresses after reset |
| Side-by-side dashboard | Cards match physical slots and show millimeter readings, addresses, status, signal/ambient count rates and range bars |
| Bidirectional gestures | Detects near-field entry/exit order even with no measurable background; uses relative-distance excursions as a fallback for valid backgrounds |
| Directional animation | Bottom panel with gesture text and a 12-segment chasing light bar, animated independently of measurements |
| Faster, balanced sampling | Uses a 20 ms per-sensor budget, alternating read order, acquisition timestamps and estimated threshold crossings to reduce missed events and directional bias |
| Per-slot compensation | Independent settings for each model and slot: VL53L4CD defaults to 0 mm; legacy VL53L0X settings retain −37 mm for the previously measured board |
| Recovery | Cleans up and rediscovers an affected slot; invalid ranges clear stale data without being treated as module removal |
| Diagnostics and tests | Rate-limited range and gesture logs, optional Debug sampling/validity diagnostics, startup bus scan, and host algorithm regression tests |

### Quick Start

1. Connect one or two ToF boards with valid EEPROM descriptors, then build and flash as described below.
2. Check the corresponding card for a valid distance. One sensor can measure distance, but cannot recognize directional swipes alone.
3. For gestures, connect both sensors and leave the interaction area clear. An empty field can show `--` on both cards while the bottom panel shows `READY - WAVE LEFT OR RIGHT`; valid background distances are not required.
4. Move a hand within 30 cm past the left sensor and then the right, then clear both sensors: expect `SWIPE RIGHT` and a rightward chase. Reverse the motion for `SWIPE LEFT` and a leftward chase.
5. Wait for cooldown and detector rearming before the next gesture. Direction refers to the mainboard's slots, not the observer's viewpoint.

The demo only displays and logs gesture results. It does not control other devices, provide touchscreen interaction, save measurement history, or perform automatic calibration.

## Sensor Models and Automatic Selection

| Hardware role | Order code | Identification | Initialization and reading |
|---|---|---|---|
| Default | **VL53L4CDV0DH/1** | 16-bit register `0x010F` = `0xEBAA` | ST ULD initialization and VHV; continuous ranging with a 20 ms budget; poll data-ready, read results, clear interrupt |
| Compatible alternative | **VL53L0CXV0DH/1** | 8-bit register `0xC0` = `0xEE` | PAL initialization and reference calibration; 20 ms single-shot ranging with signal/sigma checks |

After reset, the component checks the VL53L0X model register first; otherwise
it checks the VL53L4CD ID using 16-bit register addressing. Unknown IDs return
`ESP_ERR_NOT_SUPPORTED`. Both sensors use factory address `0x29`, so an I2C ACK
or a ToF EEPROM descriptor alone does not establish the model. Driver selection
is repeated after reconnection; no manual model switch or rebuild is required.
Use `mosaico_tof_open()`, `mosaico_tof_read()`, and `mosaico_tof_close()` for either
model. The model-neutral address, timing-budget and offset APIs coordinate the
new dashboard without passing a VL53L4CD to the legacy VL53L0X driver.

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
is released, initialized at its default address `0x29`, identified by chip ID, and assigned its slot
address. Initialization is serialized; the peer stays either shut down or awake
at its assigned address. The target address is checked for conflicts and probed
after reassignment. EEPROM addresses are unchanged.

Both initialized sensors remain powered and use a 20 ms measurement budget.
A single task reads ready slots sequentially and alternates which side goes
first. VL53L0X triggers a single-shot measurement for each read; VL53L4CD runs
continuously and returns the next ready result. The task yields one RTOS tick
(1 ms with `CONFIG_FREERTOS_HZ=1000`) while sampling. Actual paired throughput
depends on sensor type, data-ready timing and I2C overhead; use Debug sampling
statistics to measure it. INT is unused. Initialization/retries can temporarily
increase the peer's update interval.

The application scans all non-reserved 7-bit addresses (`0x08`–`0x77`) once after
bus/module-manager initialization, even with no modules or invalid descriptors.
Both XSHUT lines are held low at this stage, so the scan normally lists EEPROMs
and other powered devices, while the ToF sensors are still shut down. Each
sensor is later verified at its assigned `0x30`/`0x31` address.

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
detected models and addresses, approximately 72 px distance readings, a highlighted 28 px `mm`
unit, status, return signal and ambient count rates (MCPS, not lux).
Each card has a 0–2000 mm bar. Valid readings beyond the scale retain their
numeric value while the bar saturates. Invalid data clears the old distance.
Touch is disabled; display brightness is 80%.

The sampling task publishes copied dashboard state at most once per 100 ms and
publishes gesture events immediately. A 50 ms LVGL timer renders the latest state
and consumes pending events. Publishing does not acquire the display lock, so
rendering or a slow display transfer cannot block the sampling loop. Intermediate
display readings may be coalesced; every acquired pair still reaches detection.
Valid measurements are logged
at most once per second per slot. At Debug level, `Dual sampling: ... pairs/s` and `Sample validity: ...` logs every
five seconds report paired throughput and usable samples. Routine Info output
keeps initialization, address assignment, errors, gestures and per-slot distance
logs; raw chip-ID details and routine sampling statistics use Debug.

For troubleshooting, enable `ToF Distance Demo → Log gesture sampling diagnostics
every five seconds` (`CONFIG_TOF_GESTURE_DIAGNOSTICS`, off by default). The three
Info lines report actual paired rate, maximum loop/UI-publication latency, each
slot's valid/total samples, last range status and average/maximum read latency.
One-second distance logs are rate-limited and do not indicate the acquisition
rate. Diagnostics do not change ranging budgets or gesture thresholds.

On the tested pair of VL53L4CD boards with the same 20 ms budgets, synchronous
UI calls limited acquisition to 3.6–3.7 pairs/s and blocked for up to 278 ms.
Publishing UI state asynchronously raised acquisition to about 51–52 pairs/s,
with UI publication taking at most 1 ms. This is a measured setup result, not a
guaranteed rate. On both supported models, status 2 means signal fail/no confident
return. Near-field recognition treats it as a clear field and ignores its distance;
the cards still show `--`. Other invalid statuses remain unknown and can prevent
arming. Sensor validity checks and displayed distance validity are unchanged.

## Gesture Recognition

A panel below the two distance cards shows the last gesture and a 12-segment
animated light bar. Left-then-right distance pulses produce `SWIPE RIGHT` with
a left-to-right chase; right-then-left pulses produce `SWIPE LEFT` with the
opposite animation. Results remain visible for about 2.5 seconds. The LVGL timer
drives the animation independently of blocking measurements.

The near-field detector needs no background. Both sensors must be online, with
clear fields for 80 ms. A valid hand at or below 300 mm starts a pulse. A valid
distance at or above 360 mm, or signal-fail status 2, ends it; the intervening
60 mm band provides hysteresis. Invalid distance values never become fake far
measurements. Each pulse needs two near samples and at least 25 ms duration;
onsets must be separated by at least 25 ms, and both pulses must finish within
the configured window. Completion order must agree, allowing less than 25 ms
of sampling jitter. Simultaneous coverage alone is not a directional swipe.

When the near-field detector is not armed, the relative-distance detector can
learn a stable valid background, including a background inside the near-field
area. Once near-field recognition arms, it owns the sequence so a passing hand
is not learned as a new background. The two paths share an 800 ms cooldown and
cannot emit duplicate results for the same recognized sequence. Gestures that
occur entirely between samples can still be missed.

Bottom-panel messages:

| Message | Meaning |
|---|---|
| `WAIT FOR TWO SENSORS` | A sensor is disconnected or has unknown/fault data; signal-fail status 2 alone does not cause this |
| `HOLD STILL / CLEAR SENSORS` | Clear both fields, or keep a valid background stable, while the detectors rearm |
| `READY - WAVE LEFT OR RIGHT` | Ready to try a gesture |
| `SWIPE RIGHT` / `SWIPE LEFT` | Last detected gesture; its display does not imply the detector is already rearmed |

Relative-distance fallback rules:

- Default excursion threshold: 60 mm from each sensor's learned baseline.
- A pulse completes when the distance returns within 30 mm of its baseline.
- Increasing-then-decreasing and decreasing-then-recovering pulses are supported;
  both channels must have the same polarity and consistent onset/completion order.
- Both pulses must finish within 1500 ms of the first onset. Minimum onset
  separation is 25 ms, minimum pulse duration 25 ms, cooldown 800 ms.
- Simultaneous/ambiguous changes, missing modules, or stale samples over 500 ms
  discard the pending sequence. For connected sensors, invalid pairs are skipped
  for up to 80 ms without consuming invalid distances. Longer invalid intervals
  cancel the gesture and relearn its baseline. The baseline is also relearned
  after recognition, rejection or timeout. No valid background disables only this fallback; near-field recognition remains available.
- Each cycle alternates which sensor is measured first. Each sample is timestamped
  at its acquisition midpoint, and threshold crossing times are linearly estimated
  between samples instead of assigning a common frame timestamp. This reduces
  directional bias from sequential reads; it is not hardware synchronization.
- A clear onset order with tied completion times is accepted. Contradictory order
  or simultaneous onsets remain rejected. Individual sample ages are checked.
- The five-second Debug `Sample validity: left=... right=...` log compares usable
  readings on each side; large differences warrant checking alignment/backgrounds.

Tune `Gesture distance excursion threshold (mm)` and `Gesture completion window
(ms)` in `menuconfig → ToF Distance Demo`. Lower thresholds increase sensitivity
and false-trigger risk. No sensor address, offset or INT behavior is changed.
The settings are `CONFIG_TOF_GESTURE_THRESHOLD_MM` (default 60, range 20–500 mm)
and `CONFIG_TOF_GESTURE_WINDOW_MS` (default 1500, range 500–3000 ms).
`Near-field hand detection distance (mm)` (`CONFIG_TOF_GESTURE_NEAR_MM`, default
300, range 100–1000 mm) sets the hand zone; the clear threshold adds 60 mm.
Longer zones may include nearby furniture, so verify the installation.
The return threshold is half the excursion threshold. The 25 ms timing checks
and shared 800 ms cooldown are fixed in the detector/application, not menu settings.
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

Configure compensation in `idf.py menuconfig → ToF Distance Demo`:

| Model | Left slot | Right slot | Default adjustment |
|---|---|---|---|
| VL53L4CD | `CONFIG_TOF_L4CD_LEFT_OFFSET_ADJUST_MM` | `CONFIG_TOF_L4CD_RIGHT_OFFSET_ADJUST_MM` | 0 mm; preserve factory calibration |
| VL53L0X | `CONFIG_TOF_DISTANCE_OFFSET_ADJUST_MM` | `CONFIG_TOF_DISTANCE_RIGHT_OFFSET_ADJUST_MM` | −37 mm; retained from the source demo's legacy-board measurement |

The old −37 mm correction came from a VL53L0X board reporting 137 mm at a known
100 mm distance and 243 mm at 200 mm. It does not automatically apply to the
new default VL53L4CD or establish that another VL53L0X has the same error.
Model and slot choose the setting; verify each board before using nonzero values.

After each reset, the adjustment is added once to the initial sensor offset and
verified by readback. A zero adjustment skips the write. Signed quarter-millimeter
factory offsets and the VL53L4CD's other calibration registers are preserved.
The adjustment affects both display and logs, is stored in firmware configuration,
and does not write EEPROM/NVS. It is manual compensation, not automatic offset
or cross-talk calibration. Verify at known distances from each optical surface.

## Reference Documentation

- [VL53L4CD Datasheet (ST website, PDF)](https://www.st.com/resource/en/datasheet/vl53l4cd.pdf): default hardware and sensor specifications.
- [Component API and driver sources](../../components/mosaico_module_tof/README.md): automatic identification, address/timing/offset APIs and ST ULD licensing.

- [VL53L0X Datasheet (ST website, PDF, English)](https://www.st.com/resource/en/datasheet/vl53l0x.pdf):
  pin definitions, operating modes and specifications; DS11555 Rev 6 sections
  3.3.1–3.3.3 describe reference, offset and cross-talk calibration.

## Validation Scope

The combined firmware passes an ESP32-S31 build. A native LVGL check also covers
10,000 state publications while the display is blocked, latest-sample coalescing,
caller-data copying, pending gesture retention, independent slots, reconnect and
invalid-data rendering. Host checks under UBSan cover
VL53L4CD dual addressing and reads, VL53L0X API routing, timing changes, signed
offsets, factory-register preservation, peer isolation, failed address rebinding
and read-error invalidation. Both swipe directions have been observed on the dual VL53L4CD hardware after
these changes. Board testing confirmed `READY` with both cards showing
`-- / NO TARGET`, followed by left/right swipes without a measurable background.
Sampling remained about 51 pairs/s. Mixed-model gesture behavior still requires
hardware validation; these observations are not a measured recognition success rate.

Host regression tests cover both directions/polarities, overlapping pulses,
tied completion times, noise, invalid data, timeouts, cooldown, timestamp wrap
and varying acquisition phases with alternating read order. The original simulated phase
sweep passes 12/12 cases per direction, and a 50 Hz phase/polarity sweep passes
80/80 cases. Dropout tests cover 60 ms missing bursts and rejection of longer
blind intervals. An empty-background phase sweep passes 40/40 direction/phase
cases, including timestamp wrap; classification, error, simultaneous-coverage
and one-sample-spike rejection are checked. These are algorithm checks, not a
hardware success rate.

On hardware, verify both boards present at boot, either board alone, removal and
reinsertion, independent address logs and per-board compensation. Compare repeated
left/right swipes with the same path, speed and background, alongside `Sample
validity` and `Dual sampling` logs. Mounting, background reflectivity and motion
speed can still affect recognition.
