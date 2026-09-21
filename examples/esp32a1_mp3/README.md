# ESP32-A1 Audio Board Music Player

本示例面向 **ESP-Mosaico V1.2**，使用当前仓库中的
`components/esp-mosaico-bsp`，实现 Si12T 电容触摸按键控制的 MP3 播放器。

挂载 NAND 时先只读校验 LittleFS 超级块，再沿用盘上原有块数。
例如盘上为 58595 块、当前 Dhara 提供 59534 块时，仍按 58595 块挂载，
不格式化、不扩容。盘上容量超过当前设备容量时拒绝挂载。MP3 工程保持只读访问。
注意本工程与 `esp32a1_recorder` 是独立固件，请在对应目录编译、烧录。

## 启动加载界面

顶部显示 `MP3 DEMO`，加载期间使用旋转圆环和循环跳动的音柱动画（不表示播放状态或实际加载百分比）。
启动音量为 50%，加载完成后默认暂停，不自动开始解码播放；点击屏幕播放按钮或 TK7 开始播放。
上一首/下一首仍保留切歌并播放的行为。

启动后先显示“资源加载中”和旋转圆环，播放器按钮暂不显示。
A1 初始化、NAND 挂载和歌曲扫描在独立的低优先级任务中执行，不持有 LVGL 界面锁，
动画由 LVGL 任务刷新；加载提示随后切换到触摸按键初始化阶段。
播放器和 Si12T 均初始化成功后，自动切换到 MP3 控制界面并开放操作。
初始化失败则停止转圈，显示“加载失败”和错误提示，检查硬件、文件后重启。

启动页中文字形由 `main/loading_font.c` 提供，仅包含加载提示所需字符，
不会改变歌曲标题的字体支持范围。字形来源为 Noto Sans CJK，许可见
`main/LOADING_FONT_LICENSE.txt`；生成脚本为 `tools/generate_loading_font.py`（依赖 Pillow）。

## 硬件与总线

耳机检测 Audio_Det 使用 GPIO47（低电平表示插入）：每 10 ms 采样，低电平稳定
100 ms 确认插入，高电平连续稳定 300 ms 才确认拔出、允许喇叭恢复。
检测到任何插入迹象即关闭 PA；抖动会重新计时，慢速插入期间的短暂高电平不会反复开启喇叭。
暂停/恢复和切歌统一使用这一消抖状态；PA GPIO38 仅由应用管理，codec 不再直接控制。
参数见 `main/headphone_debounce.h`。启动无耳机时喇叭也等待高电平稳定后开启。
若插到半途停留超过 300 ms 且检测脚保持高电平，软件仍可能判为拔出；
这种情况仅靠单根检测脚无法完全区分，需根据实测加长拔出时间或检查插座检测触点。

主机测试：

```sh
cc -std=c11 -Wall -Wextra -Werror -I main tests/test_headphone_debounce.c -o /tmp/test_headphone_debounce
/tmp/test_headphone_debounce
```

V1.2 上的两套 I2C 必须分开使用：

- 主板 I2C0：SDA=GPIO56、SCL=GPIO3。本示例不再用主板 ES8311 播放。
- 扩展板 I2C1：SDA=GPIO0、SCL=GPIO1。示例先调用 `bsp_subboard_init()`，再通过
  `bsp_subboard_get_i2c_bus()` 将同一总线共享给 A1 和 Si12T 驱动。
- A1：7-bit 地址 `0x2c`（驱动配置中的 8-bit 地址为 `0x58`）。程序先拉低 A1 的
  I2S 启动绑带、通过 GPIO10 完成上电，再在外置 I2C1 上重试探测；只有收到 A1 应答后
  才创建 I2S/TDM 和 codec 设备。未插 A1 时不会初始化 I2S，屏幕显示 `A1 / MP3 MISSING`。
- Si12T：7-bit 地址 `0x78`，100 kHz。
- ESP32-A1 板安装在右侧扩展槽，Si12T 中断连接右槽 H12，即 GPIO5。代码通过
  `bsp_subboard_map_gpio(BSP_SUBBOARD_SLOT_RIGHT, GPIO_NUM_4)` 获取该引脚。
- CO5300 UI 通过 `BSP_DISPLAY_ROTATE_270` 旋转；BSP 角度按顺时针定义，因此这等价于
  逆时针旋转 90°。

程序启动时读取 eFuse 板型，只允许 BSP 识别为 `BSP_BOARD_VARIANT_V1_2` 的硬件运行。
LCD 自带 CST9217 触摸由 BSP 初始化，并与画面一起逆时针旋转 90°；播放器可以同时使用
屏幕按钮和 Si12T 实体触摸键控制。

## NAND Flash 音乐目录

播放器不再从固件内部 SPIFFS 读取歌曲。程序通过 BSP 初始化板载 SPI NAND，将其中
已有的 LittleFS 文件系统以只读方式挂载到 `/nandflash`，并扫描 NAND 根目录下的 `/music`：

