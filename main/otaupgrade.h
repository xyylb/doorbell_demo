#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIRMWARE_VERSION 2

void ota_start_upgrade(const char* url, const char* md5);

#ifdef __cplusplus
}
#endif
