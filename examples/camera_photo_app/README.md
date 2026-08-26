# Camera Photo App

基于 ESP-Mosaico 的全屏拍照示例：OV3640 实时预览、触摸 UI、SPI NAND 存图。可选启用 **USB High-Speed MSC**，在电脑上以 U 盘方式浏览照片（**默认关闭**）。

## 功能概览

| 类别 | 说明 |
|------|------|
| 实时预览 | 480×480 全屏预览，PPA 裁剪/缩放 OV3640 1024×768 YUV422 画面 |
| 拍照存储 | JPEG（质量 80，单张最大 256 KB）写入 SPI NAND，文件名 `0001.jpg`、`0002.jpg` … |
| 相册 | 全屏浏览、左右滑动切换、显示 `文件名(大小KB)`、删除确认 |
| 闪光灯 | 板载闪光灯 GPIO34；开启后拍照前后各亮 200 ms，曝光临时 ×1.5 |
| 镜头翻转 | 底部翻转按钮，预览镜像 + 左上角 `Front` / `Rear` 标签，状态写入 NVS |
| 摄像头开关 | 长按主板顶部按键（GPIO7）进入 PWDN 休眠/唤醒，关时黑屏 + 英文提示 |
| USB U 盘（可选） | `CONFIG_CAMERA_PHOTO_USB_MSC`，默认关；开启后 HS OTG 导出 `DCIM/` |
| 子板按键 | 右槽 Button LED 子板（可选）：KEY2 拍照、KEY1 切换闪光灯 |

## 硬件要求

### 必需

- **ESP-Mosaico** 主板（16 MB Flash、PSRAM、CO5300 480×480 屏）
- **Camera 子板**（OV3640）插入 **左槽**（EEPROM `0x50`）

### 可选

- **Button LED 子板** 插入 **右槽**（EEPROM `0x51`），提供物理拍照/闪光灯按键

### 接口与约束

| 项目 | 说明 |
|------|------|
| 调试串口 | UART 控制台（GPIO33 与相机 D2 共用，已禁用 USB Serial/JTAG） |
| 顶部按键 | GPIO7，**长按**切换摄像头开/关（短按无效，防误触） |
| 闪光灯 | GPIO34，低电平点亮 |
| USB 导出 | USB 2.0 HS OTG，D+ Pin 44 / D- Pin 45（需启用 `CONFIG_CAMERA_PHOTO_USB_MSC`） |
| 照片 NAND | SPI NAND，目录页与 JPEG 数据区见下方「存储说明」 |

相机子板即使 EEPROM 无有效描述符也可启动（`allow_unidentified`）。

## 工程结构

```
examples/camera_photo_app/
├── partitions.csv          # 自定义分区：app + FAT storage（USB MSC）
├── sdkconfig.defaults
└── main/
    ├── main.c              # 预览/拍照/相册主逻辑
    ├── camera_ui.c/h       # LVGL 相机与相册 UI
    ├── camera_settings.c/h # NVS：镜头翻转持久化
    ├── photo_store.c/h     # SPI NAND 照片目录与读写
    ├── photo_usb_msc.c/h   # USB MSC + NAND→FAT 同步
    └── ui_icons.c/h        # UI 图标资源
```

依赖 `esp-mosaico-bsp`、`mosaico_module_camera`、`mosaico_module_mgr`、`mosaico_module_button_led`、`esp_new_jpeg`、`esp_tinyusb` 等组件。

## 编译与烧录

