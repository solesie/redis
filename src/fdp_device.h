#ifndef __REDIS_FDP_DEVICE_H
#define __REDIS_FDP_DEVICE_H

#ifndef REDIS_IOURING_DISABLE
#include "fdp_nvme.h"
#include "io_uring.h"

typedef struct _fdpDevice fdpDevice;

fdpDevice *fdpDeviceCreate(
    fdpNvme *fdp_nvme, 
    uint32_t resubmit_limit, 
    uint32_t qdepth,
    void (*success_cb)(void *arg));
void fdpDeviceRelease(fdpDevice *fdp_device);
void fdpDeviceIOWrite(
    fdpDevice *fdp_device, 
    void *aligned_buf, 
    size_t aligned_len, 
    uint64_t aligned_offt,
    int placement_handle,
    void *success_cb_arg);
void fdpDeviceIORead(
    fdpDevice *fdp_device, 
    void *aligned_buf, 
    size_t aligned_len,
    uint64_t aligned_offt,
    void *success_cb_arg);
int fdpDeviceIOWait(fdpDevice *fdp_device);


#endif
#endif