# Sky Hop

Sky Hop is a four-level scrolling platform game. Level data is kept in the
host-testable gameplay model: progress, score, and remaining lives carry into
the next level, while completing level 4 finishes the run.

原创横版平台跳跃 Demo，用于验证 Mosaico Raylib Game SDK。

## 操作

- 点击标题卡开始。
- 屏幕底部左侧 `<` 向左移动，中间 `>` 向右移动，右侧 `JUMP` 跳跃。
- 点击右上角暂停按钮暂停/继续；Button、Joystick 和 IMU 事件会映射到相同 Action。
- 收集金币、踩掉紫色巡逻怪，并抵达关卡最右侧。

本版本使用平台 `Camera2D` 世界坐标渲染，加入场景滑入 Tween、固定容量粒子、
暂停场景与 NVS 最高分存档。游戏更新模型仍可脱离 ESP-IDF 在 Host 上测试。

当前同时嵌入一份只读 Atlas/音频作为资源分区不可用时的恢复兜底。正常情况下仍
优先读取 `game_assets` 分区；修复 Recovery system-update 通道后应取消整包嵌入，
以恢复约 300 KiB 应用空间。

<!-- bsp-usage:start -->
## 独立 BSP 克隆：构建与仿真

从 BSP 仓库根目录运行。固件需要已启用的 ESP-IDF >=6.2（ESP32-S31）、Git、CMake >=3.22 和 Python Pillow：

```sh
python -m pip install Pillow
idf.py --preview -C examples/sky_hop -DIDF_TARGET=esp32s31 build
```

首次配置会自动下载[依赖清单](../common/game_dependencies.json)指定的引擎和 utils 提交到本工程的 `build/_deps/`。
无需 vibe 工作区或相邻仓库；重复构建复用已下载的提交。仅这些游戏示例需要这两项依赖。
ESP-GSP 固定为 1.5.1，GSP 编译器由固定的产品工具自动解析。

Host 仿真不需要 ESP-IDF。从 BSP 根目录执行：

```sh
cmake -S tools/game-dependencies -B build-game-deps
python build-game-deps/_deps/mosaico_game_engine-src/host/run_game.py --project examples/sky_hop --headless --frames 300
```

安装使用 ESP-Mosaico 产品 CLI 和 retained Recovery，详见[游戏开发指南](../../docs/game-development.zh-CN.md#设备安装)。
<!-- bsp-usage:end -->

浏览器模拟器地址为 `http://127.0.0.1:8460/`。键盘使用 `A/D` 或方向键移动、
空格跳跃、`P` 暂停、回车开始/进入下一关；触屏设备可同时按住底部移动键和跳跃键。
模拟器直接编译并调用设备相同的 `platform_game.c`，所以关卡、碰撞、分数和状态切换
不需要在网页端重复实现。Host 与设备共享 RGB565 view，浏览器直接显示 C 渲染结果；音频、LCD 时序和
物理输入仍需真机验证。

应用保留 factory Recovery，并通过 `iris_ota_support_start()` 暴露进入 Recovery 的 RPC。

首次安装或布局、资源变化使用 `iris system-update`；分区表完全一致且仅修改代码时可用 `iris app-update`。

开发与回放流程见[游戏开发指南](../../docs/game-development.zh-CN.md)，
固定 60 秒场景、配置矩阵与判据见[Sky Hop 性能测试](../../docs/sky-hop-performance.zh-CN.md)。
