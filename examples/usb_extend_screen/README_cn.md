## USB 扩展屏示例 (ESP-Mosaico)

本示例参考 [esp-iot-solution USB 扩展屏](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/device/usb_extend_screen)，将 ESP-Mosaico 开发板作为 Windows 副屏使用。PC 通过 USB High Speed 口发送 JPEG 帧，设备端硬件 JPEG 解码后在 CO5300 480×480 屏幕上显示，并支持 HID 触摸回传与 UAC 音频。

## 功能

- 默认使用原生 480×480 RGB565 无损传输，避免 JPEG 在文字和鼠标边缘产生振铃花纹
- 最多 5 点触摸，通过 HID 回传给 PC
- ES8311 音频输入/输出，48 kHz 立体声

## 硬件连接

1. 将 Mosaico 板上的 **USB High Speed (OTG)** 口连接到 PC
2. 确保屏幕与 ES8311 音频电路已上电（示例会自动打开 VCC_3V3 与背光）

## 编译与烧录

```bash
cd examples/usb_extend_screen
idf.py set-target esp32s31
idf.py build flash monitor
```

首次编译会从组件仓库拉取 `tinyusb` 与 `usb_device_uac`，请确保网络可用。

> 本示例会占用 USB-OTG 端口作为扩展屏设备，已默认关闭 BSP 的 USB CDC 控制台。调试日志请使用 UART 串口。

## PC 端驱动

Windows 10/11 需要安装 Espressif 间接显示驱动 (IDD)。下载与安装说明见 esp-iot-solution 文档：

- [Windows 驱动说明](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/device/usb_extend_screen/windows_driver)
- [驱动安装包下载](https://dl.espressif.com/AE/esp-iot-solution/xfz1986_usb_graphic_250224_rc_sign.exe)

复合设备（显示 + 触摸 + 音频）使用 `USB\VID_303A&PID_2986`。若只需纯显示功能，可在 `menuconfig` 中关闭触摸和音频，并将 PID 改为 `0x2987`。

## 配置项

| 配置项 | 说明 |
|--------|------|
| `CONFIG_USB_EXTEND_SCREEN_WIDTH` / `HEIGHT` | PC 端逻辑分辨率，默认 480×480；数值越小，内容越大但会降低清晰度 |
| `CONFIG_USB_EXTEND_SCREEN_MAX_FPS` | 上报给 PC 驱动的最大帧率 |
| `CONFIG_USB_EXTEND_SCREEN_JPEG_QUALITY` | Vendor 字符串中的 JPEG 质量提示 |
| `CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_B` | 单帧 JPEG 最大字节数 |
| `CONFIG_HID_TOUCH_ENABLE` | 是否启用 HID 触摸 |
| `CONFIG_UAC_AUDIO_ENABLE` | 是否启用 UAC 音频 |

## 常见问题

### 触摸屏控制的不是扩展屏

在 Windows「平板电脑设置」中重新运行设置向导，选择 Mosaico 对应的扩展屏。

### 音频卡顿时降低帧率

适当减小 `CONFIG_USB_EXTEND_SCREEN_MAX_FPS`，为 UAC 留出 USB 带宽。

### 烧录与监控

烧录与串口监控请使用 UART 口；USB-OTG 在运行本示例时由扩展屏协议占用。
