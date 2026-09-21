# ESP32-A1 录音机（ESP-Mosaico V1.2）

参考 `../a1_record_playback`，通过 A1 音频子板录音、播放录音；不使用主板 ES8311。
屏幕使用 LVGL，逆时针旋转 90°，支持触屏操作。

## 使用

- 单击 **TK7** 开始录音；松开后，再次单击结束并保存。持续按住只触发一次。
- 屏幕 `Record` / `Finish recording` 同样可以开始、结束录音。
- 录音保存后自动加入列表，点击文件右侧的独立 `Play` 按钮播放；`Stop` 停止播放。
- 未缓存的文件播放先进入 `BUFFERING`，显示加载百分比。完整 PCM 读入 PSRAM 并关闭 NAND 文件后，
  才由独立高优先级任务从内存向 A1 供数；播放过程中不读取 NAND。
- 加载时 `Stop` 取消缓存（当前 NAND 读取返回后生效），其他操作暂时禁用。
  读取失败或 PSRAM 不足时提示错误，不回退到边读边播。短录音复用 8 MB 录音缓存，
  较长录音需要额外足够大的连续 PSRAM，最大可播放时长取决于可用内存。
- 最近一次播放的完整缓存保留到切换文件、开始新录音或删除对应文件，重复播放不再加载。
  新录音可额外申请 8 MB 内存镜像：分配成功且录音不超过约 29 秒时，保存后直接播放；
  申请失败或超过镜像容量不影响保存，但首次播放仍需读取 NAND。
- TK5 降低播放音量，TK9 增加播放音量，每次 5%，范围 0–100%，屏幕实时显示。默认 60%，重启后恢复默认值。
- `Play` 右侧的 `Del` 按钮弹出文件名及确认提示；点击 `Delete` 才永久删除，`Cancel` 不删除。
- 删除前会停止播放，成功后刷新列表。录音、保存期间禁止删除；删除不可撤销，请先备份重要录音。
- 保存期间显示 `SAVING` 并禁用操作，过期的排队指令不会继续启动新录音。
- 录音过程中不能播放列表文件。播放过程中开始录音会先停止播放。
- Si12T 使用 BSP 外置 I2C1（GPIO0/1），TK5–TK9 灵敏度均为 `0x4`；TK6、TK8 暂无功能。

## NAND 文件

使用 NAND 上**已有的 LittleFS**，挂载到 `/nandflash`，自动新建 `/nandflash/recordings`。
不会自动格式化，不删除已有 `music` 或其他文件。不兼容直接覆盖整个 NAND 的其他存储布局。
挂载失败时会在屏幕报错，此时请先确认 NAND 文件系统，勿直接擦除 NAND。
挂载前只读校验 LittleFS 超级块，沿用盘上原有块数（而非直接使用当前 Dhara 容量）。
允许当前设备容量大于文件系统容量，但不自动扩容；文件系统容量大于设备容量时拒绝挂载。
启动时串行完成 GPIO60 电源上升，再启动录音后台和屏幕，避免共享电源初始化竞争。

启动不再调用已用空间统计（`lfs_fs_size` 会遍历全文件系统），只打印总容量。
增加 128 个逻辑页的只读缓存（2048 字节页时占 256 KB PSRAM），写入前失效对应页；
分配失败时仍可无缓存运行。NAND 不超过 32 字节的命令/状态/元数据事务采用 polling，
整页传输仍使用原中断/DMA 方式，SPI 时钟保持原值，不通过超频提速。
日志分别报告 NAND 初始化、超级块探测、挂载、录音扫描和 A1 校准时间，
以及 `NAND preload: ... ms, ... KiB/s`，用于区分存储延迟与音频芯片校准时间。

新录音为 `rec_000001.pcm` + `rec_000001.meta`，扫描已有编号，不覆盖已有录音。
音频仍为 **48 kHz / 24-bit PCM / FL、FR 双声道**，约 17.28 MB/分钟，旧 WAV 继续兼容。
`.meta` 是 44 字节的标准 WAV 头；`.pcm` 是原始交织音频。复制到电脑后，可以将两者
按“meta 在前、pcm 在后”连接为标准 WAV（例如 `cat rec_000001.meta rec_000001.pcm > exported.wav`）。
备份时务必保留配套的两个文件；屏幕确认删除会删除音频及对应元数据。
A1 使用原参考示例的 5 时隙 TDM，保存前两路，另外三路不写入文件。
列表最多显示 128 个符合此格式的录音文件；达到上限后请先备份并移走旧录音。

