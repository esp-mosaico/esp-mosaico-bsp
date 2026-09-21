#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define NAND_CACHE_SLOTS 128U
typedef struct {
    uint8_t *data;
    uint32_t page_size;
    uint32_t tags[NAND_CACHE_SLOTS];
    bool valid[NAND_CACHE_SLOTS];
} nand_page_cache_t;
static inline bool nand_page_cache_get(nand_page_cache_t *c, uint32_t page, void *out)
{
    unsigned slot=page%NAND_CACHE_SLOTS;
    if(!c->data || !c->valid[slot] || c->tags[slot]!=page)return false;
    memcpy(out,c->data+(size_t)slot*c->page_size,c->page_size);
    return true;
}
static inline void nand_page_cache_invalidate(nand_page_cache_t *c, uint32_t page)
{
    unsigned slot=page%NAND_CACHE_SLOTS;
    if(c->tags[slot]==page)c->valid[slot]=false;
}
static inline void nand_page_cache_put(nand_page_cache_t *c, uint32_t page, const void *data)
{
    if(!c->data)return;
    unsigned slot=page%NAND_CACHE_SLOTS;
    memcpy(c->data+(size_t)slot*c->page_size,data,c->page_size);
    c->tags[slot]=page;c->valid[slot]=true;
}
