#ifndef TEST_CJSON_H
#define TEST_CJSON_H
#include <stddef.h>
typedef struct cJSON cJSON;
int cJSON_IsObject(const cJSON *);
int cJSON_IsString(const cJSON *);
cJSON *cJSON_ParseWithLength(const char *, size_t);
void cJSON_Delete(cJSON *);
const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *, const char *);
const char *cJSON_GetStringValue(const cJSON *);
#endif
