/* Voice Call Demo

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include "settings.h"
#include "media_sys.h"
#include "network.h"
#include "sys_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void init_board(void);

int start_webrtc(char *url);
int start_webrtc_mqtt(void);
void query_webrtc(void);
int stop_webrtc(void);
void send_cmd(char *cmd);

int voice_call_offer(const char* target_id);
int voice_call_answer(void);
int voice_call_reject(void);
int voice_call_close(void);
const char* voice_call_get_state_name(void);
const char* voice_call_get_peer_id(void);

#ifdef __cplusplus
}
#endif
