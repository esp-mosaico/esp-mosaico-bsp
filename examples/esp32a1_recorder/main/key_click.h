#pragma once
#include <stdbool.h>
typedef struct { bool down; } key_click_t;
/* One action on the press edge, rearmed by release; no long-press delay. */
static inline bool key_click_update(key_click_t *s, bool valid, bool pressed)
{
    if (!valid) return false;
    bool clicked=pressed && !s->down;
    s->down=pressed;
    return clicked;
}
