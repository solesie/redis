#ifndef __REDIS_fdp_module_H
#define __REDIS_fdp_module_H

#ifndef REDIS_IOURING_DISABLE
#include "fdp_nvme.h"
#include "io_uring.h"

typedef struct _fdpModule fdpModule;

fdpModule *fdpModuleCreate(
    fdpNvme *fdp_nvme, 
    uint32_t resubmit_limit, 
    uint32_t qdepth,
    void (*success_cb)(void *arg));
void fdpModuleRelease(fdpModule *fdp_module);
void fdpModuleIOWrite(
    fdpModule *fdp_module, 
    void *aligned_buf, 
    size_t aligned_len, 
    uint64_t aligned_offt,
    int placement_handle,
    int reclaim_group,
    void *success_cb_arg);
void fdpModuleIORead(
    fdpModule *fdp_module, 
    void *aligned_buf, 
    size_t aligned_len,
    uint64_t aligned_offt,
    void *success_cb_arg);
int fdpModuleIOWait(fdpModule *fdp_module);


#endif
#endif