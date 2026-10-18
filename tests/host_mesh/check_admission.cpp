#include "katmed_mesh_status.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <zlib.h>
struct Memory {std::vector<uint8_t> bytes;unsigned writes=0;StoreResult failure=STORE_OK;};
StoreResult read(void *c,uint8_t *p,size_t *n){auto &m=*static_cast<Memory*>(c);if(m.bytes.empty())return STORE_NOT_FOUND;if(*n<m.bytes.size())return STORE_FULL;*n=m.bytes.size();std::memcpy(p,m.bytes.data(),*n);return STORE_OK;}
StoreResult write(void *c,const uint8_t *p,size_t n){auto &m=*static_cast<Memory*>(c);++m.writes;if(m.failure!=STORE_OK)return m.failure;m.bytes.assign(p,p+n);return STORE_OK;}
int main(){
 uint8_t group[16]{7},scratch[4096],page[1044];Memory disk;MeshHistory h{};h.backend={&disk,read,write};
 mesh_history_open(&h,group,scratch);assert(mesh_history_add(&h,11));h.self_initial=11;mesh_history_commit(&h,scratch);
 uint64_t id=0;assert(!mesh_history_unprocessed(&h,&id));assert(mesh_history_admit(&h,22));assert(!mesh_history_unprocessed(&h,&id));
 mesh_history_commit(&h,scratch);assert(mesh_history_unprocessed(&h,&id)&&id==22);
 assert(mesh_history_processed(&h,22));mesh_history_commit(&h,scratch);
 MeshHistory reboot{};reboot.backend=h.backend;mesh_history_open(&reboot,group,scratch);assert(!mesh_history_unprocessed(&reboot,&id));assert(!mesh_history_admit(&reboot,22));
 // A v1 migration preserves every ID and does not invent an admission.
 assert(mesh_history_encode(&h,scratch));std::memcpy(scratch,"KMH1",4);scratch[4]=1;
 uint32_t crc=crc32(0,scratch,2076);for(unsigned i=0;i<4;++i)scratch[2076+i]=uint8_t(crc>>(24-8*i));disk.bytes.assign(scratch,scratch+2080);
 mesh_history_open(&reboot,group,scratch);assert(reboot.count==2&&reboot.dirty&&!reboot.error);assert(!mesh_history_unprocessed(&reboot,&id));
 mesh_history_commit(&reboot,scratch);assert(disk.bytes.size()==MESH_HISTORY_BLOB);
 // Corrupt input can never be replaced by a newly initialized one-ID history.
 disk.bytes[29]^=1;auto original=disk.bytes;auto writes=disk.writes;mesh_history_open(&reboot,group,scratch);assert(reboot.error&&reboot.read_failed);
 mesh_history_add(&reboot,999);mesh_history_commit(&reboot,scratch);assert(disk.bytes==original&&disk.writes==writes);
 disk.bytes[29]^=1;mesh_history_open(&reboot,group,scratch);assert(mesh_history_admit(&reboot,33));disk.failure=STORE_COMMIT_UNKNOWN;mesh_history_commit(&reboot,scratch);assert(reboot.error&&!mesh_history_unprocessed(&reboot,&id));
 MeshHistory after_unknown{};after_unknown.backend=reboot.backend;mesh_history_open(&after_unknown,group,scratch);
 assert(!mesh_history_unprocessed(&after_unknown,&id)); // failed commit cannot become a popup after reboot
 // Unconfirmed facts are never advertised as committed.
 uint64_t current[]{11};auto n=mesh_status_encode(&reboot,MS_CURRENT,0,11,current,1,page);MeshStatusPage parsed{};assert(mesh_status_decode(page,n,true,11,&parsed));
 for(unsigned i=0;i<parsed.page_ids;++i)assert(parsed.ids[9*i+8]==0);
 // root-confirmed local self admission beats self-init, but root's other past facts are baseline.
 Memory remoteDisk;MeshHistory root{};root.backend={&remoteDisk,read,write};mesh_history_open(&root,group,scratch);mesh_history_add(&root,11);assert(mesh_history_admit(&root,22));mesh_history_commit(&root,scratch);
 Memory leafDisk;MeshHistory leaf{};leaf.backend={&leafDisk,read,write};mesh_history_open(&leaf,group,scratch);mesh_history_add(&leaf,22);leaf.self_initial=22;mesh_history_commit(&leaf,scratch);
 n=mesh_status_encode(&root,MS_CURRENT,0,11,current,1,page);assert(mesh_status_decode(page,n,true,11,&parsed));MeshStatusPeer peer{};assert(mesh_status_merge(&leaf,&peer,&parsed,1));mesh_history_commit(&leaf,scratch);assert(mesh_history_unprocessed(&leaf,&id)&&id==22);
 assert(leaf.self_initial==22);assert(mesh_history_processed(&leaf,22));leafDisk.failure=STORE_COMMIT_UNKNOWN;mesh_history_commit(&leaf,scratch);assert(leaf.self_initial==22&&leaf.error);
 leafDisk.failure=STORE_OK;leaf.dirty=true;mesh_history_commit(&leaf,scratch);assert(!leaf.self_initial&&!leaf.error);
 // An ID learned on a normal page before its later confirmed fact still greets active receivers.
 assert(mesh_history_processed(&leaf,22));mesh_history_commit(&leaf,scratch);mesh_history_add(&root,44);mesh_history_commit(&root,scratch);
 n=mesh_status_encode(&root,MS_CURRENT,0,11,current,1,page);assert(mesh_status_decode(page,n,true,11,&parsed));assert(mesh_status_merge(&leaf,&peer,&parsed,2));mesh_history_commit(&leaf,scratch);assert(!mesh_history_unprocessed(&leaf,&id));
 assert(mesh_history_admit(&root,44));mesh_history_commit(&root,scratch);n=mesh_status_encode(&root,MS_CURRENT,0,11,current,1,page);assert(mesh_status_decode(page,n,true,11,&parsed));assert(mesh_status_merge(&leaf,&peer,&parsed,3));mesh_history_commit(&leaf,scratch);assert(mesh_history_unprocessed(&leaf,&id)&&id==44);
 std::puts("admission: atomic fact/processed, KMH1 migration, corrupt preservation, commitunknown, baseline/selfjoin and late fact passed");
}
