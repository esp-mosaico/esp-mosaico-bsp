#include "headphone_debounce.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    headphone_debounce_t s={0};
    assert(!headphone_debounce_update(&s,false,0));assert(!s.speaker_allowed);
    assert(!headphone_debounce_update(&s,false,299));assert(!s.speaker_allowed);
    assert(headphone_debounce_update(&s,false,300));assert(s.speaker_allowed);
    /* Slow insertion: 200 ms high gaps must not re-enable the PA. */
    for(uint32_t t=900;t<2900;t+=400) {
        headphone_debounce_update(&s,true,t);assert(!s.speaker_allowed);
        headphone_debounce_update(&s,false,t+10);
        headphone_debounce_update(&s,false,t+210);assert(!s.speaker_allowed);
    }
    headphone_debounce_update(&s,true,3000);
    assert(!headphone_debounce_update(&s,true,3099));
    assert(headphone_debounce_update(&s,true,3100));assert(s.confirmed && !s.speaker_allowed);
    /* Every removal bounce restarts the complete 300 ms qualification. */
    headphone_debounce_update(&s,false,3200);
    headphone_debounce_update(&s,false,3499);assert(!s.speaker_allowed);
    headphone_debounce_update(&s,true,3500);
    headphone_debounce_update(&s,false,3510);
    headphone_debounce_update(&s,false,3809);assert(!s.speaker_allowed);
    assert(headphone_debounce_update(&s,false,3810));assert(s.speaker_allowed);
    /* Short insertion glitch still inhibits PA until a full stable removal. */
    headphone_debounce_update(&s,true,5000);assert(!s.speaker_allowed);
    headphone_debounce_update(&s,false,5010);
    headphone_debounce_update(&s,false,5309);assert(!s.speaker_allowed);
    headphone_debounce_update(&s,false,5310);assert(s.speaker_allowed);
    s=(headphone_debounce_t){0};
    headphone_debounce_update(&s,true,0);
    assert(headphone_debounce_update(&s,true,100));assert(s.confirmed);
    s=(headphone_debounce_t){0};
    headphone_debounce_update(&s,false,UINT32_MAX-149);
    assert(!headphone_debounce_update(&s,false,149));
    assert(headphone_debounce_update(&s,false,150));assert(s.speaker_allowed);
    puts("Headphone debounce tests passed: startup, slow insertion, removal bounce, glitch, timer wrap");
    return 0;
}
