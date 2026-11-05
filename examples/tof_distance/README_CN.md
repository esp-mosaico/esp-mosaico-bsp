[English](README.md) | [简体中文](README_CN.md)

# ToF 测距示例

在 ESP-Mosaico LCD 上独立显示左右距离并识别左右挥手。默认传感器为 **VL53L4CDV0DH/1**（VL53L4CD），同时支持 **VL53L0CXV0DH/1**（VL53L0X）。固件根据芯片 ID 自动选择对应驱动，受支持型号可以混用。

## 硬件

- 带 PSRAM 的 ESP-Mosaico / ESP32-S31，以及 ESP-IDF 6.2 或更新版本环境。
- 一块或两块 ToF 子板，EEPROM 需包含有效描述符（类型 `0x10`）。如需写入，可使用 [module_eeprom_programmer](../module_eeprom_programmer)。
- 单侧可独立测距，左右手势识别需要同时连接两侧。

两个型号的出厂地址均为 `0x29`，示例自动分配独立地址：

| 插槽 | XSHUT | 传感器地址（7 位） |
|---|---|---|
| 左侧 | GPIO48 | `0x30` |
| 右侧 | GPIO47 | `0x31` |

运行期间两个插槽均专用于 ToF 子板，示例会控制其 XSHUT 引脚，请勿连接其他类型子板。

## 编译与烧录

激活 ESP-IDF 环境后，从仓库根目录执行：

```sh
cd examples/tof_distance
idf.py build
idf.py -p PORT flash monitor
```

将 `PORT` 替换为主板串口。

## 使用方法

两张卡片分别显示各侧的型号、地址、毫米距离、状态、信号／环境光计数率（MCPS）及距离条。无效读数显示 `--`，空旷无目标时显示 `NO TARGET`。距离条范围为 0～2000 mm，超出刻度的有效距离仍显示实际数值。示例不启用触摸。

使用手势功能时：

1. 连接两侧传感器，清空感应区，等待 `READY - WAVE LEFT OR RIGHT`。前方空旷、两侧均显示 `--` 时也可就绪，无需有效背景距离。
2. 在 30 cm 内从左侧传感器前挥向右侧，然后移出两侧感应区；屏幕显示 `SWIPE RIGHT`，灯条向右运动。反向操作显示 `SWIPE LEFT`。
3. 等待检测器重新就绪后再做下一次动作。方向按主板左右插槽定义。

| 提示 | 含义 |
|---|---|
| `WAIT FOR TWO SENSORS` | 传感器缺失或数据不可用 |
| `HOLD STILL / CLEAR SENSORS` | 清空感应区或保持背景稳定 |
| `READY - WAVE LEFT OR RIGHT` | 可以开始挥手 |
| `SWIPE RIGHT` / `SWIPE LEFT` | 最近识别出的手势 |

两个型号均使用 20 ms 测量预算进行连续测距，实际采样率取决于传感器和总线条件。断开或通信失败时清除对应卡片，重新连接后自动恢复。

## 配置

执行 `idf.py menuconfig`，进入 **ToF Distance Demo**：

| 配置项 | 默认值 |
|---|---|
| `CONFIG_TOF_GESTURE_NEAR_MM`：手部检测距离 | 300 mm |
| `CONFIG_TOF_GESTURE_THRESHOLD_MM`：相对背景的手势距离变化门限 | 60 mm |
| `CONFIG_TOF_GESTURE_WINDOW_MS`：手势完成时间窗口 | 1500 ms |
| `CONFIG_TOF_GESTURE_DIAGNOSTICS`：每五秒输出采样诊断 | 关闭 |

降低门限会提高灵敏度，也可能增加误触发。请让附近物体处于手部检测区域之外。

### 距离偏移

在同一菜单中按型号、插槽分别设置：

| 型号 | 左侧配置 | 右侧配置 | 默认值 |
|---|---|---|---|
| VL53L4CD | `CONFIG_TOF_L4CD_LEFT_OFFSET_ADJUST_MM` | `CONFIG_TOF_L4CD_RIGHT_OFFSET_ADJUST_MM` | 0 mm |
| VL53L0X | `CONFIG_TOF_DISTANCE_OFFSET_ADJUST_MM` | `CONFIG_TOF_DISTANCE_RIGHT_OFFSET_ADJUST_MM` | −37 mm |

VL53L0X 默认补偿仅针对原始子板。应用补偿前，请分别用已知距离检查每颗传感器；设为 0 表示不额外补偿。负值会减小读数，补偿同时影响屏幕和日志，保存在固件配置中。

## 日志与参考

启动日志列出总线上应答的 I²C 地址。扫描期间 ToF 传感器保持关断，稍后通过分配后的地址通信。各侧有效距离日志最多每秒一次，不能据此判断采样率；排查手势漏识别时可开启采样诊断。

- [ToF 组件 API 与驱动文档](../../components/mosaico_module_tof/README.md)
- [VL53L4CD 数据手册](https://www.st.com/resource/en/datasheet/vl53l4cd.pdf)
- [VL53L0X 数据手册](https://www.st.com/resource/en/datasheet/vl53l0x.pdf)
