#ifndef TEST_CJSON_H
#define TEST_CJSON_H
#include <stddef.h>
typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *child;
    char *string;
    char *valuestring;
    double valuedouble;
    int type;
} cJSON;
int cJSON_IsObject(const cJSON *);
int cJSON_IsArray(const cJSON *);
int cJSON_IsString(const cJSON *);
int cJSON_IsNumber(const cJSON *);
cJSON *cJSON_ParseWithLength(const char *, size_t);
void cJSON_Delete(cJSON *);
const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *, const char *);
const cJSON *cJSON_GetArrayItem(const cJSON *, int);
int cJSON_GetArraySize(const cJSON *);
const char *cJSON_GetStringValue(const cJSON *);
#endif
