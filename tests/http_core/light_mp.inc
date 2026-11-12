
static unsigned a1_lock;
static struct {bool ota_active,reboot_pending,light_test_neighbors;unsigned light_test_slot,light_test_until;} a1_state;
static void mico_rtos_lock_mutex(unsigned*p){assert(!*p);*p=1;}
static void mico_rtos_unlock_mutex(unsigned*p){assert(*p);*p=0;}
