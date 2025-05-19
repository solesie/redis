#ifndef __REDIS_FDP_DEVICE_H
#define __REDIS_FDP_DEVICE_H

#ifndef REDIS_IOURING_DISABLE
#include "aligned_buffer.h"
#include "fdp_nvme.h"
#include "io_uring.h"

typedef struct _FdpDevice FdpDevice;

/* solesie: User-defined request for async operation.
 * For now, not implemented yet. */
// typedef struct _IOReq{
//     IOUringOp *op;
//     uint64_t offset;
//     size_t size;
//     void *data;
//     uint16_t *placementHandle;

//     int is_req_successful;  /* 1 on success, 0 on failure */
// }IOReq;

FdpDevice *fdpDeviceCreate(FdpNvme *fdpNvme, size_t asyncIOUringQDepth);
void fdpDeviceRelease(FdpDevice *fdpDevice);
ssize_t fdpDeviceWriteSync(FdpDevice *fdpDevice, AlignedBuffer *buf, int placementHandle);
ssize_t fdpDeviceReadSync(FdpDevice *fdpDevice, AlignedBuffer *buf);
ssize_t fdpDeviceWriteAsync(FdpDevice *fdpDevice, AlignedBuffer *buf, int placementHandle);
// int handling();

#endif
#endif