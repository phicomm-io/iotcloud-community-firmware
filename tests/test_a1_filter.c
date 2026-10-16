#include <assert.h>
#include <stdio.h>
#include "../A1/filter.h"

int main(void)
{
    a1_filter_storage_t f;
    a1_filter_clock_t c={0};
    a1_filter_defaults(&f);
    assert(f.lifetime_hours==4320 && a1_filter_remaining(&f)==100);
    a1_filter_advance(&f,&c,5000,false);
    a1_filter_advance(&f,&c,10000,true);
    assert(f.used_seconds==0);
    a1_filter_advance(&f,&c,10500,false);
    a1_filter_advance(&f,&c,11000,true);
    a1_filter_advance(&f,&c,11500,false);
    assert(f.used_seconds==1 && c.remainder_ms==0);
    a1_filter_advance(&f,&c,100000,false);
    assert(f.used_seconds==1);
    c=(a1_filter_clock_t){0};
    a1_filter_advance(&f,&c,UINT32_MAX-499,true);
    a1_filter_advance(&f,&c,500,false);
    assert(f.used_seconds==2);
    f.used_seconds=4320*3600*9/10;
    assert(a1_filter_remaining(&f)==10);
    f.used_seconds=4320*3600-1;
    assert(a1_filter_remaining(&f)==1);
    f.used_seconds++;
    assert(a1_filter_remaining(&f)==0);
    f.used_seconds=INT_MAX-1;
    c=(a1_filter_clock_t){0};
    a1_filter_advance(&f,&c,0,true);
    a1_filter_advance(&f,&c,5000,true);
    assert(f.used_seconds==INT_MAX && a1_filter_remaining(&f)==0);
    puts("A1 filter elapsed time, pause/resume, tick wrap, warning, exhaustion, saturation: OK");
}
