#include "katmed_names_idf.h"
#include "katmed_source_export.h"
#include "katmed_security_layout.h"
#include "esp_littlefs.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#ifndef KATMED_NAMES_DIRECTORY
#define KATMED_NAMES_DIRECTORY "/katmed/mesh-names"
#endif
static const char *const paths[2]={KATMED_NAMES_DIRECTORY "/ledger-a.bin",KATMED_NAMES_DIRECTORY "/ledger-b.bin"};
static const char *key(NamesKey k){return k==NAMES_CTL?"names_ctl":"names_view";}
static StoreResult io_error(void){return errno==ENOSPC?STORE_FULL:STORE_IO;}
static StoreResult meta_read(void *arg,NamesKey k,uint8_t *bytes,size_t *n)
{
    NamesIdf *b=arg;
    if(!b||!b->nvs||!b->nvs->opened||!bytes||!n)return STORE_UNAVAILABLE;
    size_t size=0;
    esp_err_t e=nvs_get_blob(b->nvs->handle,key(k),NULL,&size);
    if(e==ESP_ERR_NVS_NOT_FOUND)return STORE_NOT_FOUND;
    if(e!=ESP_OK)return STORE_IO;
    if(size!=(k==NAMES_CTL?480u:48u)||size>*n)return STORE_CORRUPT;
    e=nvs_get_blob(b->nvs->handle,key(k),bytes,&size);
    if(e!=ESP_OK)return STORE_IO;
    *n=size;
    return STORE_OK;
}
static StoreResult meta_commit(void *arg,NamesKey k,const uint8_t *bytes,size_t n)
{
    NamesIdf *b=arg;
    if(!b||!b->nvs||!b->nvs->opened||!bytes||n!=(k==NAMES_CTL?480u:48u))return STORE_INVALID;
    if(!katmed_source_writer_enter())return STORE_UNAVAILABLE;
    esp_err_t e=nvs_set_blob(b->nvs->handle,key(k),bytes,n);
    StoreResult r=e==ESP_ERR_NVS_NOT_ENOUGH_SPACE?STORE_FULL:e==ESP_OK?STORE_OK:STORE_IO;
    if(r==STORE_OK&&nvs_commit(b->nvs->handle)!=ESP_OK)r=STORE_COMMIT_UNKNOWN;
    katmed_source_writer_leave();
    return r;
}
static StoreResult file_size(void *arg,unsigned bank,uint32_t *size)
{
    (void)arg;
    if(bank>1||!size)return STORE_INVALID;
    struct stat st;
    if(stat(paths[bank],&st)!=0)return errno==ENOENT?STORE_NOT_FOUND:io_error();
    if(!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>524344)return STORE_CORRUPT;
    *size=(uint32_t)st.st_size;
    return STORE_OK;
}
static int handle(NamesIdf *b,unsigned bank,bool create)
{
    if(bank>1)return -1;
    if(b->fd[bank]>=0)return b->fd[bank];
    if(create&&mkdir(KATMED_NAMES_DIRECTORY,0700)!=0&&errno!=EEXIST)return -1;
    b->fd[bank]=open(paths[bank],O_RDWR|(create?O_CREAT:0),0600);
    return b->fd[bank];
}
static StoreResult file_read(void *arg,unsigned bank,uint32_t offset,uint8_t *bytes,size_t n)
{
    if(!arg||bank>1||!bytes||offset>524344||n>524344-offset)return STORE_INVALID;
    int fd=handle(arg,bank,false);
    if(fd<0)return errno==ENOENT?STORE_NOT_FOUND:io_error();
    if(lseek(fd,(off_t)offset,SEEK_SET)<0)return io_error();
    /* A short read is corruption; never reinterpret a missing tail as zeros. */
    ssize_t got=read(fd,bytes,n);
    return got<0?io_error():(size_t)got==n?STORE_OK:STORE_CORRUPT;
}
static StoreResult file_write(void *arg,unsigned bank,uint32_t offset,const uint8_t *bytes,size_t n)
{
    if(!arg||bank>1||!bytes||offset>524344||n>524344-offset)return STORE_INVALID;
    size_t total=0,used=0;uint32_t existing=0;
    StoreResult stat_result=file_size(arg,bank,&existing);
    if(stat_result!=STORE_OK&&stat_result!=STORE_NOT_FOUND)return stat_result;
    const size_t growth=offset+n>existing?offset+n-existing:0;
    if(esp_littlefs_info(KATMED_USER_IMAGE_PARTITION_LABEL,&total,&used)!=ESP_OK||used>total)return STORE_IO;
    /* Conservative shared-FS reserve, never a lifetime-device constant.
     * Concurrent image activity can still cause a real short write/ENOSPC. */
    if(total-used<32768u||growth>total-used-32768u)return STORE_FULL;
    if(!katmed_source_writer_enter())return STORE_UNAVAILABLE;
    int fd=handle(arg,bank,true);
    StoreResult r=STORE_OK;
    if(fd<0||lseek(fd,(off_t)offset,SEEK_SET)<0)r=io_error();
    else {ssize_t wrote=write(fd,bytes,n);if(wrote<0)r=io_error();else if((size_t)wrote!=n)r=STORE_IO;}
    katmed_source_writer_leave();
    return r;
}
static StoreResult file_truncate(void *arg,unsigned bank,uint32_t n)
{
    if(!arg||bank>1||n>524344)return STORE_INVALID;
    if(!katmed_source_writer_enter())return STORE_UNAVAILABLE;
    int fd=handle(arg,bank,true);
    StoreResult r=fd<0||ftruncate(fd,(off_t)n)!=0?io_error():STORE_OK;
    katmed_source_writer_leave();
    return r;
}
static StoreResult file_sync(void *arg,unsigned bank)
{
    if(!arg||bank>1)return STORE_INVALID;
    if(!katmed_source_writer_enter())return STORE_UNAVAILABLE;
    int fd=handle(arg,bank,false);
    StoreResult r=fd<0||fsync(fd)!=0?io_error():STORE_OK;
    katmed_source_writer_leave();
    return r;
}
static StoreResult backend_reopen(void *arg)
{
    NamesIdf *b=arg;
    if(!b||!b->nvs)return STORE_INVALID;
    names_idf_close(b);
    if(b->nvs->opened)nvs_close(b->nvs->handle);
    b->nvs->opened=false;
    esp_err_t e=nvs_open("katmed",NVS_READWRITE,&b->nvs->handle);
    if(e!=ESP_OK)return STORE_IO;
    b->nvs->opened=true;
    return STORE_OK;
}
NamesBackend names_idf_backend(NamesIdf *b,StoreIdfNvs *nvs)
{
    memset(b,0,sizeof(*b));b->nvs=nvs;b->fd[0]=b->fd[1]=-1;
    return (NamesBackend){b,meta_read,meta_commit,file_size,file_read,file_write,file_truncate,file_sync,backend_reopen};
}
void names_idf_close(NamesIdf *b)
{
    if(!b)return;
    for(unsigned i=0;i<2;++i){if(b->fd[i]>=0)close(b->fd[i]);b->fd[i]=-1;}
}
bool names_idf_clear_files(void)
{
    for(unsigned i=0;i<2;++i)if(unlink(paths[i])!=0&&errno!=ENOENT)return false;
    return rmdir(KATMED_NAMES_DIRECTORY)==0||errno==ENOENT;
}
