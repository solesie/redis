#ifndef REDIS_IOURING_DISABLE
#include <pthread.h>
#include "redisassert.h"
#include "zmalloc.h"
#include "fdp_device.h"
#include "fdp_nvme.h"

#define min(a, b) (a) < (b) ? a : b

struct _FdpDevice{
    IOUring *syncIOUring;
    IOUring *asyncIOUring;
    FdpNvme *fdpNvme;

    /* Some devices have this transfer size limit due to DMA size limitations.
     * This limit is applicable for both writes and reads. */
    size_t maxIOSize;

    size_t asyncIOUringQDepth;
    pthread_t asyncIOUringHandlingThread;
};

/* solesie: check whether user change buf member of not */
static inline int isAligned(FdpNvme *fdpNvme, AlignedBuffer *buf){
    size_t lbs = fdpNvmeGetLbSize(fdpNvme);
    if(buf->offset % lbs != 0 || 
        buf->size % lbs != 0 ){
        return 0;
    }
    return 1;
}

static void *handling(void *arg){
    FdpDevice *fdpDevice = (FdpDevice *)arg;
    IOUringOp **outCompleted;
    while(1){
        size_t completedLen = ioUringPollCQ(fdpDevice->asyncIOUring, &outCompleted);
        for(size_t i = 0; i < completedLen; ++i){
            IOUringOp *op = outCompleted[i];
            ssize_t res = ioUringOpGetResult(op);
            ioUringOpRelease(&op);
            if(res != 0){
                errno = res;
                printf("solesie: error handling code should be included");
            }
        }
    }
    return NULL;
}

/*
 * solesie: Creates FDP(Flexible Data Placement) device object.
 * 
 * @note For multithreaded applications, it is necessary to have 1 FdpDevice per thread.
 */
FdpDevice *fdpDeviceCreate(FdpNvme *fdpNvme, size_t asyncIOUringQDepth){
    FdpDevice *fdpDevice = zcalloc(sizeof(*fdpDevice));
    
    fdpDevice->syncIOUring = ioUringCreate(1, CQ_SYNC, 1);
    fdpDevice->asyncIOUringQDepth = asyncIOUringQDepth;

    fdpDevice->fdpNvme = fdpNvme;

    fdpDevice->maxIOSize = fdpNvmeGetMaxIOSize(fdpNvme);

    if(asyncIOUringQDepth != 0){
        fdpDevice->asyncIOUring = ioUringCreate(1, CQ_ASYNC, asyncIOUringQDepth);
        pthread_create(&fdpDevice->asyncIOUringHandlingThread, NULL, handling, fdpDevice);
    }

    return fdpDevice;
}

void fdpDeviceRelease(FdpDevice *fdpDevice){
    pthread_detach(fdpDevice->asyncIOUringHandlingThread);
    ioUringRelease(&fdpDevice->syncIOUring);
    ioUringRelease(&fdpDevice->asyncIOUring);
    fdpNvmeRelease(fdpDevice->fdpNvme);
    zfree(fdpDevice);
}

ssize_t fdpDeviceWriteSync(FdpDevice *fdpDevice, AlignedBuffer *buf, int placementHandle){
    assert(isAligned(fdpDevice->fdpNvme, buf));

    int pid = placementHandle;
    if (pid < 0 && pid >= fdpNvmeGetMaxPIDLength(fdpDevice->fdpNvme)) {
        pid = -1;
    }

    size_t remainingSize = buf->size;
    ssize_t ret = 0;
    uint8_t *data = (uint8_t *)buf->ptr;
    off_t offset = buf->offset;
    while(remainingSize > 0){
        size_t writeSize = min(fdpDevice->maxIOSize, remainingSize);

        IOUringOp *op = ioUringOpCreate(fdpDevice->syncIOUring);
        struct io_uring_sqe *sqe = ioUringOpGetSqe(op);
        IOUringOp **completed;

        fdpNvmePrepWriteUringCmdSqe(fdpDevice->fdpNvme, sqe, data, writeSize, offset, pid);
        ioUringSubmitOp(fdpDevice->syncIOUring, op);
        size_t completedLen = ioUringWaitOps(fdpDevice->syncIOUring, 1, &completed);

        assert(completedLen == 1 && completed[0] == op);

        ssize_t res = ioUringOpGetResult(op);
        ioUringOpRelease(&op);
        if(res != 0){
            errno = res;
            return ret;
        }
        ret += writeSize;

        offset += writeSize;
        data += writeSize;
        remainingSize -= writeSize;
    }
    return ret;
}

ssize_t fdpDeviceReadSync(FdpDevice *fdpDevice, AlignedBuffer *buf){
    assert(isAligned(fdpDevice->fdpNvme, buf));

    size_t remainingSize = buf->size;
    ssize_t ret = 0;
    uint8_t *data = (uint8_t *)buf->ptr;
    off_t offset = buf->offset;
    while(remainingSize > 0){
        size_t readSize = min(fdpDevice->maxIOSize, remainingSize);
        
        IOUringOp *op = ioUringOpCreate(fdpDevice->syncIOUring);
        struct io_uring_sqe *sqe = ioUringOpGetSqe(op);
        IOUringOp **completed;

        fdpNvmePrepReadUringCmdSqe(fdpDevice->fdpNvme, sqe, data, readSize, offset);
        ioUringSubmitOp(fdpDevice->syncIOUring, op);
        size_t completedLen = ioUringWaitOps(fdpDevice->syncIOUring, 1, &completed);

        assert(completedLen == 1 && completed[0] == op);

        ssize_t res = ioUringOpGetResult(op);
        ioUringOpRelease(&op);
        if(res != 0){
            errno = res;
            return ret;
        }
        ret += readSize;

        offset += readSize;
        data += readSize;
        remainingSize -= readSize;
    }
    return ret;
}

ssize_t fdpDeviceWriteAsync(FdpDevice *fdpDevice, AlignedBuffer *buf, int placementHandle){
    assert(isAligned(fdpDevice->fdpNvme, buf));

    int pid = placementHandle;
    if (pid < 0 && pid >= fdpNvmeGetMaxPIDLength(fdpDevice->fdpNvme)) {
        pid = -1;
    }

    size_t remainingSize = buf->size;
    ssize_t ret = 0;
    uint8_t *data = (uint8_t *)buf->ptr;
    off_t offset = buf->offset;
    while(remainingSize > 0){
        size_t writeSize = min(fdpDevice->maxIOSize, remainingSize);

        IOUringOp *op = ioUringOpCreate(fdpDevice->asyncIOUring);
        struct io_uring_sqe *sqe = ioUringOpGetSqe(op);

        fdpNvmePrepWriteUringCmdSqe(fdpDevice->fdpNvme, sqe, data, writeSize, offset, pid);
        while(!ioUringSubmitOp(fdpDevice->asyncIOUring, op)){};
        
        ret += writeSize;

        offset += writeSize;
        data += writeSize;
        remainingSize -= writeSize;
    }
    return ret;
}

#endif