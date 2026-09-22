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

## 运行

```bash
python mosaico.py game sim --project projects/tower_defense --headless
python mosaico.py game sim --project projects/tower_defense --headless \
  --replay replay.json --state-output artifacts/tower-state.json
python mosaico.py game sim --project projects/tower_defense
python mosaico.py game build --project projects/tower_defense
python mosaico.py recover  # 第一次部署 game_assets 分区时执行
python mosaico.py iris system-update --project projects/tower_defense
python mosaico.py iris logs
```

非 headless 预览地址为 `http://127.0.0.1:8460/`；局域网预览可加
`--listen 0.0.0.0`。安装使用 Recovery system-update，先校验并写资源分区，再写应用。
网页工作台和 `python mosaico.py tap X Y` 可远程操作，
`python mosaico.py screenshot` 可取得真机 RGB565 画面。

首次安装或布局、资源变化使用 `iris system-update`；分区表完全一致且仅修改代码时可用 `iris app-update`。
