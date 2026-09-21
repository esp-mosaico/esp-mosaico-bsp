#include "nand_page_cache.h"
#include <assert.h>
#include <stdio.h>
static uint8_t storage[NAND_CACHE_SLOTS*32];
int main(void)
{
    nand_page_cache_t c={.data=storage,.page_size=32};
    uint8_t a[32],b[32],out[32];memset(a,0x5a,32);memset(b,0xa5,32);
    assert(!nand_page_cache_get(&c,1,out));
    nand_page_cache_put(&c,1,a);
    assert(nand_page_cache_get(&c,1,out) && !memcmp(out,a,32));
    nand_page_cache_put(&c,1+NAND_CACHE_SLOTS,b);
    assert(!nand_page_cache_get(&c,1,out));
    assert(nand_page_cache_get(&c,1+NAND_CACHE_SLOTS,out) && !memcmp(out,b,32));
    nand_page_cache_invalidate(&c,1); /* Different tag must remain valid. */
    assert(nand_page_cache_get(&c,1+NAND_CACHE_SLOTS,out));
    nand_page_cache_invalidate(&c,1+NAND_CACHE_SLOTS);
    assert(!nand_page_cache_get(&c,1+NAND_CACHE_SLOTS,out));
    c.data=NULL;nand_page_cache_put(&c,1,a);assert(!nand_page_cache_get(&c,1,out));
    puts("NAND cache tests passed: hit, collision, write invalidation, allocation fallback");
    return 0;
}
