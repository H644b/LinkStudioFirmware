#include "indicator.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    for (unsigned state=0;state<LED_STATE_COUNT;++state) {
        unsigned lit=0,dark=0;
        for (unsigned t=0;t<4000;t+=25) {
            LedColor c=indicator_pattern(state,t);
            assert(c.red<=64 && c.green<=64 && c.blue<=64);
            if (c.red||c.green||c.blue) ++lit; else ++dark;
        }
        assert(lit);
        if (state==LED_SEARCHING||state==LED_OFFERED||state==LED_SAVING||state==LED_COMPLETE||state==LED_ERROR)
            assert(dark);
    }
    LedColor idle=indicator_pattern(LED_IDLE,0), saving=indicator_pattern(LED_SAVING,0);
    LedColor complete=indicator_pattern(LED_COMPLETE,0), error=indicator_pattern(LED_ERROR,0);
    assert(!idle.red&&!idle.green&&idle.blue);
    assert(saving.red&&saving.green&&!saving.blue);
    assert(!complete.red&&complete.green&&!complete.blue);
    assert(error.red&&!error.green&&!error.blue);
    puts("LED colors, bounded brightness and distinguishable pulse states passed");
}
