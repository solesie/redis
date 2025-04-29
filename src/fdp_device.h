#ifndef __REDIS_FDP_DEVICE_H
#define __REDIS_FDP_DEVICE_H

#ifndef REDIS_IOURING_DISABLE
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
ssize_t fdpDeviceWriteSync(FdpDevice *fdpDevice, off_t offset, const uint8_t *data, size_t size, int placementHandle);
ssize_t fdpDeviceReadSync(FdpDevice *fdpDevice, off_t offset, const uint8_t *data, size_t size);
// ssize_t fdpDeviceWriteAsync(FdpDevice *fdpDevice, off_t offset, uint8_t *data, size_t size, int placementHandle);
// int handling();

#endif
#endif