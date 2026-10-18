#ifndef KATMED_STORE_IDF_H
#define KATMED_STORE_IDF_H

#include "katmed_store.h"
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    nvs_handle_t handle;
    bool opened;
} StoreIdfNvs;

/* Initializes NVS and opens only the katmed namespace. Incompatible/full NVS
 * is reported unchanged: this function never erases the partition. */
StoreResult store_idf_open_backend(StoreIdfNvs *nvs, StoreBackend *backend);
StoreBackend store_idf_history_backend(StoreIdfNvs *nvs);
void store_idf_close_backend(StoreIdfNvs *nvs);

#ifdef __cplusplus
}
#endif
#endif
