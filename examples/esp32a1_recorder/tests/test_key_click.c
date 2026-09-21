#include "key_click.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    key_click_t s={0};
    assert(!key_click_update(&s,true,false));
    assert(key_click_update(&s,true,true));
    for(int i=0;i<100;i++)assert(!key_click_update(&s,true,true));
    assert(!key_click_update(&s,false,false));
    assert(!key_click_update(&s,true,true));
    assert(!key_click_update(&s,true,false));
    assert(key_click_update(&s,true,true));
    key_click_t tk5={0},tk9={0};
    assert(key_click_update(&tk5,true,true));
    assert(key_click_update(&tk9,true,true));
    assert(!key_click_update(&tk5,true,true));
    assert(!key_click_update(&tk9,true,true));
    assert(!key_click_update(&tk5,true,false));
    assert(key_click_update(&tk5,true,true));
    assert(!key_click_update(&tk9,true,true));
    puts("TK5/TK7/TK9 click tests passed");return 0;
}
