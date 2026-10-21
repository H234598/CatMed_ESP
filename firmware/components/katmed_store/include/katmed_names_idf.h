#ifndef KATMED_NAMES_IDF_H
#define KATMED_NAMES_IDF_H
#include "katmed_names.h"
#include "katmed_store_idf.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { StoreIdfNvs *nvs; int fd[2]; } NamesIdf;
NamesBackend names_idf_backend(NamesIdf *,StoreIdfNvs *);
void names_idf_close(NamesIdf *);
/* Only called by the existing authorized complete-reset journal file stage. */
bool names_idf_clear_files(void);
#ifdef __cplusplus
}
#endif
#endif
