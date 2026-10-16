# ESP-Mosaico BSP

[Overview](#overview) | [Features](#features) | [Getting Started](#getting-started) | [Components](#components) | [Examples](#examples) | [License](#license)

## Overview

ESP-Mosaico BSP is an ESP-IDF Board Support Package for the ESP-Mosaico development board. It provides a unified API for the onboard display, touch controller, audio codec, sensors, storage, power control, user input, and two hot-pluggable expansion slots.

The repository also includes reusable components for magnetic interaction, ESP-NOW peer communication, multi-device topology, and application messaging, together with standalone examples ranging from basic peripheral bring-up to camera, voice, AI, and multi-board applications.

## Compatibility

| Item | Support |
| --- | --- |
| SoC | ESP32-S31 |
| ESP-IDF | 6.2 or newer |
| Board revisions | v1.0 and v1.2 (v1.1 uses the v1.2 mapping) |

ESP32-S31 support may require the preview target command provided by the selected ESP-IDF release:

```bash
idf.py --preview set-target esp32s31
```

## Features

| Available | Capability | Device or interface |
| --- | --- | --- |
| Yes | Display | 480 x 480 CO5300 QSPI LCD |
| Yes | Touch | CST9217 capacitive touch controller |
| Yes | Audio | ES8311 microphone and speaker codec |
| Yes | Motion sensing | BMI270 IMU |
| Yes | Magnetic sensing | Two BMM150 magnetometers |
| Yes | Battery monitoring | BQ27220 fuel gauge |
| Yes | Storage | SPI NAND flash |
| Yes | User input | AI and BOOT buttons |
| Yes | Feedback | Vibration motor; status LED on v1.0 |
| Yes | USB | USB OTG |
| Yes | Expansion | Left and right hot-pluggable module slots |

The main public header is [`components/esp-mosaico-bsp/include/bsp/esp_mosaico.h`](components/esp-mosaico-bsp/include/bsp/esp_mosaico.h). Peripheral-specific APIs are provided by the headers under [`components/esp-mosaico-bsp/include/bsp`](components/esp-mosaico-bsp/include/bsp).

## Getting Started

### Prerequisites

- ESP-IDF 6.2 or newer with ESP32-S31 target support
- An ESP-Mosaico board
- A data-capable USB or UART cable suitable for the selected example

Follow the [ESP-IDF Get Started guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/get-started/index.html) to install ESP-IDF and activate its environment.

### Use the BSP API

Include the board header to access all public BSP interfaces:

```c
#include "bsp/esp_mosaico.h"

void app_main(void)
{
    bsp_board_variant_t variant;
    ESP_ERROR_CHECK(bsp_board_variant_get(&variant));
    ESP_ERROR_CHECK(bsp_power_init());
    if (variant == BSP_BOARD_VARIANT_V1_0) {
        ESP_ERROR_CHECK(bsp_led_init());
        ESP_ERROR_CHECK(bsp_led_set(true));
    }
}
```

The BSP reads the hardware version from eFuse USER_DATA before configuring revision-dependent pins. Unsupported or unprogrammed versions return `ESP_ERR_NOT_SUPPORTED`. Applications should check the return value of every BSP function and use the high-level peripheral and expansion-module APIs instead of configuring shared board resources directly.

### Configuration

Run `idf.py menuconfig` and open `ESP-Mosaico BSP` to configure the board-specific options:

- CO5300 tearing-effect synchronization
- LCD QSPI drive strength
- SPI NAND QIO mode
- Motor PWM control
- VCC 3.3 V soft-start time

## Repository Structure

```text
.
├── components/           BSP and shared reusable components
└── examples/              Standalone ESP-IDF applications
```

## Components

### Board and common components

| Component | Description |
| --- | --- |
| [`esp-mosaico-bsp`](components/esp-mosaico-bsp) | Board-level display, touch, audio, sensors, storage, power, input, and expansion-slot APIs |
| [`mosaico_module_mgr`](components/mosaico_module_mgr) | Expansion-module discovery, descriptor validation, and exclusive slot lifecycle management |
| [`mosaico_interaction`](components/mosaico_interaction) | Hardware-independent magnetic classification, topology, and game primitives |
| [`mosaico_peer_link`](components/mosaico_peer_link) | ESP-NOW transport, contact-session negotiation, and topology propagation |
| [`mosaico_network`](components/mosaico_network) | High-level multi-device topology, routing, and application messaging service |

### Expansion modules

| Component | Description | Slot support |
| --- | --- | --- |
| [`mosaico_module_camera`](components/mosaico_module_camera) | Managed OV3640/SC101IOT DVP capture, explicit frame lifecycle, flash control, and optional hardware JPEG decoding helpers | Left only |
| [`mosaico_module_joystick`](components/mosaico_module_joystick) | Dual-axis joystick with five buttons and non-blocking calibration | Left or right |
| [`mosaico_module_interact`](components/mosaico_module_interact) | Touch/GPIO buttons, PIR motion, ambient light, six WS2812 LEDs, and NEC infrared transmission | Left or right |
| [`mosaico_module_tof`](components/mosaico_module_tof) | VL53L0X single-shot ranging and board initialization | Left or right, one module |
| [`mosaico_module_thermal`](components/mosaico_module_thermal) | MLX90640 thermal imaging with complete frame acquisition, bad pixel correction, and timeout handling | Left or right, one module on the shared bus |
| [`mosaico_module_matrix_led`](components/mosaico_module_matrix_led) | 8 x 8 WS2812 matrix with buffered pixel writes and RMT DMA output | Left or right |

The two slots share the subboard I2C bus and other board resources. `mosaico_module_mgr` validates EEPROM descriptors, reports presence and ownership changes, and provides exclusive slot claims. Camera, Joystick, and Interaction drivers discover and claim their slots automatically. The Matrix LED driver takes an explicit slot; the application must enable board power, claim the slot, and release it after deleting the LED strip, as shown in [`matrix_rgb_led`](examples/matrix_rgb_led).

Standard module EEPROMs use `0x50` in the left slot and `0x51` in the right slot. The Camera module has a fixed `0x50` address and supports only the left slot. EEPROM board-type definitions and programmer profiles do not imply that a dedicated runtime driver exists for every module type.

## Examples

Each example is a standalone ESP-IDF project. Follow its README for build steps, configuration, and hardware requirements.

### Board peripherals and product foundations

| Example | Description |
| --- | --- |
| [`button_led_test`](examples/button_led_test) | Onboard AI button events and status LED control |
| [`audio_loopback`](examples/audio_loopback) | ES8311 microphone-to-speaker audio loopback |
| [`hmi_demo`](examples/hmi_demo) | CO5300 display, CST9217 touch, and LVGL benchmark |
| [`imu_gesture`](examples/imu_gesture) | BMI270 shake, tilt, and flip recognition |
| [`low_power_wakeup`](examples/low_power_wakeup) | Board shutdown sequence and timer deep-sleep wakeup |
| [`device_settings`](examples/device_settings) | Versioned and validated product settings stored in NVS |

### Camera and AI

These examples use a Camera module in the left expansion slot through `mosaico_module_camera`, which supports OV3640 and SC101IOT sensors. The current capture paths use UYVY frames; the requested resolution must be supported by the detected sensor. OV3640-specific tuning and flash-exposure register operations are skipped for SC101IOT.

| Example | Description |
| --- | --- |
| [`camera_lcd_preview`](examples/camera_lcd_preview) | UYVY capture with PPA center-crop and RGB565 conversion for live LCD preview |
| [`camera_photo_app`](examples/camera_photo_app) | Touch camera UI, flash capture, SPI NAND photo gallery, and optional USB MSC export (disabled by default) |
| [`wifi_http_cam`](examples/wifi_http_cam) | SoftAP HTTP snapshots and MJPEG streaming with hardware JPEG encoding of UYVY frames |
| [`ai_model_gallery`](examples/ai_model_gallery) | Face and COCO object detection with PPA conversion of camera frames for inference |

### Voice, audio, and USB

| Example | Description |
| --- | --- |
| [`gmf_audio_player`](examples/gmf_audio_player) | Embedded MP3 playback using ESP-GMF |
| [`voice_tts`](examples/voice_tts) | Chinese text-to-speech using ESP-SR Xiaole voice data |
| [`voice_wake_cmd`](examples/voice_wake_cmd) | Wake word and Chinese speech-command recognition with TTS feedback |
| [`usb_extend_screen`](examples/usb_extend_screen) | Windows secondary display with optional HID touch and UAC audio |

### Expansion modules and provisioning

| Example | Description | Additional hardware |
| --- | --- | --- |
| [`interaction_board_demo`](examples/interaction_board_demo) | Hot-plug dashboard for touch/GPIO buttons, PIR, light, six LEDs, and NEC IR transmission | One Interaction module in either slot |
| [`tof_distance`](examples/tof_distance) | ToF module discovery, hot-plug recovery in either slot, live display dashboard, and serial distance readings | One VL53L0X module with a valid EEPROM descriptor |
| [`thermal_camera`](examples/thermal_camera) | Thermal module discovery, temperature display, and reconnection retries | One MLX90640 module with a valid EEPROM descriptor |
| [`matrix_rgb_led`](examples/matrix_rgb_led) | Eight looping 8 x 8 RGB animations with LCD preview, slot discovery, and hot-plug recovery | One Matrix LED module with a valid EEPROM descriptor in either slot |
| [`module_slot_scan`](examples/module_slot_scan) | Split-screen slot monitor with left-slot camera preview, Interaction controls, and names for other identified modules | Modules to insert or remove from the two slots |
| [`module_eeprom_programmer`](examples/module_eeprom_programmer) | Touchscreen selection, writing, and read-back verification of AT24C02 module descriptors | One module at a time; manufacturing/provisioning tool |

The EEPROM programmer writes only after **Start Write** is tapped. Its current profiles are Camera, Button LED, Sensor, TOF, Matrix LED, Thermal, Relay, IO Test, and Interaction; it does not currently offer a Joystick profile.

### Multi-device interaction

| Example | Description | Additional hardware |
| --- | --- | --- |
| [`espnow_chat`](examples/espnow_chat) | Minimal multi-board chat over ESP-NOW | Two or more Mosaico boards |
| [`magnetic_interaction_demo`](examples/magnetic_interaction_demo) | Magnetic contact, topology reconstruction, routing, and multi-screen energy relay | Two or more Mosaico boards |
| [`dual_screen_pong`](examples/dual_screen_pong) | Synchronized two-screen Pong over ESP-NOW | Two boards and two Joystick modules |

## Hardware Notes

- The Camera module is supported in the left slot only.
- Both expansion slots share I2C and other multiplexed resources. Slot ownership is exclusive while a concrete module driver is active.
- Some camera signals conflict with USB Serial/JTAG pins. Camera examples that document this conflict must be flashed and monitored through UART.
- Use `module_eeprom_programmer` for manufacturing or descriptor provisioning, with one module connected at a time.
- The checked-in magnetic calibration profile is specific to the tested mechanical arrangement. Recalibrate and revalidate it after changing magnets, sensor orientation, enclosure, or assembly tolerances.

## Resources

- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/)
- [ESP Component Registry](https://components.espressif.com/)
- [ESP-IoT-Solution](https://github.com/espressif/esp-iot-solution)

## License

This project is licensed under the Apache License 2.0. See [`LICENSE`](LICENSE) for details.