```text
/music/
├── song_01.mp3
├── song_02.mp3
└── song_03.mp3
```

NAND 初始化和逻辑页读取方式参考 `camera_photo_app`：通过 BSP 获得 NAND 句柄，再以
`spi_nand_flash_read_page()` 读取磨损均衡层提供的逻辑页。应用只执行目录扫描和文件读取。
挂载配置明确设置 `read_only=true`、`format_if_mount_failed=false`，所以
文件系统损坏或格式不兼容时程序只报错，不会格式化 NAND，也不会删除已有歌曲。MP3
按文件名（忽略大小写）排序；一首播放完成后自动切换到下一首并循环。

扫描时检查文件类型、大小和首字节可读性，跳过空文件、目录及读取失败的条目，
日志会注明文件名与原因；不删除或修改 NAND 文件。切歌前再次检查文件。
若解码结束前没有成功输出任何 PCM，则暂停而不是自动进入下一首，避免坏文件导致
“上一首后马上跳回下一首”。这不是完整的 MP3 格式校验；确认为空或损坏的文件需重新拷贝。

UI 使用 Montserrat 字体，建议使用简短 ASCII 文件名，否则中文歌名可能显示为方框。
`/music` 不存在或其中没有 MP3 时，屏幕会显示
`NO MP3 FILES`，触摸任务仍正常运行。

工程中的 [`mp3`](mp3) 文件夹仅作为 PC 端歌曲备份，不参与构建，也不会被烧录到芯片
内部 Flash。

## 按键功能

| Si12T 通道 | 功能 |
|---|---|
| TK5 | 音量降低 5% |
| TK6 | 上一首 |
| TK7 | 播放/暂停 |
| TK8 | 下一首 |
| TK9 | 音量提高 5% |

Si12T 的 TK5、TK7、TK9 使用最高灵敏度 SEN 半字节 `0x0`（`ChHL=0, ChM=000`）；
TK6 灵敏度为 `0x4`（`ChHL=0, ChM=100`），TK8 为 `0x2`（`ChHL=0, ChM=010`），均正常响应。
其他通道保持原初始化值。屏幕上的上一首、播放/暂停、下一首、
音量减和音量加按钮提供相同功能。

实体按键接受低/中/高强度输出，中断等级同步设置为 `ILC=01`，按下边沿立即执行
一次命令；不屏蔽多键，也不等待持续按下或稳定释放。
最高灵敏度更容易受到播放音频或环境噪声影响而误触发，请上板验证。
如需进一步调节，修改 `main/touch_keys.c` 中的 `TOUCH_SENSITIVITY`（其他按键）
或 `TOUCH_TRACK_SENSITIVITY`（TK6）、`TOUCH_NEXT_SENSITIVITY`（TK8）；
固定 ChHL=0 时，ChM 越大灵敏度越低。

播放器从 NAND 的 `/music` 目录流式读取文件，使用 ESP-GMF 解码 MP3，通过右侧 A1
音频子板输出。A1 使用双时隙 Philips TDM、APLL 和 `384fs` MCLK，ESP32-S31 的播放数据
由 GPIO52 输出到 A1 的 `I2S_SDIN`；GPIO40 是 A1 的 `I2S_SDOUT`，仅用于录音回传。
GPIO38 控制外置扬声器 PA，GPIO47 检测耳机：插入耳机后自动关闭 PA。播放器暂停、切歌
以及首个有效 PCM 到达前保持静音，避免喇叭空闲时持续输出杂音。UI 同步显示文件名、
播放状态和音量。

## 编译与烧录

本示例固定使用 LVGL 9.5.0，匹配 `esp_lvgl_adapter 0.6.2`。LVGL 9.6 对旧配置和
旧头文件路径产生弃用警告，会在 ESP-IDF 的 `-Werror` 构建下失败；不要单独升级 LVGL。

本示例将 ESP32-S31 的原生 USB Serial/JTAG 设置为主控制台。烧录复位后 USB 串口会重新
枚举，日志继续从该端口输出；如果设备名发生变化，请重新选择新出现的 `/dev/ttyACM*`。

```sh
cd examples/esp32a1_mp3
idf.py set-target esp32s31
idf.py build
idf.py -p PORT flash monitor
```

正常启动时日志会明确显示两条 I2C 的用途，例如：

```text
Mainboard I2C initialized: port=0 SDA=56 SCL=3
Subboard I2C initialized: port=1 SDA=0 SCL=1
A1 audio subboard detected on external I2C at 0x2c
A1 ready: TDM TX GPIO52, MCLK GPIO54, BCLK GPIO37, WS GPIO49, 384fs
ready: subboard I2C1 SDA=GPIO0 SCL=GPIO1, Si12T=0x78, IRQ=GPIO5
```
