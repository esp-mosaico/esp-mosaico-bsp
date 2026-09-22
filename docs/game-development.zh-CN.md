# 游戏开发：从 Host 仿真到真机

[返回 BSP](../README.md)

BSP 的三个完整游戏示例可以在单独克隆本仓库后构建和仿真。
[依赖清单](../examples/common/game_dependencies.json)声明引擎与 utils 的固定 Git 提交。
固件配置通过 CMake FetchContent 下载到本工程 `build/_deps/`；Host 依赖下载到
`build-game-deps/_deps/`。不会搜索相邻目录，也不要求预先克隆 vibe。
这些依赖只用于游戏示例，不影响 BSP 组件和其他外设示例。
当前引擎修正尚在上游 PR 中，清单使用发布该提交的 fork URL 和完整提交号；不依赖浮动分支或未声明的本地文件。

构建需要 ESP-IDF >=6.2（ESP32-S31）、Git、CMake >=3.22 和 Pillow。
Host 需要 C 编译器、CMake、Git 和 Pillow，不需要 ESP-IDF。
显式指定 `MOSAICO_UTILS_ROOT`、`RAYLIB_LITE_ENGINE_ROOT` 可以使用离线 checkout；
路径无效时立即报错，不回退到另一份源码。工作区生成器会显式提供这些路径。

## 选择参考项目

| 项目 | 适合参考的内容 |
| --- | --- |
| [Raylib Shooter](../examples/raylib_shooter/README.md) | 小型射击玩法与共享绘制 |
| [Tower Defense](../examples/tower_defense/README.md) | Atlas、Tiled 地图、音频和 Host 回放 |
| [Sky Hop](../examples/sky_hop/README.md) | 平台物理、滚动视图、关卡与性能对比 |

在 vibe 工作区运行 `python mosaico.py game create sky_hop --template sky-hop`，
从 BSP 示例生成 `projects/sky_hop/`；其他模板为 `shooter` 和 `tower-defense`。保留参考应用的 Recovery 契约及共享 CMake 集成，
使用 [Hello World](https://github.com/esp-mosaico/esp-mosaico-utils/blob/39d0e116381b42b05e072afd2e27f304a0246803/mosaico-tools/templates/hello_world/README.md)核对普通应用约束。
实现新能力前，查看引擎的[组件职责与生命周期](https://github.com/espressif2022/raylib-lite-engine/blob/76226b741590116ee1e40e0f9fb6d78f8a3c9e7c/components/README.md)。

## 组织同源代码

- 玩法模型使用可由主机 C 编译器编译的 C 源码，避免依赖 ESP-IDF、BSP 或 FreeRTOS。
- `<game>_view.c` 同时进入设备构建和 Host 清单，以相同的 Raylib 兼容调用绘制。
- `game_module.c` 只负责 Host 生命周期与输入映射。
- 设备 `main.c` 调用 `mosaico_game_app_run()`；资源、触区、音频和玩法回调放在
  项目自己的设备适配文件中。

项目根目录通过 `game.sim.json` 声明 Host 源码，例如：

```json
{
  "schema": "mosaico-game-sim/v1",
  "sources": ["main/game_module.c", "main/game.c", "main/game_view.c"]
}
```

文件名按实际项目调整。CMake 能力选择参考现有工程及
[工作区引擎集成](https://github.com/esp-mosaico/esp-mosaico-utils/blob/39d0e116381b42b05e072afd2e27f304a0246803/mosaico-tools/cmake/raylib_lite_engine.cmake)。
兼容 API 以引擎的 [mosaico_raylib_fast.h](https://github.com/espressif2022/raylib-lite-engine/blob/76226b741590116ee1e40e0f9fb6d78f8a3c9e7c/components/mosaico_raylib_fast/include/mosaico_raylib_fast.h)
为准，不把完整桌面 Raylib 的能力视为设备已支持的能力。

## 运行仿真

以下命令从单独克隆的 BSP 根目录执行：

```sh
python -m pip install Pillow
cmake -S tools/game-dependencies -B build-game-deps
python build-game-deps/_deps/mosaico_game_engine-src/host/run_game.py --project examples/raylib_shooter
```

模拟器默认打开 `http://127.0.0.1:8460/`，支持输入、暂停、单步、变速、重置、
截图和录制。渲染由原生 C 代码完成，浏览器显示其 RGB565 帧。
Host 不启动 ESP-GSP，也不需要设备连接。

无界面检查示例：

```sh
python build-game-deps/_deps/mosaico_game_engine-src/host/run_game.py --project examples/sky_hop --headless --frames 300
python build-game-deps/_deps/mosaico_game_engine-src/host/run_game.py --project examples/tower_defense --headless --frames 300
python build-game-deps/_deps/mosaico_game_engine-src/host/run_game.py --project examples/raylib_shooter --headless --replay my_replay.json --state-output artifacts/state.json
```

`--replay` 指向已有回放文件，可从浏览器录制后下载；仓库不预置
`my_replay.json`。Host ABI 以引擎的
[公开头文件](https://github.com/espressif2022/raylib-lite-engine/blob/76226b741590116ee1e40e0f9fb6d78f8a3c9e7c/host/include/mosaico_host_game.h)为准；
HTTP 接口和事件格式参见 [Host runner](https://github.com/espressif2022/raylib-lite-engine/blob/76226b741590116ee1e40e0f9fb6d78f8a3c9e7c/host/run_game.py)
中的 `Handler` 与 `load_replay()`。回放事件使用非负、递增或相同的 `frame` 序号。

例如，将下列内容保存为 `my_replay.json` 后执行上述回放命令：

```json
{
  "events": [
    {"frame": 0, "type": "action", "code": "restart", "pressed": true},
    {"frame": 1, "type": "action", "code": "restart", "pressed": false},
    {"frame": 2, "type": "action", "code": "right", "pressed": true},
    {"frame": 20, "type": "action", "code": "right", "pressed": false}
  ]
}
```

## 构建

激活兼容 ESP-IDF 后，从 BSP 根目录执行（其他游戏替换目录名）：

```sh
idf.py --preview -C examples/sky_hop -DIDF_TARGET=esp32s31 build
```

CMake 首次配置自动获取清单中的精确依赖。应用的 Recovery 分区、UI 和游戏资源
布局不变；直接 IDF flash/app-flash 目标保留禁止写入 Recovery 的保护。

## 设备安装

设备部署使用 ESP-Mosaico 产品 CLI。独立 BSP 构建和 Host 仿真无需该工作区；
需要安装时，按 [vibe 工作区指南](https://github.com/esp-mosaico/esp-mosaico-vibe/blob/main/docs/project-init.zh-CN.md)
准备工作区，再通过 `python mosaico.py game create sky_hop --template sky-hop` 生成对应应用。
从工作区根目录运行：

```sh
python mosaico.py recover  # 空白或未验证设备首次安装前
python mosaico.py iris system-update --project projects/sky_hop
python mosaico.py iris logs --project projects/sky_hop --timeout 20
```

首次安装或分区、资源变化使用 system-update；仅代码更新且完整分区表一致时使用 app-update。
更新前保存有效 core dump，更新后核对同一 Device ID、新 Boot ID、目标固件及 healthy 状态。
实际 LCD、触摸、IMU 和音频需要真机验证。
Sky Hop 的固定场景和性能矩阵见 [Sky Hop 性能测试](sky-hop-performance.zh-CN.md)。
