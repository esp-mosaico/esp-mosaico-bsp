# ESP-Mosaico BSP

[Overview](#overview) | [Features](#features) | [Getting Started](#getting-started) | [Components](#components) | [Examples](#examples) | [License](#license)

## Overview

ESP-Mosaico BSP is an ESP-IDF Board Support Package for the ESP-Mosaico development board. It provides a unified API for the onboard display, touch controller, audio codec, sensors, storage, power control, user input, and two hot-pluggable expansion slots.

The repository also includes reusable components for magnetic interaction, ESP-NOW peer communication, multi-device topology, and application messaging, together with standalone examples ranging from basic peripheral bring-up to camera, voice, AI, and multi-board applications.

## Compatibility

| Item | Support |
| --- | --- |
| SoC | ESP32-S31 |
| ESP-IDF | 6.1 or newer |

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
| Yes | Feedback | Status LED and vibration motor |
| Yes | USB | USB OTG |
| Yes | Expansion | Left and right hot-pluggable module slots |

The main public header is [`include/bsp/esp_mosaico.h`](include/bsp/esp_mosaico.h). Peripheral-specific APIs are provided by the headers under [`include/bsp`](include/bsp).

## Getting Started

### Prerequisites

- ESP-IDF 6.1 or newer with ESP32-S31 target support
- An ESP-Mosaico board
- A data-capable USB or UART cable suitable for the selected example

Follow the [ESP-IDF Get Started guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/get-started/index.html) to install ESP-IDF and activate its environment.

### Use the BSP API

Include the board header to access all public BSP interfaces:

```c
#include "bsp/esp_mosaico.h"

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_power_init());
    ESP_ERROR_CHECK(bsp_led_init());
    ESP_ERROR_CHECK(bsp_led_set(true));
}
```

Applications should check the return value of every BSP function. Use the high-level peripheral and expansion-module APIs where available instead of configuring shared board resources directly.

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
├── include/bsp/          Public BSP headers
├── onboard/              Onboard peripheral implementations
├── common_components/    Shared interaction, networking, and device components
├── expansion_modules/    Hot-pluggable expansion-module drivers
└── examples/              Standalone ESP-IDF applications
```

## Components

### Board and common components

| Component | Description |
| --- | --- |
| [`esp-mosaico-bsp`](idf_component.yml) | Board-level display, touch, audio, sensors, storage, power, input, and expansion-slot APIs |
| [`esp_lcd_co5300`](common_components/esp_lcd_co5300) | CO5300 SPI, QSPI, and MIPI-DSI LCD driver with Sleep In and Deep Standby support |
| [`mosaico_module_mgr`](common_components/mosaico_module_mgr) | Expansion-module discovery, descriptor validation, and exclusive slot lifecycle management |
| [`mosaico_interaction`](common_components/mosaico_interaction) | Hardware-independent magnetic classification, topology, and game primitives |
| [`mosaico_peer_link`](common_components/mosaico_peer_link) | ESP-NOW transport, contact-session negotiation, and topology propagation |
| [`mosaico_network`](common_components/mosaico_network) | High-level multi-device topology, routing, and application messaging service |

### Expansion modules

| Component | Description | Slot support |
| --- | --- | --- |
| [`mosaico_module_camera`](expansion_modules/mosaico_module_camera) | Managed OV3640 DVP camera with optional hardware JPEG decoding helpers | Left only |
| [`mosaico_module_button_led`](expansion_modules/mosaico_module_button_led) | Two-button and three-WS2812 expansion module | Left or right |
| [`mosaico_module_joystick`](expansion_modules/mosaico_module_joystick) | Dual-axis joystick with five buttons and non-blocking calibration | Left or right |

The two slots share board resources. Applications should use a concrete module driver, which discovers and claims the required slot through `mosaico_module_mgr`, rather than controlling connector pins directly.

## Examples

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

These examples require an OV3640 Camera module installed in the left expansion slot.

| Example | Description |
| --- | --- |
| [`camera_lcd_preview`](examples/camera_lcd_preview) | Live camera preview on the onboard LCD |
| [`camera_photo_app`](examples/camera_photo_app) | Touch camera UI, JPEG storage in SPI NAND, and optional USB MSC export |
| [`wifi_http_cam`](examples/wifi_http_cam) | SoftAP HTTP snapshot and MJPEG streaming server |
| [`ai_model_gallery`](examples/ai_model_gallery) | Face and COCO object-detection model gallery |

### Voice, audio, and USB

| Example | Description |
| --- | --- |
| [`gmf_audio_player`](examples/gmf_audio_player) | Embedded MP3 playback using ESP-GMF |
| [`voice_tts`](examples/voice_tts) | Chinese text-to-speech using ESP-SR Xiaole voice data |
| [`voice_wake_cmd`](examples/voice_wake_cmd) | Wake word and Chinese speech-command recognition with TTS feedback |
| [`usb_extend_screen`](examples/usb_extend_screen) | Windows secondary display with optional HID touch and UAC audio |

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
- Expansion-module EEPROM programming is a manufacturing operation and is not part of the public example workflow.
- The checked-in magnetic calibration profile is specific to the tested mechanical arrangement. Recalibrate and revalidate it after changing magnets, sensor orientation, enclosure, or assembly tolerances.

## Resources

- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/)
- [ESP Component Registry](https://components.espressif.com/)
- [ESP-IoT-Solution](https://github.com/espressif/esp-iot-solution)

## License

This project is licensed under the Apache License 2.0. See [`LICENSE`](LICENSE) for details.
