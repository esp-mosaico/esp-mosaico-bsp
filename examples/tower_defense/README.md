# Circuit Keep 塔防游戏

这是 Mosaico 游戏平台的资源化验收项目，使用 480×480 RGB565 快速渲染后端，目标 30 FPS。
地图来自 Tiled `.tmj`，角色和塔来自统一 RGB565+A8 Atlas，短音效采用 PCM16，循环
背景音乐采用 IMA-ADPCM。设备和 Host 使用相同的资源文件、游戏模型和 C 像素渲染核心。

## 玩法

1. 点击开始。
2. 在底部选择 `PULSE`、`RAPID` 或 `FROST`。
3. 点击地图上的 `+` 基座建塔；选中同类塔再点已有塔可升级，最高三级。
   选中不同类型再点已有塔可将其改造为新塔，旧塔累计投入按 50% 折抵。
4. 阻止三类敌人沿道路进入右侧核心。击杀获得金币，波次结束有奖励。
5. 右上角按钮暂停或继续，核心生命归零后点击面板重新开始。

三类塔分别侧重均衡伤害、高射速和减速控制；游戏模型使用固定对象池，运行中不分配对象。

<!-- bsp-usage:start -->
## 独立 BSP 克隆：构建与仿真

从 BSP 仓库根目录运行。固件需要已启用的 ESP-IDF >=6.2（ESP32-S31）、Git、CMake >=3.22 和 Python Pillow：

```sh
python -m pip install Pillow
idf.py --preview -C examples/tower_defense -DIDF_TARGET=esp32s31 build
```

首次配置会自动下载[依赖清单](../common/game_dependencies.json)指定的引擎和 utils 提交到本工程的 `build/_deps/`。
无需 vibe 工作区或相邻仓库；重复构建复用已下载的提交。仅这些游戏示例需要这两项依赖。
ESP-GSP 固定为 1.4.0，GSP 编译器由固定的产品工具自动解析。

Host 仿真不需要 ESP-IDF。从 BSP 根目录执行：

```sh
cmake -S tools/game-dependencies -B build-game-deps
python build-game-deps/_deps/mosaico_game_engine-src/host/run_game.py --project examples/tower_defense --headless --frames 300
```

安装使用 ESP-Mosaico 产品 CLI 和 retained Recovery，详见[游戏开发指南](../../docs/game-development.zh-CN.md#设备安装)。
<!-- bsp-usage:end -->

非 headless 预览地址为 `http://127.0.0.1:8460/`；局域网预览可加
`--listen 0.0.0.0`。安装使用 Recovery system-update，先校验并写资源分区，再写应用。
通过产品 CLI 启动 Gateway 后，工作台可显示屏幕；远程输入能力以固件实际注册的服务为准。

首次安装或布局、资源变化使用 `iris system-update`；分区表完全一致且仅修改代码时可用 `iris app-update`。
