#!/usr/bin/env python3
"""Normalize generated transparent ship art into a deterministic 4x1 atlas."""
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parent
CELL = 64
CONTENT = 58
SOURCES = (
    "player_source.png",
    "enemy_scout_source.png",
    "enemy_assault_source.png",
    "enemy_tank_source.png",
)


def normalized_sprite(path: Path) -> Image.Image:
    source = Image.open(path).convert("RGBA")
    alpha = source.getchannel("A")
    bounds = alpha.point(lambda value: 255 if value >= 32 else 0).getbbox()
    if bounds is None:
        raise ValueError(f"sprite has no visible pixels: {path}")
    source = source.crop(bounds)
    scale = min(CONTENT / source.width, CONTENT / source.height)
    size = (max(1, round(source.width * scale)), max(1, round(source.height * scale)))
    source = source.resize(size, Image.LANCZOS)
    cell = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    cell.alpha_composite(source, ((CELL - size[0]) // 2, (CELL - size[1]) // 2))
    return cell


atlas = Image.new("RGBA", (CELL * len(SOURCES), CELL), (0, 0, 0, 0))
for index, name in enumerate(SOURCES):
    atlas.alpha_composite(normalized_sprite(ROOT / name), (index * CELL, 0))
atlas.save(ROOT / "shooter_atlas.png", optimize=True)
