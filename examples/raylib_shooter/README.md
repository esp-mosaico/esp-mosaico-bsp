# Mosaico Raylib Shooter

Reference application for the Mosaico Game SDK. Its embedded build keeps the
familiar Raylib 2D call surface through `mosaico_raylib_fast.h`, but maps common
drawing calls directly to a 480x480 RGB565 framebuffer instead of Raylib's
generic software-OpenGL rasterizer. Four retained buffers absorb LCD/GSP
latency and are presented without an extra full-frame copy.

<!-- bsp-usage:start -->
## 独立 BSP 克隆：构建与仿真

从 BSP 仓库根目录运行。固件需要已启用的 ESP-IDF >=6.2（ESP32-S31）、Git、CMake >=3.22 和 Python Pillow：

```sh
python -m pip install Pillow
idf.py --preview -C examples/raylib_shooter -DIDF_TARGET=esp32s31 build
```

首次配置会自动下载[依赖清单](../common/game_dependencies.json)指定的引擎和 utils 提交到本工程的 `build/_deps/`。
无需 vibe 工作区或相邻仓库；重复构建复用已下载的提交。仅这些游戏示例需要这两项依赖。
ESP-GSP 固定为 1.4.0，GSP 编译器由固定的产品工具自动解析。

Host 仿真不需要 ESP-IDF。从 BSP 根目录执行：

```sh
cmake -S tools/game-dependencies -B build-game-deps
python build-game-deps/_deps/mosaico_game_engine-src/host/run_game.py --project examples/raylib_shooter --headless --frames 300
```

安装使用 ESP-Mosaico 产品 CLI 和 retained Recovery，详见[游戏开发指南](../../docs/game-development.zh-CN.md#设备安装)。
<!-- bsp-usage:end -->

Touch to start and drag the ship; firing is automatic. Gameplay runs at 30 Hz
and uses fixed enemy and bullet pools. Raylib ESP 6.0.0~2 does not yet populate
standard mouse state, so device and browser touch use the Mosaico extension
queue. Event sound effects use the board ES8311/I2S path because the current
Raylib ESP backend disables `raudio`.

The fast compatibility layer currently accelerates `InitWindow`,
`BeginDrawing`/`EndDrawing`, clear, pixels, rectangles, triangles, bitmap text,
measurement and `TextFormat`. Extend that layer for additional Raylib calls;
unsupported APIs must not silently fall back to the slow `rlsw` path.

首次安装或布局、资源变化使用 `iris system-update`；分区表完全一致且仅修改代码时可用 `iris app-update`。
