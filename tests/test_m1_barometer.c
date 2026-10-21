#include <m1.h>
#include "bmp3_vendor/bmp3.h"
#include <assert.h>
#include <stdio.h>

mico_mutex_t m1_lock;
m1_state_t m1_state;
static uint32_t now;
static uint8_t registers[2][128], pointer[2];
static bool present[2], fail_combined, fail_initialize;
static unsigned writes[2], reads, calibration_reads, resets, held;

uint32_t mico_rtos_get_time(void) { return now; }
void mico_thread_msleep(uint32_t ms) { now += ms; }
void mico_rtos_lock_mutex(mico_mutex_t *lock) { (void)lock; assert(!held++); }
void mico_rtos_unlock_mutex(mico_mutex_t *lock) { (void)lock; assert(held-- == 1); }
OSStatus MicoI2cInitialize(mico_i2c_device_t *d)
{
    assert(d->port == MICO_I2C_1 && d->address >= 0x76 && d->address <= 0x77);
    return fail_initialize ? kTimeoutErr : kNoErr;
}
OSStatus MicoI2cBuildCombinedMessage(mico_i2c_message_t *m,const void *tx,void *rx,uint16_t nt,uint16_t nr,uint16_t retry)
{ assert(retry==1); *m=(mico_i2c_message_t){tx,rx,nt,nr}; return 0; }
OSStatus MicoI2cBuildTxMessage(mico_i2c_message_t *m,const void *tx,uint16_t n,uint16_t retry)
{ return MicoI2cBuildCombinedMessage(m,tx,NULL,n,0,retry); }
OSStatus MicoI2cBuildRxMessage(mico_i2c_message_t *m,void *rx,uint16_t n,uint16_t retry)
{ return MicoI2cBuildCombinedMessage(m,NULL,rx,0,n,retry); }
OSStatus MicoI2cTransfer(mico_i2c_device_t *d,mico_i2c_message_t *m,uint16_t count)
{
    assert(count==1 && !held);
    unsigned i=d->address-0x76;
    if (!present[i] || (fail_combined && m->tx_len && m->rx_len)) return kTimeoutErr;
    if (m->tx_len) {
        pointer[i]=m->tx[0];
        if (m->tx_len>1) {
            assert(m->tx_len==2); /* Adapter configures one register per write. */
            assert(m1_barometer_model(registers[i][0])); /* Never configure unknown IDs. */
            writes[i]++;
            assert(pointer[i]+m->tx_len-1<=sizeof(registers[i]));
            memcpy(registers[i]+pointer[i],m->tx+1,m->tx_len-1);
            if (pointer[i]==BMP3_REG_CMD && m->tx[1]==BMP3_SOFT_RESET) {
                registers[i][BMP3_REG_PWR_CTRL]=0;
                registers[i][BMP3_REG_ERR]=0;
                resets++;
            }
        }
    }
    if (m->rx_len) {
        assert(m->rx_len<=21); /* Bounds vendor get_regs VLA for stack review. */
        reads++;
        assert(pointer[i]+m->rx_len<=sizeof(registers[i]));
        if (pointer[i]==BMP3_REG_CALIB_DATA) { assert(m->rx_len==21); calibration_reads++; }
        memcpy(m->rx,registers[i]+pointer[i],m->rx_len);
    }
    return 0;
}

