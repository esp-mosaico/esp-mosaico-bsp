# Game drawing inventory

| Example | Shared view | Assets |
| --- | --- | --- |
| Sky Hop | examples/sky_hop/main/sky_hop_view.c | Atlas sprites and generated sound cues |
| Tower Defense | examples/tower_defense/main/tower_view.c | Atlas, Tiled map and audio |
| Raylib Shooter | examples/raylib_shooter/main/shooter_view.c | Shared RGB565 primitives and sprites |

Each game.sim.json selects the same C game/view sources used by the firmware.
Engine rendering and asset APIs are maintained by Raylib Lite Engine.
Source assets and existing license notices were transferred from
esp-mosaico-vibe at ce37d02 without replacing their provenance.
