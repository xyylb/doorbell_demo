#include "door_bell_audio.h"
#include "media_sys.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t ring_music_start[] asm("_binary_ring_aac_start");
extern const uint8_t ring_music_end[] asm("_binary_ring_aac_end");
extern const uint8_t open_music_start[] asm("_binary_open_aac_start");
extern const uint8_t open_music_end[] asm("_binary_open_aac_end");
extern const uint8_t join_music_start[] asm("_binary_join_aac_start");
extern const uint8_t join_music_end[] asm("_binary_join_aac_end");

static const char *TAG = "DOOR_BELL_AUDIO";

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    int            duration;
} door_bell_tone_data_t;

static door_bell_tone_data_t tone_data[] = {
    { ring_music_start, ring_music_end, 4000 },
    { open_music_start, open_music_end, 0 },
    { join_music_start, join_music_end, 0 },
};

int play_tone(door_bell_tone_type_t type)
{
    if (type >= sizeof(tone_data) / sizeof(tone_data[0])) {
        ESP_LOGE(TAG, "Invalid tone type: %d", type);
        return 0;
    }
    return play_music(tone_data[type].start, (int)(tone_data[type].end - tone_data[type].start), tone_data[type].duration);
}

int play_tone_int(int t)
{
    return play_tone((door_bell_tone_type_t)t);
}

#ifdef __cplusplus
}
#endif
