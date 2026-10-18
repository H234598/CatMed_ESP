#ifndef HOST_STORE_APP_DESC_H
#define HOST_STORE_APP_DESC_H
#include <stddef.h>
typedef struct { char version[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
int esp_app_get_elf_sha256(char *buffer, size_t size);
#endif
