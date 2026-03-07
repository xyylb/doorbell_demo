#ifndef CALL_STATE_MACHINE_H_
#define CALL_STATE_MACHINE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CALL_STATE_IDLE,      
    CALL_STATE_CALLING,   
    CALL_STATE_INCOMING,  
    CALL_STATE_IN_CALL,   
} call_state_t;

typedef enum {
    CALL_CMD_OFFER,       
    CALL_CMD_ANSWER,      
    CALL_CMD_REJECT,      
    CALL_CMD_CLOSE,       
    CALL_CMD_RECV_OFFER,  
    CALL_CMD_RECV_ANSWER, 
    CALL_CMD_RECV_REJECT, 
    CALL_CMD_RECV_BYE,    
    CALL_CMD_RECV_TIMEOUT,
    CALL_CMD_TIMEOUT,     
} call_command_t;

typedef struct {
    call_state_t current_state;
    char peer_device_id[64];
    bool is_initiator;
    uint32_t state_enter_time;
    uint32_t call_timeout_ms;    
    uint32_t incoming_timeout_ms; 
} call_state_machine_t;

typedef void (*call_state_callback_t)(call_state_t old_state, call_state_t new_state, const char* peer_id, void* ctx);

void call_state_machine_init(call_state_machine_t* sm);
call_state_t call_state_machine_get_state(call_state_machine_t* sm);
const char* call_state_machine_get_state_name(call_state_t state);
const char* call_state_machine_get_peer_id(call_state_machine_t* sm);
bool call_state_machine_is_initiator(call_state_machine_t* sm);
int call_state_machine_process_command(call_state_machine_t* sm, call_command_t cmd, const char* peer_id);
void call_state_machine_set_callback(call_state_machine_t* sm, call_state_callback_t callback, void* ctx);
void call_state_machine_set_peer(call_state_machine_t* sm, const char* peer_id, bool is_initiator);
void call_state_machine_clear_peer(call_state_machine_t* sm);
void call_state_machine_update(call_state_machine_t* sm);

#ifdef __cplusplus
}
#endif

#endif
