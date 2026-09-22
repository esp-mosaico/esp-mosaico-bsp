# 游戏开发：从 Host 仿真到真机

[返回 BSP](../README.md)

本工作区通过固定版本的 [Raylib Lite Engine](../../raylib-lite-engine/README.md)
提供 RGB565 游戏运行时、资源管线和 Host 仿真。Host 与设备编译同一份玩法和
绘制代码，PC 预览适合验证状态、输入和像素结果；LCD 时序、触摸手感与音频仍需
真机验收。GSP UI 应用使用独立的 [GSP 仿真工具](../../esp-mosaico-utils/mosaico-tools/tools/gsp-sim/README.md)。

## 选择参考项目

| 项目 | 适合参考的内容 |
| --- | --- |
| [Raylib Shooter](../examples/raylib_shooter/README.md) | 小型射击玩法与共享绘制 |
| [Tower Defense](../examples/tower_defense/README.md) | Atlas、Tiled 地图、音频和 Host 回放 |
| [Sky Hop](../examples/sky_hop/README.md) | 平台物理、滚动视图、关卡与性能对比 |

在 vibe 工作区运行 `python mosaico.py game create sky_hop --template sky-hop`，
从 BSP 示例生成 `projects/sky_hop/`；其他模板为 `shooter` 和 `tower-defense`。保留参考应用的 Recovery 契约及共享 CMake 集成，
使用 [Hello World](../../esp-mosaico-utils/mosaico-tools/templates/hello_world/README.md)核对普通应用约束。
实现新能力前，查看引擎的[组件职责与生命周期](../../raylib-lite-engine/components/README.md)。

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
[工作区引擎集成](../../esp-mosaico-utils/mosaico-tools/cmake/raylib_lite_engine.cmake)。
兼容 API 以引擎的 [mosaico_raylib_fast.h](../../raylib-lite-engine/components/mosaico_raylib_fast/include/mosaico_raylib_fast.h)
为准，不把完整桌面 Raylib 的能力视为设备已支持的能力。

## 运行仿真

在工作区根目录初始化游戏引擎，使用提供 `cc`、`gcc` 或 `clang` 的主机环境；
Host runner 还需要 Pillow：

```sh
git submodule update --init submodule/raylib-lite-engine
python -m pip install Pillow
python mosaico.py game sim projects/raylib_shooter
```

模拟器默认打开 `http://127.0.0.1:8460/`，支持输入、暂停、单步、变速、重置、
截图和录制。渲染由原生 C 代码完成，浏览器显示其 RGB565 帧。
`game sim` 不启动 ESP-GSP，也不需要设备连接。

无界面检查示例：

```sh
python mosaico.py game sim projects/sky_hop --headless --frames 300
python mosaico.py game sim projects/tower_defense --headless --frames 300
python mosaico.py game sim projects/raylib_shooter --headless --scenario my_replay.json --state-output artifacts/state.json
```

`--scenario` 指向已有回放文件，可从浏览器录制后下载；仓库不预置
`my_replay.json`。Host ABI 以引擎的
[公开头文件](../../raylib-lite-engine/host/include/mosaico_host_game.h)为准；
HTTP 接口和事件格式参见 [Host runner](../../raylib-lite-engine/host/run_game.py)
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

## 构建与真机验证

构建前按[工程初始化指南](https://github.com/esp-mosaico/esp-mosaico-vibe/blob/main/docs/project-init.zh-CN.md)准备应用依赖与
兼容 ESP-IDF。以 Sky Hop 为例：

```sh
python mosaico.py game build projects/sky_hop
python mosaico.py recover
python mosaico.py iris system-update --project projects/sky_hop
python mosaico.py iris logs --project projects/sky_hop --timeout 20
```

其中 `recover` 用于空白或未经验证的设备。首次安装，或分区布局、外部资源变化，
使用 `system-update`；后续代码更新且完整分区表一致时使用 `app-update`。
更新前保存有效 core dump，更新后核对同一 Device ID、新 Boot ID、目标固件及
healthy 状态。完整规则见 [CLI 命令参考](https://github.com/esp-mosaico/esp-mosaico-vibe/blob/main/docs/mosaico-cli.zh-CN.md)。

先在 Host 验证输入、状态和绘制，再通过 ESP-Iris 日志、屏幕截图和实际操作验证
LCD 显示、双触点、IMU 与音频。需要持续观察时打开
[Gateway Web 工作台](https://github.com/esp-mosaico/esp-mosaico-vibe/blob/main/docs/project-gateway.zh-CN.md)。
Sky Hop 的固定场景和性能矩阵见 [Sky Hop 性能测试](sky-hop-performance.zh-CN.md)。
