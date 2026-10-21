#ifndef KATMED_MESH_NAMES_H
#define KATMED_MESH_NAMES_H
#include "katmed_names.h"
#ifdef __cplusplus
extern "C" {
#endif
NamesResult katmed_mesh_names_backend(NamesBackend,uint64_t,const uint8_t group[16]);
void katmed_mesh_device_name(char *out,size_t capacity,bool *assigned,bool *uncertain);
#ifdef __cplusplus
}
#endif
#endif
