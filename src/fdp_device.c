#ifndef REDIS_IOURING_DISABLE
#include <pthread.h>
#include <error.h>
#include "atomicvar.h"
#include "redisassert.h"
#include "zmalloc.h"
#include "fdp_device.h"
#include "fdp_nvme.h"

#define min(a, b) (a) < (b) ? a : b

typedef enum _CurIOType {
    NONE = 0,
    READ,
    WRITE
} CurIOType;

struct _FdpDevice{
    IOUring *uring;
    FdpNvme *fdpNvme;

    /* Some devices have this transfer size limit due to DMA size limitations.
     * This limit is applicable for both writes and reads. */
    size_t maxIOSize;

    uint32_t resubmitLimit;

    size_t curSubmittedCnt;
    size_t curCompletedCnt;
    size_t curSuccessCnt;
    atomic_int waitCalled;
    atomic_int waitCompleted;
    int curIORes;
    CurIOType curIOType;

    pthread_t tid;
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
    /* solesie: CQ handling start */
    while(1){
        size_t completedLen = ioUringPollCQ(fdpDevice->uring, &outCompleted);
        for(size_t i = 0; i < completedLen; ++i){
            IOUringOp *op = outCompleted[i];
            ssize_t res = ioUringOpGetResult(op);
            if(res != 0){
                if(ioUringOpGetResubmitted(op) < fdpDevice->resubmitLimit){
                    while(!ioUringSubmitOp(fdpDevice->uring, op)){};
                    ioUringOpIncreaseResubmitted(op);
                } else{
                    ioUringOpRelease(&op);
                    errno = res;
                    fdpDevice->curIORes = res;
                    fdpDevice->curCompletedCnt++;
                }
            } else{
                ioUringOpRelease(&op);
                fdpDevice->curCompletedCnt++;
                fdpDevice->curSuccessCnt++;
            }
        }

        int waitCalled = atomic_load_explicit(&fdpDevice->waitCalled, memory_order_acquire);
        if(waitCalled && fdpDevice->curCompletedCnt == fdpDevice->curSubmittedCnt){
            if(fdpDevice->curSuccessCnt == fdpDevice->curSubmittedCnt){
                fdpDevice->curIORes = 1;
            } else{
                assert(fdpDevice->curIORes < 0);
            }
            atomic_store_explicit(&fdpDevice->waitCalled, 0, memory_order_release);

            /* solesie: IO thread(i.e., main thread) will be waked up. */
            atomic_store_explicit(&fdpDevice->waitCompleted, 1, memory_order_release);
        }
    }

    return NULL;
}

/*
 * solesie: Creates FDP(Flexible Data Placement) device object.
 * 
 * @note For multithreaded applications, it is necessary to have 1 FdpDevice per thread.
 */
FdpDevice *fdpDeviceCreate(FdpNvme *fdpNvme, uint32_t resubmitLimit, uint32_t qdepth){
    FdpDevice *fdpDevice = zcalloc(sizeof(*fdpDevice));
    
    fdpDevice->uring = ioUringCreate(1, qdepth);
    fdpDevice->resubmitLimit = resubmitLimit;

    fdpDevice->fdpNvme = fdpNvme;

    fdpDevice->maxIOSize = fdpNvmeGetMaxIOSize(fdpNvme);

    int res = pthread_create(&fdpDevice->tid, NULL, handling, fdpDevice);
    assert(res == 0);
    pthread_detach(fdpDevice->tid);

    return fdpDevice;
}

void fdpDeviceRelease(FdpDevice *fdpDevice){
    ioUringRelease(&fdpDevice->uring);
    fdpNvmeRelease(fdpDevice->fdpNvme);
    zfree(fdpDevice);
}

void fdpDeviceIORead(FdpDevice *fdpDevice, AlignedBuffer *buf){
    assert(isAligned(fdpDevice->fdpNvme, buf));
    assert(fdpDevice->curIOType == NONE || fdpDevice->curIOType == READ);
    if(fdpDevice->curIOType == NONE){
        fdpDevice->curIOType = READ;
    }

    size_t remainingSize = buf->size;
    uint8_t *data = (uint8_t *)buf->ptr;
    off_t offset = buf->offset;

    fdpDevice->curSubmittedCnt += (remainingSize + fdpDevice->maxIOSize - 1) / fdpDevice->maxIOSize;

    while(remainingSize > 0){
        size_t readSize = min(fdpDevice->maxIOSize, remainingSize);
        
        IOUringOp *op = ioUringOpCreate(fdpDevice->uring);
        struct io_uring_sqe *sqe = ioUringOpGetSqe(op);

        fdpNvmePrepReadUringCmdSqe(fdpDevice->fdpNvme, sqe, data, readSize, offset);
        while(!ioUringSubmitOp(fdpDevice->uring, op)){};

        offset += readSize;
        data += readSize;
        remainingSize -= readSize;
    }
    return;
}

void fdpDeviceIOWrite(FdpDevice *fdpDevice, AlignedBuffer *buf, int placementHandle){
    assert(isAligned(fdpDevice->fdpNvme, buf));
    assert(fdpDevice->curIOType == NONE || fdpDevice->curIOType == WRITE);
    if(fdpDevice->curIOType == NONE){
        fdpDevice->curIOType = WRITE;
    }

    int pid = placementHandle;
    if (pid < 0 || pid >= fdpNvmeGetMaxPIDLength(fdpDevice->fdpNvme)) {
        pid = -1;
    }

    size_t remainingSize = buf->size;
    uint8_t *data = (uint8_t *)buf->ptr;
    off_t offset = buf->offset;

    fdpDevice->curSubmittedCnt += (remainingSize + fdpDevice->maxIOSize - 1) / fdpDevice->maxIOSize;
    
    while(remainingSize > 0){
        size_t writeSize = min(fdpDevice->maxIOSize, remainingSize);

        IOUringOp *op = ioUringOpCreate(fdpDevice->uring);
        struct io_uring_sqe *sqe = ioUringOpGetSqe(op);

        fdpNvmePrepWriteUringCmdSqe(fdpDevice->fdpNvme, sqe, data, writeSize, offset, pid);
        while(!ioUringSubmitOp(fdpDevice->uring, op)){};

        offset += writeSize;
        data += writeSize;
        remainingSize -= writeSize;
    }
    return;
}

int fdpDeviceIOWait(FdpDevice *fdpDevice){
    atomic_store_explicit(&fdpDevice->waitCalled, 1, memory_order_release);

    /* solesie: spin lock */
    int waitCompleted = 0;
    while(1){
        waitCompleted = atomic_load_explicit(&fdpDevice->waitCompleted, memory_order_acquire);
        if(waitCompleted == 1){
            break;
        }
    }
    atomic_store_explicit(&fdpDevice->waitCompleted, 0, memory_order_release);
    
    int ret = fdpDevice->curIORes;

    fdpDevice->curSubmittedCnt = 0;
    fdpDevice->curCompletedCnt = 0;
    fdpDevice->curSuccessCnt = 0;
    fdpDevice->curIORes = 0;
    fdpDevice->curIOType = NONE;

    return ret;
}

#endif