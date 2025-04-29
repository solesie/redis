#ifndef REDIS_IOURING_DISABLE
#include "fdp_device.h"
#include "redisassert.h"
#include "fdp_nvme.h"

#define min(a, b) (a) < (b) ? a : b

struct _FdpDevice{
    IOUring *syncIOUring;
    IOUring *asyncIOUring;
    FdpNvme *fdpNvme;

    /* Some devices have this transfer size limit due to DMA size limitations.
     * This limit is applicable for both writes and reads. */
    size_t maxIOSize;
};

/*
 * solesie: Creates FDP(Flexible Data Placement) device object.
 * 
 * @note For multithreaded applications, it is necessary to have 1 FdpDevice per thread.
 */
FdpDevice *fdpDeviceCreate(FdpNvme *fdpNvme, size_t asyncIOUringQDepth){
    FdpDevice *fdpDevice = zcalloc(sizeof(*fdpDevice));
    
    fdpDevice->syncIOUring = ioUringCreate(1, CQ_SYNC, 1);
    if(asyncIOUringQDepth != 0){
        fdpDevice->asyncIOUring = ioUringCreate(1, CQ_ASYNC, asyncIOUringQDepth);
    }

    fdpDevice->fdpNvme = fdpNvme;

    fdpDevice->maxIOSize = fdpNvmeGetMaxIOSize(fdpDevice->fdpNvme);
}

void fdpDeviceRelease(FdpDevice *fdpDevice){
    ioUringRelease(fdpDevice->syncIOUring);
    ioUringRelease(fdpDevice->asyncIOUring);
    fdpNvmeRelease(fdpDevice->fdpNvme);
    zfree(fdpDevice);
}

ssize_t fdpDeviceWriteSync(FdpDevice *fdpDevice, off_t offset, const uint8_t *data, size_t size, int placementHandle){
    size_t remainingSize = size;
    ssize_t ret = 0;
    while(remainingSize > 0){
        size_t writeSize = min(fdpDevice->maxIOSize, remainingSize);
        
        IOUringOp *op = ioUringOpCreate(fdpDevice->syncIOUring);
        struct io_uring_sqe *sqe = ioUringOpGetSqe(op);
        IOUringOp **completed;

        fdpNvmePrepWriteUringCmdSqe(fdpDevice->fdpNvme, sqe, (const void*)data, writeSize, offset, placementHandle);
        ioUringSubmitOp(fdpDevice->syncIOUring, op);
        size_t completedLen = ioUringWaitOps(fdpDevice->syncIOUring, 1, &completed);

        assert(completedLen == 1 && completed[0] == op);

        ssize_t bytes = ioUringOpGetResult(op);
        ioUringOpRelease(&op);
        if(bytes < 0){
            return bytes;
        } else{
            ret += bytes;
        }

        offset += bytes;
        data += bytes;
        remainingSize -= bytes;
    }
    return ret;
}

ssize_t fdpDeviceReadSync(FdpDevice *fdpDevice, off_t offset, const uint8_t *data, size_t size){
    size_t remainingSize = size;
    ssize_t ret = 0;
    while(remainingSize > 0){
        size_t readSize = min(fdpDevice->maxIOSize, remainingSize);
        
        IOUringOp *op = ioUringOpCreate(fdpDevice->syncIOUring);
        struct io_uring_sqe *sqe = ioUringOpGetSqe(op);
        IOUringOp **completed;

        fdpNvmePrepReadUringCmdSqe(fdpDevice->fdpNvme, sqe, (const void*)data, readSize, offset);
        ioUringSubmitOp(fdpDevice->syncIOUring, op);
        size_t completedLen = ioUringWaitOps(fdpDevice->syncIOUring, 1, &completed);

        assert(completedLen == 1 && completed[0] == op);

        ssize_t bytes = ioUringOpGetResult(op);
        ioUringOpRelease(&op);
        if(bytes < 0){
            return bytes;
        } else{
            ret += bytes;
        }

        offset += bytes;
        data += bytes;
        remainingSize -= bytes;
    }
    return ret;
}

#endif