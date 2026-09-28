# Ambient light ADC — ESP32-S31

环境光传感器模拟输出接 **GPIO48（当前 IDF 单端接口为 ADC1 CH6）**，与开发板共地。目标输入范围 0～2 V。
图中的 `ADC1_CH3_N` 不等于 SDK 的 `ADC_CHANNEL_3`；程序使用 `adc_oneshot_io_to_channel()` 查询 GPIO48 的映射。
按传感器规格供电，不要将电源或 5 V 信号接入 ADC；该引脚不能同时用于摄像头 PWDN 或其他外设。
本工程不启用摄像头、Wi-Fi、NAND 或屏幕触摸。沿用 BSP 屏幕驱动，显示逆时针旋转 90°。

每 100 ms 采集 16 次并取均值，显示原始计数、电压和 0～2 V 条形图。
ADC 采样在应用任务中进行，不在 LVGL 锁内；读取失败显示错误而不是保留旧电压。
这是传感器输出电压测试，不换算 lux（需要传感器型号及响应曲线）。

## S31 电压校准（必须）

当前本地 ESP-IDF 的 S31 `soc_caps.h` 声明 17 位 ADC，只有一种衰减；
`adc_cali_schemes.h` 尚未启用校准算法。不能用其他 ESP32 的 4095/3.3 V
或 12 dB 公式。未启用校准时只显示 RAW 和 `Calibration required`，电压显示 `--.--- V`。
当前工程已按本板实测值启用两点换算：0 V → RAW 2197，2 V → RAW 162；
同时写入 `sdkconfig.defaults` 和当前 `sdkconfig`。支持负斜率，不要求 RAW 随电压增大。
这只确定两个端点的线性映射，尚未验证 ADC 在中间区域是否线性或存在回绕/饱和；
务必输入 1.000 V 复核，此时预期 RAW 约为 1179～1180。若相差明显，应检查
输入接线、ADC 模式和传递特性，不能仅凭两个端点认定电压准确。换板后应重新校准。
在确定芯片输入范围允许 2 V 后，使用稳定电压源、万用表完成以下校准：

1. 断开传感器输出，GPIO48 接 GND，记录稳定的 RAW 均值。
2. GPIO48 输入经过万用表确认的 2.000 V（共地），记录 RAW 均值。
3. `idf.py menuconfig` → `Ambient light ADC`，启用 `Use measured two-point calibration`，
   填入两个实际 RAW 值。两点不能相等；不要把本工程预填的本板测量值当成其他板的校准值。
4. 重新编译烧录，接回传感器。公式：`mV=(RAW-RAW_0V)*2000/(RAW_2V-RAW_0V)`。

两点校准修正增益和偏移，不消除 ADC 非线性；请用中间电压（如 1 V）复核。
切换到 GPIO48 后应重新校准，不要沿用原 GPIO55 的校准值。
超出 0～2 V 时数字不截断，并显示越界提示；这不替代硬件输入保护。

## 编译烧录

```sh
cd examples/ambient_light_adc
idf.py set-target esp32s31
idf.py build
idf.py -p PORT flash monitor
```

使用当前仓库对应的 ESP-IDF master（6.2）及 ESP-Mosaico 屏幕/PSRAM配置。
保留 USB Serial/JTAG 日志。首次构建需要访问组件注册表。

换算函数主机测试（在工程目录运行）：

```sh
cc -std=c11 -Wall -Wextra -Werror -I main tests/test_voltage_conversion.c -o /tmp/test-light-voltage
/tmp/test-light-voltage
```
