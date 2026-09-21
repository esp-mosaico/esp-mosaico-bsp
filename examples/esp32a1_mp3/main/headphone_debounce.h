#pragma once
#include <stdbool.h>
#include <stdint.h>
#define HP_INSERT_STABLE_MS 100U
#define HP_REMOVE_STABLE_MS 300U
typedef struct {
    bool initialized, candidate, confirmed, confirmed_valid, speaker_allowed;
    uint32_t since;
} headphone_debounce_t;
/* inserted is active-low GPIO converted to a boolean; now_ms may wrap.
 * Any insertion sample inhibits the PA immediately. Re-enabling it requires
 * an uninterrupted removal interval, even if insertion was never confirmed. */
static inline bool headphone_debounce_update(headphone_debounce_t *s,
                                              bool inserted, uint32_t now_ms)
{
    if(!s->initialized || inserted!=s->candidate) {
        s->initialized=true;s->candidate=inserted;s->since=now_ms;
    }
    if(inserted)s->speaker_allowed=false;
    uint32_t threshold=inserted?HP_INSERT_STABLE_MS:HP_REMOVE_STABLE_MS;
    if((uint32_t)(now_ms-s->since)<threshold)return false;
    bool changed=!s->confirmed_valid || s->confirmed!=inserted;
    s->confirmed=inserted;s->confirmed_valid=true;
    s->speaker_allowed=!inserted;
    return changed;
}
