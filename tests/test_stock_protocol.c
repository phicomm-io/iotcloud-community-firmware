#include "../M1/protocol.h"
#include "../A1/filter_stock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *tone_versions[]={"0.9.3","0.9.3-abcdef","0.9.10-ab12ef-d","0.10.0","1.0.0"};
    for(unsigned i=0;i<sizeof(tone_versions)/sizeof(*tone_versions);i++)assert(m1_tone_version_supported(tone_versions[i]));
    const char *old_versions[]={"","1.2","oss-0.3","0.9.2-abcdef","0.8.99","0.9.3x","0.9.3-","0..3","999999.0.0"};
    for(unsigned i=0;i<sizeof(old_versions)/sizeof(*old_versions);i++)assert(!m1_tone_version_supported(old_versions[i]));
    uint8_t frame[20]={0x23,1,0,80,1,5,50,60,25,0,35,1,7,2,0,0,0,0,0,0x21};
    m1_sample_t sample={0};
    assert(m1_parse_sample(frame,UINT32_MAX-100,&sample));
    assert(sample.temperature_deci_c==-55 && sample.humidity_deci_pct==602);
    assert(sample.pm25_status==1 && sample.auxiliary_status==7 && sample.climate_status==2);
    assert(m1_sample_fields(&sample)==15 && m1_sample_fresh(&sample,100));
    frame[11]=frame[13]=0;frame[4]=0;
    assert(m1_parse_sample(frame,100,&sample) && sample.temperature_deci_c==55);
    assert(m1_sample_fields(&sample)==8); /* Unknown flag is not labeled HCHO health. */
    assert(!m1_sample_fresh(&sample,15100));
    uint32_t seq=sample.sequence;frame[19]=0;
    assert(!m1_parse_sample(frame,200,&sample) && sample.sequence==seq);
    uint8_t tx[12];m1_buzzer_frame(tx,true);assert(tx[1]==14 && tx[2]==1 && tx[3]==0 && tx[11]==0x21);
    m1_buzzer_frame(tx,false);assert(!tx[2]);m1_version_frame(tx);assert(tx[1]==17 && !tx[2]);
    m1_threshold_frame(tx,false,0x1234,0xabcd);
    const uint8_t hcho_threshold[12]={0x23,5,0x12,0x34,0xab,0xcd,0,0,0,0,0,0x21};
    assert(!memcmp(tx,hcho_threshold,12));
    m1_threshold_frame(tx,true,0,65535);
    const uint8_t pm_threshold[12]={0x23,6,0,0,0xff,0xff,0,0,0,0,0,0x21};
    assert(!memcmp(tx,pm_threshold,12));
    assert(m1_sensor_alarm_frame(tx,42,false,0));
    const uint8_t alarm_read[12]={0x23,0x70,20,42,0,0,0,0,0,0,0,0x21};
    assert(!memcmp(tx,alarm_read,12));
    for(unsigned mask=0;mask<8;mask++)assert(m1_sensor_alarm_frame(tx,43,true,mask) && tx[4]==1 && tx[5]==mask && !tx[6] && !tx[10]);
    assert(!m1_sensor_alarm_frame(tx,1,true,8) && !m1_sensor_alarm_frame(tx,1,false,1));
    uint8_t alarm_reply[20]={0x23,0x70,20,42,0,1,3,1,0,0,0,0,0,0,0,0,0,0,0,0x21},mask=0;
    bool active=false;assert(m1_parse_sensor_alarm(alarm_reply,&mask,&active) && mask==3 && active);
    for(unsigned i=0;i<4;i++){
        unsigned at=i==0?4:i==1?5:i==2?6:7;uint8_t old=alarm_reply[at];
        alarm_reply[at]=i==0?1:i==1?2:i==2?8:2;
        assert(!m1_parse_sensor_alarm(alarm_reply,&mask,&active));alarm_reply[at]=old;
    }
    char version[17]="keep";
    memset(frame,0,sizeof(frame));frame[0]=0x23;frame[1]=17;frame[19]=0x21;frame[2]=16;
    memcpy(frame+3,"0123456789abcdef",16);
    assert(m1_parse_version(frame,version) && !strcmp(version,"0123456789abcdef"));
    frame[2]=16;memcpy(frame+3,"1.2",3);memset(frame+6,0,13);
    assert(m1_parse_version(frame,version) && !strcmp(version,"1.2"));
    memcpy(frame+3,"0123456789abcdef",16);assert(m1_parse_version(frame,version));
    frame[2]=17;assert(!m1_parse_version(frame,version) && !strcmp(version,"0123456789abcdef"));
    frame[2]=0;assert(!m1_parse_version(frame,version));
    frame[2]=1;frame[3]=0;assert(!m1_parse_version(frame,version));
    /* Decoder recovers after noise and a truncated corrupt frame. */
    m1_decoder_t decoder={0};uint8_t out[20];
    assert(!m1_decode_byte(&decoder,0xff,out));
    for(unsigned i=0;i<3;i++)assert(!m1_decode_byte(&decoder,0x23,out));
    frame[2]=3;memcpy(frame+3,"1.2",3);
    for(unsigned i=0;i<20;i++) {
        bool complete=m1_decode_byte(&decoder,frame[i],out);
        if(i==19)assert(complete && !memcmp(out,frame,20));
    }
    a1_filter_stock_t stock;a1_filter_stock_defaults(&stock,true);
    a1_filter_stock_clock_t c={0};a1_filter_stock_advance(&stock,&c,0);c.speed=100;c.weight=15;
    a1_filter_stock_advance(&stock,&c,60000);
    assert(stock.usage==97 && stock.usage_remainder==1800000 && stock.air_litres==6500);
    a1_filter_stock_advance(&stock,&c,120000);assert(stock.usage==195 && !stock.usage_remainder);
    c.weight=150;a1_filter_stock_advance(&stock,&c,180000);assert(stock.usage==1170);
    c.speed=0;a1_filter_stock_advance(&stock,&c,3600000);assert(stock.usage==1170);
    a1_filter_stock_defaults(&stock,true);c=(a1_filter_stock_clock_t){.last_ms=UINT32_MAX-499,.speed=100,.weight=75,.initialized=true};
    a1_filter_stock_advance(&stock,&c,500);assert(stock.usage==8 && stock.air_litres==108);
    stock.usage=A1_FILTER_STOCK_CAPACITY-1;stock.air_litres=UINT32_MAX-1;
    a1_filter_stock_advance(&stock,&c,3600500);
    assert(stock.usage==A1_FILTER_STOCK_CAPACITY && stock.air_litres==UINT32_MAX && !a1_filter_stock_remaining(&stock));
    puts("OEM M1 frames and A1 weighted consumption/volume/wrap/saturation: OK");
}
