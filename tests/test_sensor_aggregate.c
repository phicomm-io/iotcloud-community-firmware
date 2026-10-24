#include "phicomm_features.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void){
 feature_sample_t s[3]={0};float out[4];
 for(unsigned i=0;i<3;i++){s[i].partial=true;s[i].valid=true;s[i].received_ms=100;s[i].fields=4;s[i].value[2]=10+10*i;}
 s[1].fields|=8;s[1].value[3]=.123f;
 assert(feature_aggregate(s,3,101,STAT_MEAN,out,NULL)==3);
 assert(out[2]==20&&fabsf(out[3]-.123f)<.00001f&&isnan(out[0]));
 s[1].valid=false;
 assert(feature_aggregate(s,3,101,STAT_MEAN,out,NULL)==2&&out[2]==20&&isnan(out[3]));
 assert(feature_aggregate(s,3,15100,STAT_MEAN,out,NULL)==0&&isnan(out[2]));
 s[0].fields=5;s[0].field_age_ms[0]=14000;s[0].field_age_ms[2]=0;
 assert(feature_aggregate(s,1,2100,STAT_MEAN,out,NULL)==1&&isnan(out[0])&&out[2]==10);
 s[0].received_ms=0xfffffff0;s[0].value[2]=0;
 assert(feature_sample_fresh(&s[0],10));
 s[0].fields=0;assert(!feature_sample_fresh(&s[0],10));
 puts("Independent capability aggregation, missing fields, stale removal and zero values passed");
}
