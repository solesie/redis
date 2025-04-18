#ifndef __REDIS_FDP_NVME_H
#define __REDIS_FDP_NVME_H

#ifndef REDIS_IOURING_DISABLE
#include "io_uring.h"

typedef struct _FdpNvme FdpNvme;

FdpNvme *createFdpNvme();
void releaseFdpNvme(FdpNvme *fdpNvme);
// void prepFdpNvmeIo(FdpNvme *fdpNvme, IOReq *req);

#endif
#endif