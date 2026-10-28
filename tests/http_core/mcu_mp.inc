
static unsigned mcu_calls,mcu_commits,mcu_received,mcu_expected,m1_lock;static bool mcu_fail;
static struct {bool mcu_updating,ota_active,reboot_pending,wifi_trial_active,uart_ready;} m1_state={.uart_ready=true};
static void mico_rtos_lock_mutex(unsigned*p){assert(!*p);*p=1;}
static void mico_rtos_unlock_mutex(unsigned*p){assert(*p);*p=0;}
static bool m1_mcu_exchange(const uint8_t*p,size_t n,uint8_t out[20]){
 assert(p[0]==0x23);mcu_calls++;memset(out,0,20);out[0]=0x23;out[19]=0x21;
 if(p[1]==0x16&&p[2]==1){m1_state.mcu_updating=false;return true;}
 if(mcu_fail)return false;
 if(p[1]==0x70){assert(n==12);out[1]=0x70;return true;}
 out[1]=0x14;
 if(p[1]==0x12){assert(n==12);mcu_expected=p[2]*256+p[3];mcu_received=0;m1_state.mcu_updating=true;}
 else if(p[1]==0x13){assert(n>=6&&n<=262);mcu_received+=n-6;out[2]=p[2];out[3]=p[3];out[4]=1;}
 else {assert(p[1]==0x16&&n==12&&mcu_received==mcu_expected);mcu_commits++;m1_state.mcu_updating=false;}
 return true;
}

