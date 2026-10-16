# 热成像示例

使用 ESP-Mosaico LCD 显示 MLX90640 的 32 × 24 热成像，包含伪彩色、3 × 3 中值滤波、双线性放大、自动温度色阶，以及环境、最低、最高和中心温度。显示处理留在示例中，传感器采集由[mosaico_module_thermal 组件](../../components/mosaico_module_thermal) 完成。

## 硬件与编译

- ESP-Mosaico / ESP32-S31，ESP-IDF 6.2 开发环境及 PSRAM。
- 子板共享总线 SDA=GPIO0、SCL=GPIO1，只接一块默认地址为 `0x33` 的 MLX90640。示例通过模块管理器初始化 BSP 子板电源和对应板型的总线。
- 子板 EEPROM 必须包含有效的 Thermal Board 描述符（`MOSAICO_BOARD_TYPE_THERMAL`，类型 `0x12`）。示例自动发现并占用左侧或右侧槽位；可用 `module_eeprom_programmer` 示例烧写描述符。不要连接同地址的第二块传感器。
- BSP 按 eFuse 识别板型，需要有效的板型配置。显示根据 BSP 槽位方向自动补偿：左槽按传感器安装方向旋转 180°，右槽叠加槽位的 180° 后按 0° 显示；换槽后方向随新帧一起更新。

```sh
cd examples/thermal_camera
idf.py build
idf.py -p PORT flash monitor
```

`sdkconfig.defaults` 已指定 `esp32s31`、显示和 PSRAM。示例关闭触摸，LVGL 任务栈为 16 KB，主任务栈为 8 KB；采集任务固定在 CPU0，优先级 7、栈 10 KB，避免低优先级主任务在显示渲染期间延迟读取。默认参数为 400 kHz I²C、16 Hz 子页刷新率、约 8 Hz 完整图像、0.95 发射率；可在 `main.c` 中修改配置。反射温度默认估计为环境温度减 8°C，实际测温需按目标材料和环境调整。

## 运行行为

通过模块管理器发现并占用热成像板后初始化驱动，丢弃前两幅完整帧，然后持续显示，每 32 帧记录一次环境温度和供电电压。单次完整采集的 I/O 截止时间为 1 秒；读取失败或检测到模块移除后清除旧画面，显示等待状态，先销毁传感器再释放槽位，然后重新发现模块。恢复连接后重新读取校准参数并等待稳定帧。

本示例从 `test/subboard_test_lh` 分支的同名示例迁移而来，保留其显示算法；已完成 ESP32-S31 编译和驱动主机测试，已在 ESP-Mosaico v1.1（BSP 按 v1.2 映射）实板验证左槽识别与连续采集。右槽热图方向已由现场确认正确；温度精度和完整的双向热插拔流程尚未完成验证。


## 实板验证记录（2026-10-16）

使用 `/dev/ttyACM0`（ESP-Prog-2）烧录，目标 ESP32-S31，左槽模块识别为 `Thermal Camera(0x12)` / `ThermalBoard`。使用已有构建环境 `/home/zhouli/esp/esp-idf-6.2`（日志版本 `v6.1-dev-8142-g188e3e55bb2-dirt`），烧录命令为 `idf.py -p /dev/ttyACM0 -b 1152000 flash`，串口监视波特率 115200。

实测发现低优先级主任务采集会出现帧覆盖；1 MHz、跨核采集配置下还出现过 I²C 接收中断崩溃。当前默认改为 400 kHz、CPU0 优先级 7 的独立采集任务，并在同一截止时间内重试被覆盖的帧。启动观察到一次辅助数据校验失败，示例自动释放并重新初始化后恢复采集。持续运行结果见下方记录；这些结果不代表高刷新率或其他总线配置已通过验证。

最终观察到开机后 179.9 秒、第 1376 幅完整图像，平均约 7.82 帧/秒；启动恢复后的持续监视中未见采集错误、异常重启或崩溃。Vdd 约 3.35 V，温度数据持续更新。日志保存在 `/tmp/thermal-hw-monitor-400k.log` 和 `/tmp/thermal-hw-monitor-soak-20261016.log`。

后续修正了固定 180° 显示的问题，改为根据 BSP 槽位方向逐帧补偿。修正版已重新烧录到 `/dev/ttyACM0`，用户确认右槽热图方向正确；运行日志为 `/tmp/thermal-rotation-monitor.log`。
