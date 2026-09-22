# Sky Hop 真机性能测试

[返回文档索引](../README.md) · [游戏开发](game-development.zh-CN.md) · [Sky Hop](../examples/sky_hop/README.md)

本测试针对 Sky Hop 固定 60 秒场景，用于比较相同设备和玩法下的显示配置。
LCD 保持受支持的 40 MHz QSPI 时钟；每组结果记录 framebuffer 数、
TE compose 缓冲数和 draw-buffer 行数。

## 配置与运行

设置 `CONFIG_SKY_HOP_BENCHMARK_MODE=y`，固件会自动进入各阶段，并按固定 tick
序列驱动移动和跳跃。每组配置调整：

- `CONFIG_MOSAICO_GAME_FRAMEBUFFER_COUNT`：2、3、4。
- `CONFIG_SKY_HOP_TE_COMPOSE_BUFFERS`：1、2。
- `CONFIG_SKY_HOP_DRAWBUF_LINES`：10、34。

构建与安装通过 `mosaico.py`，按[更新方式](https://github.com/esp-mosaico/esp-mosaico-vibe/blob/main/docs/mosaico-cli.zh-CN.md)选择
`system-update` 或 `app-update`。保存覆盖完整 60 秒场景的 ESP-Iris 原始日志。

## 分析与判据

在 BSP 根目录先准备 Host 依赖，再生成比较矩阵或分析已保存的日志：

```sh
cmake -S tools/game-dependencies -B build-game-deps
python build-game-deps/_deps/mosaico_game_engine-src/tools/game_benchmark_matrix.py --output benchmark-matrix.json
python build-game-deps/_deps/mosaico_game_engine-src/tools/analyze_game_perf.py --label fb3-te1-lines34 raw.log
```

`raw.log` 替换为本次运行保存的日志路径。分析工具及其格式由引擎维护。

验收目标：逻辑更新频率 `30.0 ± 0.5 Hz`，framebuffer acquire p95 小于
1000 µs，无显示错误，显示帧率不低于 24 FPS 基线。
运行时允许 busy 或 superseded 帧，以最新游戏状态优先，避免 LCD 吞吐阻塞逻辑。

Device ID、Boot ID、固件哈希、配置和原始日志必须一起保留，结果才可比较。
这些数值是本测试的比较基线，不能代替其他应用的性能目标或当前构建的实测结果。
