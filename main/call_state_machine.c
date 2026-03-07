#include "call_state_machine.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <inttypes.h>
#include <string.h>

static const char* TAG = "CALL_SM";

static call_state_callback_t g_callback = NULL;
static void* g_callback_ctx = NULL;

static const char* state_names[] = {
    "IDLE",
    "CALLING",
    "INCOMING",
    "IN_CALL"
};

void call_state_machine_init(call_state_machine_t* sm) {
    if (!sm) return;
    memset(sm, 0, sizeof(call_state_machine_t));
    sm->current_state = CALL_STATE_IDLE;
    sm->call_timeout_ms = 30000;   
    sm->incoming_timeout_ms = 15000; 
}

call_state_t call_state_machine_get_state(call_state_machine_t* sm) {
    return sm ? sm->current_state : CALL_STATE_IDLE;
}

const char* call_state_machine_get_state_name(call_state_t state) {
    if (state >= 0 && state < sizeof(state_names) / sizeof(state_names[0])) {
        return state_names[state];
    }
    return "UNKNOWN";
}

const char* call_state_machine_get_peer_id(call_state_machine_t* sm) {
    return sm ? sm->peer_device_id : "";
}

bool call_state_machine_is_initiator(call_state_machine_t* sm) {
    return sm ? sm->is_initiator : false;
}

void call_state_machine_set_callback(call_state_machine_t* sm, call_state_callback_t callback, void* ctx) {
    g_callback = callback;
    g_callback_ctx = ctx;
}

void call_state_machine_set_peer(call_state_machine_t* sm, const char* peer_id, bool is_initiator) {
    if (!sm || !peer_id) return;
    strncpy(sm->peer_device_id, peer_id, sizeof(sm->peer_device_id) - 1);
    sm->peer_device_id[sizeof(sm->peer_device_id) - 1] = '\0';
    sm->is_initiator = is_initiator;
}

void call_state_machine_clear_peer(call_state_machine_t* sm) {
    if (!sm) return;
    sm->peer_device_id[0] = '\0';
    sm->is_initiator = false;
}

static void change_state(call_state_machine_t* sm, call_state_t new_state) {
    if (!sm || sm->current_state == new_state) return;
    
    call_state_t old_state = sm->current_state;
    sm->current_state = new_state;
    sm->state_enter_time = (uint32_t)(esp_timer_get_time() / 1000);
    
    ESP_LOGI(TAG, "State change: %s -> %s (peer: %s)", 
             state_names[old_state], state_names[new_state], sm->peer_device_id);
    
    if (g_callback) {
        g_callback(old_state, new_state, sm->peer_device_id, g_callback_ctx);
    }
}

int call_state_machine_process_command(call_state_machine_t* sm, call_command_t cmd, const char* peer_id) {
    if (!sm) return -1;
    
    ESP_LOGI(TAG, "Process cmd %d in state %s (peer: %s)", 
             cmd, state_names[sm->current_state], peer_id ? peer_id : "NULL");
    
    switch (sm->current_state) {
        case CALL_STATE_IDLE:
            if (cmd == CALL_CMD_OFFER && peer_id) {
                call_state_machine_set_peer(sm, peer_id, true);
                change_state(sm, CALL_STATE_CALLING);
                return 0;
            }
            else if (cmd == CALL_CMD_RECV_OFFER && peer_id) {
                call_state_machine_set_peer(sm, peer_id, false);
                change_state(sm, CALL_STATE_INCOMING);
                return 0;
            }
            break;
            
        case CALL_STATE_CALLING:
            if (cmd == CALL_CMD_RECV_ANSWER) {
                change_state(sm, CALL_STATE_IN_CALL);
                return 0;
            }
            else if (cmd == CALL_CMD_RECV_REJECT || cmd == CALL_CMD_RECV_TIMEOUT || cmd == CALL_CMD_TIMEOUT) {
                call_state_machine_clear_peer(sm);
                change_state(sm, CALL_STATE_IDLE);
                return 0;
            }
            else if (cmd == CALL_CMD_CLOSE) {
                call_state_machine_clear_peer(sm);
                change_state(sm, CALL_STATE_IDLE);
                return 0;
            }
            break;
            
        case CALL_STATE_INCOMING:
            if (cmd == CALL_CMD_ANSWER) {
                change_state(sm, CALL_STATE_IN_CALL);
                return 0;
            }
            else if (cmd == CALL_CMD_REJECT || cmd == CALL_CMD_TIMEOUT) {
                call_state_machine_clear_peer(sm);
                change_state(sm, CALL_STATE_IDLE);
                return 0;
            }
            else if (cmd == CALL_CMD_CLOSE) {
                call_state_machine_clear_peer(sm);
                change_state(sm, CALL_STATE_IDLE);
                return 0;
            }
            break;
            
        case CALL_STATE_IN_CALL:
            if (cmd == CALL_CMD_CLOSE || cmd == CALL_CMD_RECV_BYE || cmd == CALL_CMD_RECV_REJECT) {
                call_state_machine_clear_peer(sm);
                change_state(sm, CALL_STATE_IDLE);
                return 0;
            }
            break;
    }
    
    ESP_LOGW(TAG, "Invalid cmd %d in state %s", cmd, state_names[sm->current_state]);
    return -1;
}

void call_state_machine_update(call_state_machine_t* sm) {
    if (!sm) return;
    
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t elapsed = now - sm->state_enter_time;
    
    if (sm->current_state == CALL_STATE_CALLING) {
        if (elapsed >= sm->call_timeout_ms) {
            ESP_LOGW(TAG, "Call timeout after %" PRIu32 " ms", elapsed);
            call_state_machine_process_command(sm, CALL_CMD_TIMEOUT, NULL);
        }
    }
    else if (sm->current_state == CALL_STATE_INCOMING) {
        if (elapsed >= sm->incoming_timeout_ms) {
            ESP_LOGW(TAG, "Incoming call timeout after %" PRIu32 " ms", elapsed);
            call_state_machine_process_command(sm, CALL_CMD_TIMEOUT, NULL);
        }
    }
}
