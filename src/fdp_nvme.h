#ifndef __REDIS_FDP_NVME_H
#define __REDIS_FDP_NVME_H

#ifndef REDIS_IOURING_DISABLE
#include "io_uring.h"

typedef struct _fdpNvme fdpNvme;

fdpNvme *fdpNvmeCreate(const char *bdev_name);
void fdpNvmeRelease(fdpNvme *fdp_nvme);
void fdpNvmePrepReadUringCmdSqe(
    fdpNvme *fdp_nvme,
    struct io_uring_sqe *sqe,
    void *buf,
    size_t size,
    off_t start);
void fdpNvmePrepWriteUringCmdSqe(
    fdpNvme *fdp_nvme,
    struct io_uring_sqe *sqe, 
    const void *buf, 
    size_t size, 
    off_t start, 
    int handle);
int fdpNvmeAllocateFdpHandle(fdpNvme *fdp_nvme);
uint32_t fdpNvmeGetMaxIOSize(fdpNvme *fdp_nvme);
uint16_t fdpNvmeGetMaxPIDLength(fdpNvme *fdp_nvme);
uint32_t fdpNvmeGetPreferredWriteSize(fdpNvme *fdp_nvme);
uint32_t fdpNvmeGetLbSize(fdpNvme *fdp_nvme);
uint32_t fdpNvmeGetLbaShift(fdpNvme *fdp_nvme);
uint64_t fdpNvmeGetStartLba(fdpNvme *fdp_nvme);
uint64_t fdpNvmeGetEndLba(fdpNvme *fdp_nvme);

#endif
#endif