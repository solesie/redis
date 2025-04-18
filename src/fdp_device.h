#ifndef __REDIS_FDP_DEVICE_H
#define __REDIS_FDP_DEVICE_H

#ifndef REDIS_IOURING_DISABLE
#include "io_uring.h"

typedef struct _FdpDevice FdpDevice;

// IO Operation type supported by IOReq
// typedef enum { 
//     INVALID = 0, 
//     READ, 
//     WRITE 
// } OpType;

typedef enum {
    SYNC,
    EVENT
} CQHandlingMode;

FdpDevice *fdpDeviceCreate(
    CQHandlingMode cqHandlingMode,
    size_t qDepth,
    int fd, 
    size_t ioAlignmentSize, 
    size_t maxIoSize, 
    size_t maxWriteSize);
void fdpDeviceRelease(FdpDevice *fdpDevice);
// int fdpDeviceWrite(FdpDevice *fdpDevice, uint64_t offset, uint8_t* data, size_t size, uint8_t placementHandle);
// int fdpDeviceRead(FdpDevice *fdpDevice, uint64_t offset, uint8_t* data, size_t size);

#endif
#endif