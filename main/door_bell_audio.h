#ifndef DOOR_BELL_AUDIO_H
#define DOOR_BELL_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DOOR_BELL_TONE_RING,
    DOOR_BELL_TONE_OPEN_DOOR,
    DOOR_BELL_TONE_JOIN_SUCCESS,
} door_bell_tone_type_t;

int play_tone(door_bell_tone_type_t type);
int play_tone_int(int t);

#ifdef __cplusplus
}
#endif

#endif