录音中顺序写入 `.pcm.part`，停止后排空缓存并同步，然后单独写入小型 `.meta.part`。
先发布 `.pcm`，最后发布 `.meta` 作为完成标记，不再回填文件头、复制整段音频。
保存、删除后只更新内存列表，不重扫其他录音；LittleFS 分配位图扩大至 8192 字节以减少分配扫描。
保存仍需等待积压写入和 NAND 同步，不能保证任何长度、任何 NAND 状态都零等待。
断电或失败时保留未完成的数据；缺少完整 `.meta` 的 PCM 不加入列表，需要另行恢复。
启动仍可恢复旧版本头部完整、长度匹配的 WAV `.part`，不自动修复新格式的未完成提交。
NAND 写入过慢、容量不足或音频读取失败时停止录音，并尽可能保存已写入的部分。
8 MB PSRAM 环形缓存可缓冲约 29 秒的 NAND 写入停顿（并非录音时长上限）。
计时由实际采集的 PCM 数据量更新，不再等待 NAND 写入；点击停止立即停止采集，
屏幕显示保存状态，后台排空缓存。缓存溢出或 ADC 错误停止采集时仍保存缓存中的数据；
真正的文件写入错误则报告部分录音，零数据文件不会发布为成功录音。
持续写入速度低于 288 KB/s 或超长阻塞仍可能使缓存耗尽，需按日志检查存储。
Dhara 逻辑页可覆盖写入，不再通过写整页 `0xff` 模拟擦除，减少重复写入和回收开销。
本工程在构建目录生成 NAND 驱动适配副本，将 BUSY 超时下限设为 20 ms，
并在超时前再次读取状态；这不是固定等待 20 ms，不修改托管组件源文件。
若依赖中的等待逻辑变化，构建会提示重新审查该适配，避免静默套用错误补丁。
录音/待机关闭功放，避免录音回授；播放音量默认 60%，ADC 增益固定 10 dB（不随播放音量变化）。

## 接线与初始化

使用右侧音频子板引脚：电源 GPIO10、PA GPIO38、耳机检测 GPIO47；
I2S MCLK=54、BCLK=37、WS=49、主板输出到 A1 SDIN=52、主板输入来自 A1 SDOUT=40。
A1 上电后先在外置 I2C 上探测 `0x2c`，检测到后才初始化 I2S 和 A1 ADC/DAC。
启动时未检测到子板需检查连接后重启，本示例不实现热插拔。

## 编译烧录

```sh
source /home/lianghao/esp/esp_idf_master/esp-idf/export.sh
cd /home/lianghao/esp/esp_mosaico/esp-mosaico-bsp/examples/esp32a1_recorder
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

使用当前支持 ESP32-S31 的 ESP-IDF。A1 和 esp_codec_dev 依赖与参考工程一致，
需要 Espressif 私有 GitLab 的访问权限；LVGL 固定 9.5.0。
USB Serial/JTAG 为默认日志口；具体串口名以本机枚举结果为准。
不需要打包、烧写 NAND 文件系统镜像。

主机 WAV 文件格式测试：

```sh
cc -std=c11 -Wall -Wextra -Werror -I main tests/test_wav.c main/wav_file.c -o /tmp/recorder_test_wav
/tmp/recorder_test_wav
cc -std=c11 -Wall -Wextra -Werror -I main tests/test_key_click.c -o /tmp/recorder_test_key
/tmp/recorder_test_key
cc -std=c11 -Wall -Wextra -Werror -I main tests/test_pcm_cache.c main/pcm_cache.c -o /tmp/recorder_test_pcm_cache
/tmp/recorder_test_pcm_cache
cc -std=c11 -Wall -Wextra -Werror -I main tests/test_nand_page_cache.c -o /tmp/recorder_test_nand_cache
/tmp/recorder_test_nand_cache
cc -std=c11 -Wall -Wextra -Werror -I main tests/test_recording_storage.c main/recording_storage.c main/wav_file.c -o /tmp/recorder_test_storage
/tmp/recorder_test_storage
```

LittleFS 容量兼容及只读探测测试（先完成组件下载）：

```sh
cc -std=c11 -Wall -Wextra -Werror -I main \
  -I managed_components/joltwallet__littlefs/src/littlefs \
  tests/test_nand_lfs_probe.c main/nand_lfs_probe.c \
  managed_components/joltwallet__littlefs/src/littlefs/lfs.c \
  managed_components/joltwallet__littlefs/src/littlefs/lfs_util.c \
  -o /tmp/recorder_test_geometry
/tmp/recorder_test_geometry
```
