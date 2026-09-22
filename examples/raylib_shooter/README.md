# Mosaico Raylib Shooter

Reference application for the Mosaico Game SDK. Its embedded build keeps the
familiar Raylib 2D call surface through `mosaico_raylib_fast.h`, but maps common
drawing calls directly to a 480x480 RGB565 framebuffer instead of Raylib's
generic software-OpenGL rasterizer. Four retained buffers absorb LCD/GSP
latency and are presented without an extra full-frame copy.

```bash
python mosaico.py game sim --project projects/raylib_shooter
python mosaico.py game build --project projects/raylib_shooter
python mosaico.py iris system-update --project projects/raylib_shooter
```

The Registry GSP package needs a standalone scene compiler. This workspace
discovers `../esp-gsp/ci/gspc-dev`; in a standalone clone, set
`GSPC_EXECUTABLE` to a compiler compatible with ESP-GSP 1.0.0.

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
