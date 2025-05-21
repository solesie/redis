#ifndef __REDIS_FDP_DEVICE_H
#define __REDIS_FDP_DEVICE_H

#ifndef REDIS_IOURING_DISABLE
#include "aligned_buffer.h"
#include "fdp_nvme.h"
#include "io_uring.h"

typedef struct _FdpDevice FdpDevice;

FdpDevice *fdpDeviceCreate(FdpNvme *fdpNvme, uint32_t resubmitLimit, uint32_t qdepth);
void fdpDeviceRelease(FdpDevice *fdpDevice);
void fdpDeviceIOWrite(FdpDevice *fdpDevice, AlignedBuffer *buf, int placementHandle);
void fdpDeviceIORead(FdpDevice *fdpDevice, AlignedBuffer *buf);
int fdpDeviceIOWait(FdpDevice *fdpDevice);


#endif
#endif