```bash
cd examples/camera_photo_app
idf.py set-target esp32s31
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

> 首次使用 USB 导出或修改过分区表时，请执行完整 `flash`（含 `partition_table`）。

默认配置见 `sdkconfig.defaults`：PSRAM Oct 250 MHz、OV3640 1024×768@25fps、**USB U 盘默认关闭**（`SINGLE_APP_LARGE` 分区表）。

> **注意**：`sdkconfig.defaults` 只在**首次生成**或**删除 `sdkconfig` 后**生效。若你之前已编译过并启用过 USB，本地 `sdkconfig` 会保留旧选项，需按下方「关闭 USB U 盘」操作后再烧录。

## 快速上手

1. 左槽插入相机子板，上电后等待串口出现 `Camera photo app started` 与实时预览。
2. 点击底部 **快门** 拍照；左下角缩略图更新为最新一张。
3. 点击左下角 **相册** 进入浏览；左右滑动切换，左上角 **返回** 回到相机。
4. （可选）启用 USB U 盘后，用 USB 线连接 HS OTG 口到 PC，打开 U 盘 `DCIM` 文件夹查看照片。

## 相机界面（触摸）

```
┌─────────────────────────────────────┐
│ Front          [闪光灯] [i]         │  ← 顶栏
│                                     │
│           实时预览 / 关摄像头黑屏      │
│                                     │
│ [相册]      (快门)        [翻转]    │  ← 底栏
└─────────────────────────────────────┘
```

| 控件 | 位置 | 作用 |
|------|------|------|
| 方向标签 | 左上 | 显示 `Front` 或 `Rear`（随翻转变化） |
| 闪光灯 | 顶栏右侧 | 开关闪光灯（图标 on/off） |
| 提示 (i) | 顶栏最右 | 弹出按键与顶部长按说明 |
| 相册 | 左下 | 进入相册（显示最新缩略图） |
| 快门 | 底部中央 | 拍照 |
| 翻转 | 右下 | 切换前/后预览镜像，写入 NVS |

### 长按关闭摄像头

- **操作**：长按主板 **顶部按键（GPIO7）**
- **关闭后**：全屏黑底，英文提示
  `Camera is off.` / `Long press the top button to turn on again.`
- **再次打开**：再次长按顶部按键，预览恢复

传感器通过 PWDN（GPIO48）进入休眠，停止取流但不卸载子板。

## Button LED 子板（可选）

右槽插入 Button LED 子板后，串口会打印 `Button sub-board ready … KEY1=flash, KEY2=capture`：

| 子板按键 | 功能 |
|----------|------|
| KEY2 | 拍照（等同触摸快门） |
| KEY1 | 切换闪光灯开/关 |

未插入子板时，仅触摸 UI 与顶部按键可用；串口可能周期性打印子板发现失败，属正常现象。

## 相册模式

- **进入**：相机界面点击左下角相册按钮
- **切换**：在画面上 **左右滑动**
- **标题**：顶部显示 `0039.jpg(320KB)` 格式
- **删除**：顶栏右侧删除图标 → 确认框 → Delete
- **返回**：顶栏左侧返回图标

删除会同时更新 NAND 目录；若已启用 USB MSC，也会从 U 盘导出区删除（在 PC 未挂载 U 盘时）。删光所有照片后自动回到相机界面。

## 闪光灯行为

开启闪光灯后每次拍照：

1. 曝光临时提高至约 **1.5 倍**
2. 闪光灯 **亮 200 ms** → 抓帧 → 编码保存到 NAND（及 USB 导出区，若已启用）
3. 保存后再 **亮 200 ms** → 关灯并恢复曝光

关闭闪光灯时拍照全程不亮灯。

## USB 照片导出（U 盘，可选）

> **默认不启用。** 在 `menuconfig -> Camera Photo App` 中打开
> `Enable USB High-Speed MSC photo export (U-disk)`（`CONFIG_CAMERA_PHOTO_USB_MSC`）。

启用后还需：

1. 将 `sdkconfig.defaults.usb_msc` 中的配置合并进 `sdkconfig.defaults`（或通过 menuconfig 手动设置分区表与 TinyUSB MSC）。
2. 使用本目录下的 **`partitions.csv`**（含 FAT `storage` 分区），**删除 `sdkconfig` 后**完整重新编译烧录。

### 关闭 USB U 盘

`sdkconfig.defaults` 不会自动覆盖已有 `sdkconfig`。若烧录后 PC 仍出现 U 盘，说明本地仍是旧配置：

```bash
cd examples/camera_photo_app
rm -f sdkconfig
idf.py build
# Enter ROM download mode with the board controls, then:
idf.py -p ROM_PORT flash
```

或在 `idf.py menuconfig` 中关闭 `Enable USB High-Speed MSC photo export`，并改回 `Single factory app, large` 分区表。

确认 `sdkconfig` 中为：

- `# CONFIG_CAMERA_PHOTO_USB_MSC is not set`
- `# CONFIG_TINYUSB_MSC_ENABLED is not set`
- `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`

