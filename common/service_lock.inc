/* SPDX-License-Identifier: GPL-3.0-or-later */
/* All command-mutex users share this admission token. A LAN caller takes it
 * without waiting; after admission nobody else can own command_lock. Keep the
 * mutex for the existing control/dispatcher critical section. */
static mico_semaphore_t command_gate;
static bool command_gate_ready;
static bool service_command_enter(bool wait)
{
    if(!command_gate_ready||mico_rtos_get_semaphore(&command_gate,wait?MICO_WAIT_FOREVER:0)!=kNoErr)return false;
    mico_rtos_lock_mutex(&command_lock);return true;
}
static void service_command_leave(void)
{mico_rtos_unlock_mutex(&command_lock);mico_rtos_set_semaphore(&command_gate);}
static bool service_command_gate_init(void)
{
    if(command_gate_ready)return true;
    if(mico_rtos_init_semaphore(&command_gate,1)!=kNoErr)return false;
    if(mico_rtos_set_semaphore(&command_gate)!=kNoErr){mico_rtos_deinit_semaphore(&command_gate);return false;}
    command_gate_ready=true;return true;
}
