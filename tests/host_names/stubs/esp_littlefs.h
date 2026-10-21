#ifndef HOST_NAMES_LITTLEFS_H
#define HOST_NAMES_LITTLEFS_H
#include <stddef.h>
#include "nvs.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_littlefs_info(const char *,size_t *,size_t *);
#ifdef __cplusplus
}
#endif
#endif
