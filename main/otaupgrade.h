#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ota_start_upgrade(const char* url, const char* md5);
int get_firmware_version(void);

#ifdef __cplusplus
}
#endif
