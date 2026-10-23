#include "../A1/fresh_air_model.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    fresh_mapping_t m={.pulses_per_rev=2,.cap_ratio=100,.boost_ratio=100,.a=1,.pwm_max=100};
    assert(fresh_mapping_valid(&m));
    m.coupling=2;unsigned ip,iff;fresh_pair(&m,0,100,0,0,100,&ip,&iff);assert(ip==0 && iff==100);
    m.coupling=0;
    m.pwm_min=20;m.pwm_max=80;m.a=.5;m.b=10;
    assert(fresh_duty(&m,0)==0 && fresh_duty(&m,1)==20 && fresh_duty(&m,100)==60);
    m.active_low=1;m.a=-1;m.b=100;
    assert(fresh_duty(&m,0)==100 && fresh_duty(&m,1)==80 && fresh_duty(&m,100)==20);
    m.a=NAN;assert(!fresh_mapping_valid(&m));m.a=INFINITY;assert(!fresh_mapping_valid(&m));m.a=1;
    m.pwm_min=81;assert(!fresh_mapping_valid(&m));m.pwm_min=0;
    /* Every ratio, requested speed, and changing cap. Start at an arbitrary
     * previous pair: a new configuration must constrain the VERY NEXT step. */
    for(unsigned mode=0;mode<2;mode++) {
        m.coupling=mode;
        for(unsigned ratio=mode?1:0;ratio<=(mode?500:100);ratio++) {
            m.cap_ratio=mode?100:ratio;m.boost_ratio=mode?ratio:100;
            for(unsigned f=0;f<=100;f++) for(unsigned p=0;p<=100;p+=5) {
                unsigned np,nf;
                for(unsigned limit=0;limit<=100;limit+=10) {
                    fresh_pair(&m,p,f,limit,100,100,&np,&nf);
                    assert(np<=limit && nf<=100);
                    assert(mode?nf*ratio<=np*100:nf*100<=np*ratio);
                }
                unsigned op=0,of=0;
                for(unsigned tick=0;tick<12;tick++) {
                    fresh_pair(&m,p,f,100,op,of,&np,&nf);
                    assert(np<=op+10 && nf<=of+10);
                    assert(mode?nf*ratio<=np*100:nf*100<=np*ratio);
                    op=np;of=nf;
                }
                unsigned expected_p=mode?(f*ratio+99)/100:p;
                if(expected_p<p)expected_p=p;if(expected_p>100)expected_p=100;
                unsigned expected_f=mode?expected_p*100/ratio:expected_p*ratio/100;
                if(expected_f>f)expected_f=f;
                assert(np==expected_p && nf==expected_f);
            }
        }
    }
    puts("Fresh PWM mapping and every coupling ratio / ramp / cap: PASS");
}
