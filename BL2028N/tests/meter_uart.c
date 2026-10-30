/* Exercise the actual product tick with separate programming/meter UARTs. */
#include <assert.h>
#include <math.h>
#include "../src/ph_port.c"
void bl_mqtt_start(void){}
void bl_mqtt_diagnostics(ph_writer *w){(void)w;}
static void u24(unsigned char *out,unsigned value){out[0]=value>>16;out[1]=value>>8;out[2]=value;}
int main(int argc,char **argv){
 assert(argc==3);snprintf(store_dir,sizeof(store_dir),"%s",argv[1]);assert(!mkdir(store_dir,0700));
 PH_BL_Init(atoi(argv[2]));assert(bl_ready);
 unsigned char frame[24]={0x55,0x5a};
 for(unsigned i=0;i<3;++i){u24(frame+2+i*6,i==1?1000:122340);u24(frame+5+i*6,1000);}
 frame[20]=0x70;frame[22]=1;for(unsigned i=2;i<23;++i)frame[23]+=frame[i];
 /* A valid frame on VTX/VRX must not be mistaken for metering input. */
 host_uart_feed(UART_PORT_INDEX_0,frame,sizeof(frame));PH_BL_Tick();
 assert(!bl_device.meter.frames&&!bl_device.meter.received_bytes);
 assert(UART_GetDataSizeEx(UART_PORT_INDEX_0)==sizeof(frame));
 host_uart_feed(UART_PORT_INDEX_1,frame,11);PH_BL_Tick();
 assert(!bl_device.meter.frames&&bl_device.meter.received_bytes==11);
 host_uart_feed(UART_PORT_INDEX_1,frame+11,13);PH_BL_Tick();
 assert(bl_device.meter.frames==1&&bl_device.meter.received_bytes==24&&!bl_device.meter.bad_frames);
 assert(ph_meter_fields(&bl_device.meter,rtos_get_time())==7);
 assert(fabs(bl_device.meter.value[0]-231.74719392)<0.0001&&bl_device.meter.value[1]==1.0102);
 assert(fabs(bl_device.meter.value[2]-231.0207498960506)<0.0001);
 assert(UART_GetDataSizeEx(UART_PORT_INDEX_1)==0&&UART_GetDataSizeEx(UART_PORT_INDEX_0)==24);
 puts("PASS: product tick receives fragmented HLW8032 frames from UART2, ignores programming UART1, and keeps SDK logs off the meter UART");
}
