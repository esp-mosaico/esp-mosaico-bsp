#!/usr/bin/env python3
"""Trim generated sprite cells into a deterministic transparent 4x2 atlas."""
from pathlib import Path
from collections import deque
from PIL import Image

ROOT = Path(__file__).resolve().parent
source = Image.open(ROOT / "sky_hop_sprite_source.png").convert("RGBA")
pixels = source.load()
for y in range(source.height):
    for x in range(source.width):
        r, g, b, _ = pixels[x, y]
        # The generated checkerboard is connected to the outer background;
        # these neutral light pixels are not used by sprite outlines.
        if min(r, g, b) >= 205 and max(r, g, b) - min(r, g, b) <= 15:
            pixels[x, y] = (r, g, b, 0)

atlas = Image.new("RGBA", (256, 128), (0, 0, 0, 0))
cell_w, cell_h = source.width // 4, source.height // 2
for index in range(8):
    col, row = index % 4, index // 4
    cell = source.crop((col * cell_w, row * cell_h,
                        (col + 1) * cell_w, (row + 1) * cell_h))
    bounds = cell.getchannel("A").getbbox()
    if not bounds:
        raise SystemExit(f"empty sprite cell {index}")
    cell = cell.crop(bounds)
    resampling = getattr(Image, "Resampling", Image)
    cell.thumbnail((60, 60), resampling.LANCZOS)
    x = col * 64 + (64 - cell.width) // 2
    y = row * 64 + (64 - cell.height) // 2
    atlas.alpha_composite(cell, (x, y))

# The playable hero is generated as three equal columns (idle/run/jump).  Its
# source preview contains a neutral checkerboard, so remove only neutral pixels
# connected to the outer edge; enclosed highlights such as the headband remain.
hero_source = Image.open(ROOT / "portrait_hero_source.png").convert("RGBA")
rgb = hero_source.load()
visited = bytearray(hero_source.width * hero_source.height)
queue = deque()

def neutral_background(x, y):
    r, g, b, _ = rgb[x, y]
    return min(r, g, b) >= 220 and max(r, g, b) - min(r, g, b) <= 16

def enqueue(x, y):
    offset = y * hero_source.width + x
    if not visited[offset] and neutral_background(x, y):
        visited[offset] = 1
        queue.append((x, y))

for x in range(hero_source.width):
    enqueue(x, 0)
    enqueue(x, hero_source.height - 1)
for y in range(hero_source.height):
    enqueue(0, y)
    enqueue(hero_source.width - 1, y)
while queue:
    x, y = queue.popleft()
    rgb[x, y] = (255, 255, 255, 0)
    if x: enqueue(x - 1, y)
    if x + 1 < hero_source.width: enqueue(x + 1, y)
    if y: enqueue(x, y - 1)
    if y + 1 < hero_source.height: enqueue(x, y + 1)

for index in range(3):
    left = index * hero_source.width // 3
    right = (index + 1) * hero_source.width // 3
    cell = hero_source.crop((left, 0, right, hero_source.height))
    bounds = cell.getchannel("A").getbbox()
    if not bounds:
        raise SystemExit(f"empty hero cell {index}")
    cell = cell.crop(bounds)
    cell.thumbnail((60, 60), resampling.LANCZOS)
    x = index * 64 + (64 - cell.width) // 2
    y = (64 - cell.height) // 2
    atlas.paste((0, 0, 0, 0), (index * 64, 0, (index + 1) * 64, 64))
    atlas.alpha_composite(cell, (x, y))
atlas.save(ROOT / "sky_hop_atlas.png")