### 原理

- 照片主存储在 **SPI NAND**（`photo_store` 自定义目录）
- 内部 Flash 上另有 **FAT 分区**（`partitions.csv` 中 `storage`），用于 USB MSC
- 拍照/删除/插 USB 前会自动把 NAND 中的 JPEG **同步** 到 `/usb/DCIM/`

### 使用步骤

1. 在设备上正常拍照。
2. 用 USB 线连接 **HS OTG 口（Pin 44/45）** 到电脑（需支持数据传输的线缆）。
3. 电脑识别为 **可移动磁盘**，进入 **`DCIM`** 文件夹查看 `0001.jpg` 等。
4. 安全弹出或拔掉 USB 后，设备继续拍照；下次连接前会再次同步。

### 注意

- U 盘内容为设备相册的 **导出副本**；在 PC 上删除文件 **不会** 删除设备 NAND 中的照片（下次同步可能重新出现）。
- 在 PC 挂载 U 盘期间，设备仍可拍照（写入 NAND），USB 侧同步会等到 U 盘释放后再更新。
- 本示例 **未启用** BSP USB CDC 控制台，调试请用 UART。

### 分区表（16 MB Flash）

| 分区 | 类型 | 大小（约） | 用途 |
|------|------|-----------|------|
| `factory` | app | 2.5 MB | 应用程序 |
| `storage` | fat | 剩余 ~13.4 MB | USB MSC / DCIM 导出 |

## 配置项

### menuconfig → Camera Photo App

| 选项 | 默认值 | 说明 |
|------|--------|------|
| Enable USB High-Speed MSC photo export | **关闭** | 启用 U 盘导出（见上文） |
| First logical NAND page used for photo storage | 64 | 照片目录与数据区起始 NAND 页 |
| Maximum number of photos stored on NAND | 64 | 最多保存张数 |

修改 NAND 起始页会 **覆盖** 该页及之后用于照片的数据区域，请谨慎调整。

### 其他

- 镜头翻转：NVS 命名空间 `camera_app`，键 `prev_flip`
- JPEG：`JPEG_QUALITY=80`，`MAX_JPEG_SIZE=256 KB`

## 存储说明（SPI NAND）

- 第 `CONFIG_CAMERA_PHOTO_NAND_START_PAGE` 页：目录（magic `PHOT`、文件名、大小、页号）
- 之后页：JPEG 顺序追加；删除只改目录，不回收已写页
- 文件名：`%04u.jpg` 递增，与 U 盘 `DCIM` 中一致

## 常见问题

**PC 未出现 U 盘**
确认已在 menuconfig 启用 `CONFIG_CAMERA_PHOTO_USB_MSC`，并使用 `partitions.csv` 完整烧录；USB 线接 HS OTG 口（Pin 44/45）。

**预览花屏或相机掉线**
检查左槽相机子板接触；串口若报 capture 失败会自动尝试重启取流。

**关摄像头后仍看到最后一帧**
请烧录包含最新 UI 修复的固件；关摄像头时应显示黑屏与英文提示。

**右槽无 Button LED 子板**
不影响主功能；触摸 UI 与 GPIO7 长按仍可用。

## 参考

- 预览与显示：参考 `camera_lcd_preview` 思路（PPA + LVGL canvas）
- NAND 访问：参考 `nand_flash_test` / BSP `bsp_nand_flash_*`
- USB MSC：ESP-IDF `tusb_msc` / `esp_tinyusb` 组件
