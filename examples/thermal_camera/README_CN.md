[English](README.md) | [简体中文](README_CN.md)

# 热成像示例

在 ESP-Mosaico LCD 上显示 MLX90640 的 32 × 24 热图，支持全宽显示、自动温度色阶和触摸测温。

## 硬件

- 带 PSRAM 的 ESP-Mosaico / ESP32-S31，以及 ESP-IDF 6.2 或更新版本环境。
- 在任一插槽连接一块 MLX90640 热成像板，EEPROM 需包含有效的 Thermal Board 描述符（类型 `0x12`）。如需写入描述符，可使用 [module_eeprom_programmer](../module_eeprom_programmer)。
- 子板 I²C：SDA=GPIO0、SCL=GPIO1，传感器地址为 `0x33`。同时只能连接一块热成像板。

## 编译与烧录

激活 ESP-IDF 环境后，从仓库根目录执行：

```sh
cd examples/thermal_camera
idf.py build
idf.py -p PORT flash monitor
```

将 `PORT` 替换为主板串口。

## 使用方法

- 顶部显示 `ESP-Mosaico | Module Thermal Camera`。热图铺满屏幕宽度，上下留白相同，方向随插槽自动调整。
- 点击热图选点，白框标出位置，上方显示温度。拖动可移动选点，松手后保留位置，温度持续更新。
- 底栏显示环境、最低、最高和中心温度，以及帧编号。放大显示不增加传感器的 32 × 24 原生分辨率。
- 断开模块后清除图像和选点；重新连接后自动恢复成像，点击即可重新选点。

## 配置

默认使用 400 kHz I²C、16 Hz 子页刷新（约每秒 8 幅完整图像）、0.95 发射率，反射温度估计为环境温度减 8°C。可在 `main/main.c` 中按目标材料和环境调整传感器配置。

传感器规格和 API 见[热成像组件文档](../../components/mosaico_module_thermal/README.md)。