static void attach(unsigned address,uint8_t id)
{
    unsigned i=address-0x76;
    present[i]=true;
    memset(registers[i],0,sizeof(registers[i]));
    registers[i][0]=id;
    registers[i][BMP3_REG_SENS_STATUS]=0x70;
    /* Captured device vector: a decoding regression, not an accuracy reference. */
    const uint8_t trim[]={0x5e,0x6d,0xde,0x48,0xf6,0xd3,0x01,0x6f,0xf6,0x23,0x00,
        0x7b,0x62,0x49,0x79,0xf3,0xf6,0x64,0x42,0x16,0xc4};
    memcpy(registers[i]+BMP3_REG_CALIB_DATA,trim,sizeof(trim));
    uint32_t pressure=8463968,temperature=8663552;
    for(unsigned j=0;j<3;j++) {
        registers[i][BMP3_REG_DATA+j]=pressure>>(8*j);
        registers[i][BMP3_REG_DATA+3+j]=temperature>>(8*j);
    }
}
static void tick(uint32_t at) { now=at; m1_barometer_tick(); assert(!held); }
static void sample(uint32_t at,uint8_t id,uint8_t address)
{
    tick(at);
    assert(m1_state.barometer.ready && m1_state.barometer.received);
    assert(m1_state.barometer.chip_id==id && m1_state.barometer.address==address);
    assert(m1_state.barometer.pressure_centi_pa==8157697);
    assert(m1_barometer_fresh(&m1_state.barometer,now));
}
int main(void)
{
    assert(!m1_barometer_model(0) && !m1_barometer_model(0x58));
    assert(!strcmp(m1_barometer_model(0x50),"BMP388"));
    assert(!strcmp(m1_barometer_model(0x60),"BMP390"));
    tick(2999); assert(!reads && !m1_state.barometer.attempts);
    tick(3000); assert(!m1_state.barometer.ready && m1_state.barometer.errors==1);
    tick(12000); assert(m1_state.barometer.attempts==1); /* Bounded absent-device retries. */
    attach(0x76,0x58); attach(0x77,0x60);
    fail_combined=true; /* Exercise ROM STOP-separated fallback. */
    tick(13000);
    assert(m1_state.barometer.ready && !m1_state.barometer.received);
    assert(!writes[0] && writes[1] && resets==1 && calibration_reads==1);
    sample(14000,0x60,0x77);
    assert(m1_barometer_fresh(&m1_state.barometer,m1_state.barometer.updated_ms+9999));
    assert(!m1_barometer_fresh(&m1_state.barometer,m1_state.barometer.updated_ms+10000));
    present[1]=false;
    tick(15000); assert(!m1_state.barometer.ready && !m1_barometer_fresh(&m1_state.barometer,now));
    fail_combined=false;
    attach(0x76,0x50);
    tick(25000); assert(m1_state.barometer.ready && !m1_state.barometer.received);
    sample(26000,0x50,0x76);
    registers[0][BMP3_REG_SENS_STATUS]=0x10; /* Present but ADC never ready. */
    tick(36000); assert(!m1_state.barometer.ready);
    registers[0][BMP3_REG_SENS_STATUS]=0x70;
    tick(46000); sample(47000,0x50,0x76);
    /* An MCU-only restart must reset a sensor left in normal mode. */
    assert(registers[0][BMP3_REG_PWR_CTRL]==0x33);
    present[0]=false; tick(48000);
    present[0]=true; tick(58000); sample(59000,0x50,0x76);
    assert(resets==4 && calibration_reads==4);
    /* Warning/clamped temperature must never publish a fresh pressure. */
    memset(registers[0]+BMP3_REG_DATA+3,0,3);
    unsigned samples=m1_state.barometer.samples;
    tick(60000); assert(!m1_state.barometer.ready && m1_state.barometer.samples==samples);
    present[0]=present[1]=false; tick(70000);
    assert(!m1_state.barometer.chip_id && !m1_state.barometer.address);
    fail_initialize=true; tick(80000);
    assert(!m1_state.barometer.ready && m1_state.barometer.last_error==kTimeoutErr);
    fail_initialize=false;
    uint32_t at=90000;
    for(unsigned address=0x76;address<=0x77;address++)for(unsigned id=0x50;id<=0x60;id+=0x10) {
        present[0]=present[1]=false;
        attach(address,id);tick(at);sample(at+1000,id,address);
        present[address-0x76]=false;tick(at+2000);at+=12000;
    }
    attach(0x76,0x50);registers[0][BMP3_REG_ODR]=29;
    tick(at);assert(!m1_state.barometer.ready); /* Invalid ODR cannot index outside vendor table. */
    attach(0x76,0x50);tick(at+10000);sample(at+11000,0x50,0x76);
    registers[0][BMP3_REG_ERR]=1;samples=m1_state.barometer.samples;
    tick(at+12000);assert(!m1_state.barometer.ready && m1_state.barometer.samples==samples);
    barometer_state_t wrap={.ready=true,.received=true,.updated_ms=UINT32_MAX-100};
    assert(m1_barometer_fresh(&wrap,50));
    assert(!m1_barometer_fresh(&wrap,10000));
    puts("BMP388/BMP390: IDs/addresses, no-device/unknown, fallback, trim/ADC compensation, loss/recovery, stale/wrap and warnings PASS");
}
