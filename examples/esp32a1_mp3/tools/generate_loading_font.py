"""Print an LVGL 9 font subset for the startup page (Pillow required).

Usage: python3 tools/generate_loading_font.py /path/to/NotoSansCJK-Regular.ttc
Source font is distributed under SIL Open Font License 1.1.
"""
import sys
from PIL import Image, ImageDraw, ImageFont

font = ImageFont.truetype(sys.argv[1], 24)
chars = sorted(set("资源加载中失败"), key=ord)
bitmap = []
descriptors = ["    {0},"]
for char in chars:
    left, top, right, bottom = font.getbbox(char, anchor="ls")
    width, height = right-left, bottom-top
    canvas = Image.new("L", (width, height))
    ImageDraw.Draw(canvas).text((-left, -top), char, font=font, fill=255, anchor="ls")
    pixels = list(canvas.getdata())
    offset = len(bitmap)
    for i in range(0, len(pixels), 2):
        bitmap.append(((pixels[i] >> 4) << 4) | (pixels[i+1] >> 4 if i+1 < len(pixels) else 0))
    descriptors.append(f"    {{.bitmap_index={offset}, .adv_w={round(font.getlength(char)*16)}, "
                       f".box_w={width}, .box_h={height}, .ofs_x={left}, .ofs_y={-bottom}}}, /* {char} */")
print('/* Generated from Noto Sans CJK; see LOADING_FONT_LICENSE.txt. */\n#include "lvgl.h"')
print("static const uint8_t glyph_bitmap[] = {")
for i in range(0, len(bitmap), 16):
    print("    " + ",".join(f"0x{x:02x}" for x in bitmap[i:i+16]) + ",")
print("};\nstatic const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
print("\n".join(descriptors))
print("};\nstatic const uint16_t unicode_list[] = {" + ",".join(str(ord(c)-ord(chars[0])) for c in chars) + "};")
print("static const lv_font_fmt_txt_cmap_t cmaps[] = {{"
      f".range_start={ord(chars[0])}, .range_length={ord(chars[-1])-ord(chars[0])+1}, "
      f".glyph_id_start=1, .unicode_list=unicode_list, .list_length={len(chars)}, "
      ".type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}};")
print("static const lv_font_fmt_txt_dsc_t font_dsc = {.glyph_bitmap=glyph_bitmap, "
      ".glyph_dsc=glyph_dsc, .cmaps=cmaps, .cmap_num=1, .bpp=4};")
print("const lv_font_t loading_font_24 = {.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt, "
      ".get_glyph_bitmap=lv_font_get_bitmap_fmt_txt, .line_height=32, .base_line=7, .dsc=&font_dsc};")
