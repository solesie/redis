#ifndef __REDIS_FDP_NVME_H
#define __REDIS_FDP_NVME_H

#ifndef REDIS_IOURING_DISABLE
#include "io_uring.h"

typedef struct _FdpNvme FdpNvme;

FdpNvme *fdpNvmeCreate(const char *bdevName);
void fdpNvmeRelease(FdpNvme *fdpNvme);
void fdpNvmePrepReadUringCmdSqe(
    FdpNvme *fdpNvme,
    struct io_uring_sqe *sqe,
    void *buf,
    size_t size,
    off_t start);
void fdpNvmePrepWriteUringCmdSqe(
    FdpNvme *fdpNvme,
    struct io_uring_sqe *sqe, 
    const void *buf, 
    size_t size, 
    off_t start, 
    int handle);
int fdpNvmeAllocateFdpHandle(FdpNvme *fdpNvme);
uint32_t fdpNvmeGetMaxIOSize(FdpNvme *fdpNvme);
uint16_t fdpNvmeGetMaxPIDLength(FdpNvme *fdpNvme);
uint32_t fdpNvmeGetPreferredWriteSize(FdpNvme *fdpNvme);
uint32_t fdpNvmeGetLbSize(FdpNvme *fdpNvme);

#endif
#endif